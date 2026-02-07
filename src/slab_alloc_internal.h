#ifndef SLAB_ALLOC_INTERNAL_H
#define SLAB_ALLOC_INTERNAL_H

/*
 * Internal implementation details - NOT part of public API
 * Do not include this file from outside src/
 */

#include <slab_alloc.h>
#include <stdatomic.h>
#include <pthread.h>

/* Internal configuration */
#ifndef SLAB_PAGE_SIZE
#define SLAB_PAGE_SIZE 4096u
#endif

#define SLAB_MAGIC   0x534C4142u /* "SLAB" */
#define SLAB_VERSION 1u

/* Epoch configuration */
#define EPOCH_COUNT 16u  /* Ring buffer size (power of 2 for fast modulo) */

/* Slab list membership (partial/full lists, protected by sc->lock) */
typedef enum SlabListId {
  SLAB_LIST_PARTIAL = 0,
  SLAB_LIST_FULL    = 1,
  SLAB_LIST_NONE    = 2,  /* Not on any list */
} SlabListId;

/* Slab cache state (lifecycle, protected by cache_lock or sc->lock) */
typedef enum SlabCacheState {
  SLAB_ACTIVE      = 0,  /* In use (on partial/full list) */
  SLAB_CACHED      = 1,  /* In slab_cache array */
  SLAB_OVERFLOWED  = 2,  /* In cache_overflow list */
} SlabCacheState;

/* Forward declarations */
typedef struct Slab Slab;
typedef struct SlabList SlabList;
typedef struct SizeClassAlloc SizeClassAlloc;
typedef struct SlabRegistry SlabRegistry;
typedef struct SlabMeta SlabMeta;
typedef struct CachedSlab CachedSlab;

/* Internal slab structure */
struct Slab {
  /* Intrusive list links (protected by size-class mutex) */
  Slab* prev;
  Slab* next;

  /* Metadata */
  _Atomic uint32_t magic;     /* Atomic for lock-free validation */
  uint32_t object_size;
  uint32_t object_count;

  /* free_count: atomic for precise transition detection */
  _Atomic uint32_t free_count;

  /* List membership for O(1) moves (protected by size-class mutex) */
  SlabListId list_id;
  
  /* Cache state for lifecycle tracking */
  SlabCacheState cache_state;
  
  /* Epoch membership for temporal grouping */
  uint32_t epoch_id;
  
  /* Phase 2.2: Era stamping for monotonic observability */
  uint64_t era;  /* Era when slab was created/reused */
  
  /* Registry ID for portable handle encoding + ABA protection */
  uint32_t slab_id;
};

/* Slab registry metadata (generation stored outside page for madvise safety) */
struct SlabMeta {
  _Atomic(Slab*) ptr;        /* NULL if unmapped/unused */
  _Atomic uint32_t gen;      /* Monotonically increases on reuse (ABA protection) */
};

/* Slab registry for portable handle encoding */
struct SlabRegistry {
  SlabMeta* metas;
  uint32_t cap;
  
  /* Simple ID allocator */
  uint32_t* free_ids;
  uint32_t free_count;
  uint32_t next_id;
  
  pthread_mutex_t lock;
};

/* Cache entry storing both slab pointer and ID (survives madvise)
 * 
 * INVARIANT: Any slab that may be madvised must have its reuse metadata
 * stored off-page (slab_id, etc.). Never read slab_id from the slab header
 * on reuse - it may have been zeroed by madvise(MADV_DONTNEED).
 * 
 * Both array cache (CachedSlab) and overflow cache (CachedNode) follow this
 * invariant: (slab*, slab_id) pairs stored off-page, stable across madvise.
 */
struct CachedSlab {
  Slab* slab;
  uint32_t slab_id;
};

/* Overflow cache node (stores slab_id off-page for madvise safety)
 * 
 * CRITICAL: When overflow slabs are madvised, slab_id in slab header is zeroed.
 * We must store slab_id in this node (off-page) to prevent corruption on reuse.
 * This matches the array cache invariant above.
 */
typedef struct CachedNode {
  struct CachedNode* prev;
  struct CachedNode* next;
  Slab* slab;
  uint32_t slab_id;
} CachedNode;

/* Intrusive doubly-linked list */
struct SlabList {
  Slab* head;
  Slab* tail;
  size_t len;
};

/* Per-epoch state within a size class */
typedef struct EpochState {
  /* Lists guarded by parent size-class mutex */
  SlabList partial;
  SlabList full;

  /* Fast-path current slab (lock-free) */
  _Atomic(Slab*) current_partial;
  
  /* Phase 2.1: O(1) reclaimable tracking (avoids O(n) scan in stats) */
  _Atomic uint32_t empty_partial_count;  /* Slabs with free_count == object_count */
} EpochState;

/* Per-size-class allocator state */
struct SizeClassAlloc {
  uint32_t object_size;

  /* Per-epoch state arrays (EPOCH_COUNT entries) */
  EpochState* epochs;  /* Dynamically allocated [EPOCH_COUNT] */

  pthread_mutex_t lock;

  size_t total_slabs;

  /* Performance counters for tail latency attribution */
  _Atomic uint64_t slow_path_hits;
  _Atomic uint64_t new_slab_count;
  _Atomic uint64_t list_move_partial_to_full;
  _Atomic uint64_t list_move_full_to_partial;
  _Atomic uint64_t current_partial_null;  /* fast path saw NULL current_partial */
  _Atomic uint64_t current_partial_full;  /* fast path saw full current_partial */
  
  /* Phase 2: Empty slab recycling counters */
  _Atomic uint64_t empty_slab_recycled;         /* empty slab pushed to cache */
  _Atomic uint64_t empty_slab_overflowed;       /* cache full, pushed to overflow */
  
  /* Phase 2.0: Slow-path attribution counters */
  _Atomic uint64_t slow_path_cache_miss;        /* new_slab needed mmap */
  _Atomic uint64_t slow_path_epoch_closed;      /* allocation rejected (epoch CLOSING) */
  
  /* Phase 2.0: RSS reclamation tracking */
  _Atomic uint64_t madvise_calls;               /* madvise(MADV_DONTNEED) invocations */
  _Atomic uint64_t madvise_bytes;               /* Total bytes passed to madvise */
  _Atomic uint64_t madvise_failures;            /* madvise() returned error */

  /* Slab cache: free page stack to avoid mmap() in hot path */
  CachedSlab* slab_cache;  /* Changed to store (Slab*, slab_id) pairs */
  size_t cache_capacity;
  size_t cache_size;
  pthread_mutex_t cache_lock;
  
  /* Overflow list: bounded tracking of slabs when cache is full
   * Uses CachedNode to store slab_id off-page (survives madvise) */
  CachedNode* cache_overflow_head;
  CachedNode* cache_overflow_tail;
  size_t cache_overflow_len;
};

/* Phase 2.3: Epoch metadata for rich observability */
typedef struct EpochMetadata {
  uint64_t open_since_ns;           /* Timestamp when epoch became ACTIVE (0 if never opened) */
  _Atomic uint64_t domain_refcount; /* Domain scopes (enter/exit tracking) - Phase 2.3 */
  char label[32];                   /* Semantic label (e.g., "request_id:abc", "frame:1234") */
  
  /* Phase 2.4: RSS delta tracking for reclamation quantification */
  uint64_t rss_before_close;        /* RSS snapshot at start of epoch_close() (0 if never closed) */
  uint64_t rss_after_close;         /* RSS snapshot at end of epoch_close() (0 if never closed) */
} EpochMetadata;

/* Main allocator structure */
struct SlabAllocator {
  SizeClassAlloc classes[8];  /* Expanded from 4 to 8 for HFT granularity */
  
  /* Global epoch state */
  _Atomic uint32_t current_epoch;  /* Active epoch for new allocations */
  uint32_t epoch_count;            /* Number of epochs (EPOCH_COUNT) */
  
  /* Per-epoch lifecycle state (ACTIVE=0 or CLOSING=1) */
  _Atomic uint32_t epoch_state[EPOCH_COUNT];
  
  /* Phase 2.2: Monotonic epoch era for observability */
  _Atomic uint64_t epoch_era_counter;  /* Increments on every epoch_advance */
  uint64_t epoch_era[EPOCH_COUNT];     /* Era when each epoch was last activated */
  
  /* Phase 2.3: Rich epoch metadata for debugging */
  EpochMetadata epoch_meta[EPOCH_COUNT];
  pthread_mutex_t epoch_label_lock;  /* Protects label writes (rare, cold path) */
  
  /* Slab registry for portable handle encoding + ABA protection */
  SlabRegistry reg;
};

#endif /* SLAB_ALLOC_INTERNAL_H */
