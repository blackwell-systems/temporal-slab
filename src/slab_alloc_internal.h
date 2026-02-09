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
#define SLAB_PAGE_SIZE 4096u  /* Matches x86-64 Linux page size. Must be power of 2
                                * for fast address arithmetic (ptr & ~(PAGE_SIZE-1)).
                                * Larger pages (64KB on some ARM) increase RSS granularity. */
#endif

#define SLAB_MAGIC   0x534C4142u /* "SLAB" in ASCII, used to detect corruption */
#define SLAB_VERSION 1u          /* Handle format version for future compatibility */

/* Epoch configuration */
#define EPOCH_COUNT 16u  /* Ring buffer size. Power of 2 enables fast modulo via bitwise AND.
                          * 16 epochs provides ~16s drain window if epochs rotate every ~1s.
                          * Too small risks premature wraparound; too large holds memory longer. */

/* Slab list membership (partial/full lists, protected by sc->lock)
 *
 * Slabs move between two lists based on free slot availability:
 * - PARTIAL: Has free slots, actively used for allocations
 * - FULL: No free slots, parked until something is freed
 *
 * Conservative recycling safety: Only FULL slabs are recycled when empty.
 * PARTIAL slabs may be held by threads via the lock-free current_partial
 * pointer, so recycling them could cause use-after-free races.
 */
typedef enum SlabListId {
  SLAB_LIST_PARTIAL = 0,  /* Has free slots, may be visible to lock-free path */
  SLAB_LIST_FULL    = 1,  /* No free slots, never published (safe to recycle) */
  SLAB_LIST_NONE    = 2,  /* Not on any list (in cache or being destroyed) */
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

/* Internal slab structure stored at the start of each page.
 *
 * Memory layout example for 128-byte objects in 4KB page:
 * [Slab: 64B][Bitmap: 4B][Padding: 28B][Slots: 31×128B]
 *
 * Header sits at page start so we can recover it quickly from any pointer
 * via address arithmetic: (ptr & ~(PAGE_SIZE-1))
 */
struct Slab {
  /* Intrusive list links for partial/full list membership.
   * Protected by size-class mutex. No separate malloc needed for list nodes. */
  Slab* prev;
  Slab* next;

  /* Slab metadata (immutable after slab creation) */
  _Atomic uint32_t magic;     /* "SLAB" magic, atomic for lock-free validation */
  uint32_t object_size;       /* Size class this slab belongs to (64, 96, 128, etc.) */
  uint32_t object_count;      /* Number of slots (varies by size: 64B→63 slots, 768B→5 slots) */

  /* Atomic free slot counter. Tracks lifecycle transitions:
   * 0→1 (becomes partial), N-1→N (becomes empty), etc.
   * Atomic because lock-free path reads it and concurrent frees update it. */
  _Atomic uint32_t free_count;

  /* Current list membership (PARTIAL/FULL/NONE).
   * Tracked for safety: prevents double-insert, enables clean removal. */
  SlabListId list_id;
  
  /* Cache lifecycle state (ACTIVE/CACHED/OVERFLOWED).
   * Prevents recycling slabs that are still in active use. */
  SlabCacheState cache_state;
  
  /* Epoch this slab belongs to. Objects with similar lifetimes get grouped
   * in the same epoch's slabs so they can drain together without fragmentation. */
  uint32_t epoch_id;
  
  /* Monotonic era counter stamped when slab was created.
   * Helps distinguish "epoch 5 at era 100" from "epoch 5 at era 105" after wraparound.
   * Useful for correlating allocator state with application logs. */
  uint64_t era;
  
  /* Registry ID for portable handle encoding.
   * Handles store this ID instead of raw pointers, enabling validation
   * and ABA protection via generation counters. */
  uint32_t slab_id;
};

/* Slab registry metadata stored off-page.
 *
 * Generation counter provides ABA protection: if a handle references slab_id=42
 * with generation=10, but the registry shows generation=11, the handle is stale.
 * Prevents use-after-free when slabs are recycled and reused.
 */
struct SlabMeta {
  _Atomic(Slab*) ptr;        /* Pointer to slab, NULL if recycled or unmapped */
  _Atomic uint32_t gen;      /* Generation counter, incremented on each reuse */
};

/* Slab registry: central table mapping slab_id to slab pointer + generation.
 * 
 * Handles encode slab_id instead of raw pointers for portability and ABA protection.
 * Registry grows dynamically as slabs are allocated (starts at 128, doubles on growth).
 */
struct SlabRegistry {
  SlabMeta* metas;   /* Array of (pointer, generation) pairs */
  uint32_t cap;      /* Current capacity (grows via realloc) */
  
  /* Simple ID allocator: bump-allocate from next_id, or reuse from free_ids.
   * Free list currently unused (IDs never freed), but infrastructure exists. */
  uint32_t* free_ids;    /* Array of recycled IDs (currently always empty) */
  uint32_t free_count;   /* Number of IDs in free_ids (currently always 0) */
  uint32_t next_id;      /* Next unused ID to allocate */
  
  pthread_mutex_t lock;  /* Protects registry growth and ID allocation */
};

/* Cache entry storing both slab pointer and ID off-page.
 *
 * When a slab is madvised, the kernel zeros its memory, destroying the header.
 * We store slab_id here (off-page) so we can still identify and reuse the slab
 * after madvise. Never read slab_id from the slab header during reuse—it's been zeroed.
 */
struct CachedSlab {
  Slab* slab;        /* Virtual address (still mapped after madvise) */
  uint32_t slab_id;  /* Registry ID, survives madvise */
};

/* Overflow cache node for when array cache fills up.
 * 
 * Like CachedSlab, stores slab_id off-page since madvise zeros the slab header.
 * Forms a doubly-linked list of cached slabs beyond the 32-entry array cache.
 */
typedef struct CachedNode {
  struct CachedNode* prev;
  struct CachedNode* next;
  Slab* slab;        /* Virtual address */
  uint32_t slab_id;  /* Registry ID, survives madvise */
} CachedNode;

/* Intrusive doubly-linked list */
struct SlabList {
  Slab* head;
  Slab* tail;
  size_t len;
};

/* Per-epoch state within a size class.
 * 
 * Each (size_class, epoch) pair has its own slab lists and lock-free pointer.
 * This temporal partitioning keeps objects with similar lifetimes together.
 */
typedef struct EpochState {
  /* Slab lists protected by parent size-class mutex.
   * Allocations scan partial list, move slabs to full list when exhausted. */
  SlabList partial;
  SlabList full;

  /* Lock-free fast-path pointer.
   * Points to a slab on the partial list with likely free slots.
   * Atomic for lock-free loads and CAS updates. */
  _Atomic(Slab*) current_partial;
  
  /* Count of empty slabs on the partial list.
   * Enables O(1) query of reclaimable memory without scanning the list.
   * Incremented when a slab becomes empty, decremented when it gets its first allocation. */
  _Atomic uint32_t empty_partial_count;
} EpochState;

/* Label registry for semantic attribution in observability.
 * Maps small integer IDs (0-15) to label strings like "request" or "batch_42".
 * ID 0 is reserved for unlabeled allocations. */
#define MAX_LABEL_IDS 16

typedef struct LabelRegistry {
  char labels[MAX_LABEL_IDS][32];  /* label_id → label string */
  uint8_t count;                   /* Next available ID (capped at 16) */
  pthread_mutex_t lock;            /* Protects registration, cold path only */
} LabelRegistry;

/* Per-size-class allocator state.
 * 
 * One instance per size class (64B, 96B, 128B, etc.).
 * Manages slab allocation, caching, and recycling for that size.
 */
struct SizeClassAlloc {
  uint32_t object_size;  /* Size class: 64, 96, 128, ... 768 */

  /* Array of per-epoch state (16 epochs).
   * Each epoch has its own partial/full lists and lock-free pointer.
   * Dynamically allocated to keep this struct smaller. */
  EpochState* epochs;

  /* Protects partial/full list mutations and cache operations.
   * Fast path (current_partial) is lock-free; slow path takes this lock. */
  pthread_mutex_t lock;
  
  /* Backpointer to parent allocator.
   * Used in hot path for label_id lookup when ENABLE_LABEL_CONTENTION is on. */
  struct SlabAllocator* parent_alloc;

  size_t total_slabs;  /* Total slabs allocated for this size class (lifetime counter) */

  /* Performance counters answer "why is allocation slow?"
   * All atomic with relaxed ordering—eventual consistency is fine for diagnostics. */
  _Atomic uint64_t slow_path_hits;              /* Lock-free path failed, took slow path */
  _Atomic uint64_t new_slab_count;              /* Created new slab (mmap syscall) */
  _Atomic uint64_t list_move_partial_to_full;   /* Slab exhausted, moved to full list */
  _Atomic uint64_t list_move_full_to_partial;   /* First free in full slab, moved to partial */
  _Atomic uint64_t current_partial_null;        /* Fast path saw NULL (no slab selected yet) */
  _Atomic uint64_t current_partial_full;        /* Fast path saw full slab (race with other thread) */
  
  /* Empty slab recycling: tracks cache hit rate */
  _Atomic uint64_t empty_slab_recycled;         /* Empty slab pushed to cache for reuse */
  _Atomic uint64_t empty_slab_overflowed;       /* Cache full, pushed to overflow list */
  
  /* Slow-path attribution: answers "why did we hit slow path?" */
  _Atomic uint64_t slow_path_cache_miss;        /* Cache empty, needed mmap */
  _Atomic uint64_t slow_path_epoch_closed;      /* Epoch in CLOSING state, allocation rejected */
  
  /* RSS reclamation: how much memory returned to OS via madvise */
  _Atomic uint64_t madvise_calls;               /* Number of madvise(MADV_DONTNEED) calls */
  _Atomic uint64_t madvise_bytes;               /* Total bytes madvised */
  _Atomic uint64_t madvise_failures;            /* madvise() system call failures */
  
  /* Diagnostic counters for RSS analysis (added for sustained_phase_shifts debugging) */
  _Atomic uint64_t committed_bytes;             /* Total bytes mmap'd (slabs * SLAB_PAGE_SIZE) */
  _Atomic uint64_t live_bytes;                  /* Bytes currently allocated to live objects */
  _Atomic uint64_t empty_slabs;                 /* Number of slabs currently empty (all slots free) */
  
  /* Phase 2.1: Epoch-close telemetry */
  _Atomic uint64_t epoch_close_calls;           /* How many times epoch_close() called */
  _Atomic uint64_t epoch_close_scanned_slabs;   /* Total slabs scanned for reclaimable */
  _Atomic uint64_t epoch_close_recycled_slabs;  /* Slabs actually recycled */
  _Atomic uint64_t epoch_close_total_ns;        /* Total time spent in epoch_close() */
  
  /* Lock-free contention: how often does CAS retry?
   * High retry rates indicate thundering herd (adaptive scanning mitigates this). */
  _Atomic uint64_t bitmap_alloc_cas_retries;     /* Bitmap CAS retries during allocation */
  _Atomic uint64_t bitmap_free_cas_retries;      /* Bitmap CAS retries during free */
  _Atomic uint64_t current_partial_cas_failures; /* Failed to update current_partial pointer */
  
  /* Denominators for computing retry rates.
   * Example: bitmap_alloc_cas_retries / bitmap_alloc_attempts = retries per operation */
  _Atomic uint64_t bitmap_alloc_attempts;        /* Total successful allocations */
  _Atomic uint64_t bitmap_free_attempts;         /* Total successful frees */
  _Atomic uint64_t current_partial_cas_attempts; /* Total current_partial CAS attempts */
  
  /* Lock contention: trylock-based probe with ~2ns overhead.
   * Answers "are threads blocking?" without expensive clock syscalls. */
  _Atomic uint64_t lock_fast_acquire;            /* Trylock succeeded immediately */
  _Atomic uint64_t lock_contended;               /* Trylock failed, had to block */
  
#ifdef ENABLE_LABEL_CONTENTION
  /* Phase 2.3: Per-label contention attribution (compile-time optional) */
  _Atomic uint64_t lock_fast_acquire_by_label[MAX_LABEL_IDS];
  _Atomic uint64_t lock_contended_by_label[MAX_LABEL_IDS];
  _Atomic uint64_t bitmap_alloc_cas_retries_by_label[MAX_LABEL_IDS];
  _Atomic uint64_t bitmap_free_cas_retries_by_label[MAX_LABEL_IDS];
#endif

  /* Adaptive bitmap scanning: dynamically switches between sequential and randomized
   * scanning based on CAS retry rate. Reduces thundering herd under high contention.
   * 
   * Controller uses windowed deltas (not lifetime averages) for fast convergence.
   * Triggered by allocation count (not time) to avoid clock syscalls in hot path. */
  struct {
    /* Window endpoints for computing retry rate delta.
     * Controller compares current counters against these to decide mode. */
    _Atomic uint64_t last_attempts;  /* Last snapshot of bitmap_alloc_attempts */
    _Atomic uint64_t last_retries;   /* Last snapshot of bitmap_alloc_cas_retries */

    /* Scanning mode: 0 = sequential (low contention), 1 = randomized (high contention).
     * Threshold: 0.30 retry rate. Dwell time: 3 checks to prevent flapping. */
    _Atomic uint32_t mode;
    uint32_t         dwell_countdown;  /* Decrements each check, mode locked until zero */

    /* Observability: how often does controller run and switch modes? */
    _Atomic uint32_t checks;    /* Total controller invocations */
    _Atomic uint32_t switches;  /* Mode transitions (sequential↔randomized) */

    /* Single-writer guard: ensures only one thread runs controller at a time.
     * CAS-based, lock-free, cheap (~1-2 instructions). */
    _Atomic uint32_t in_check;
  } scan_adapt;

  /* Slab cache: array of recycled slabs to avoid mmap() syscalls.
   * 32 entries per size class. Cache hit rate >97% in benchmarks.
   * Stores (Slab*, slab_id) pairs off-page so IDs survive madvise. */
  CachedSlab* slab_cache;         /* Fixed-size array [32] */
  size_t cache_capacity;          /* Always 32 */
  size_t cache_size;              /* Current fill level (0-32) */
  pthread_mutex_t cache_lock;     /* Protects cache array */
  
  /* Overflow list: doubly-linked list of slabs beyond the 32-entry cache.
   * Unbounded, but typically small. Stores slab_id off-page like array cache. */
  CachedNode* cache_overflow_head;
  CachedNode* cache_overflow_tail;
  size_t cache_overflow_len;      /* Number of nodes in overflow list */
};

/* Epoch metadata for debugging and observability.
 * 
 * Helps answer questions like:
 * - When did this epoch open? (open_since_ns)
 * - How many domains are holding this epoch open? (domain_refcount)
 * - What application phase does this epoch represent? (label)
 * - How much memory was reclaimed when closing? (rss_before_close - rss_after_close)
 */
typedef struct EpochMetadata {
  uint64_t open_since_ns;           /* nanoseconds since boot, set by epoch_advance() */
  _Atomic uint64_t domain_refcount; /* Number of active epoch_domain_enter() calls */
  char label[32];                   /* Human-readable label like "request" or "batch_42" */
  uint8_t label_id;                 /* Compact ID (0=unlabeled, 1-15=registered labels) */
  
  /* RSS snapshots before/after epoch_close() to measure reclamation effectiveness.
   * Both zero if epoch never closed. Delta shows MB returned to OS. */
  uint64_t rss_before_close;
  uint64_t rss_after_close;
} EpochMetadata;

/* Main allocator structure: one per allocator instance.
 * 
 * Manages 8 size classes (64B to 768B), 16 epochs, and global registry.
 * All size classes share the same epoch state for temporal grouping.
 */
struct SlabAllocator {
  SizeClassAlloc classes[8];  /* One per size class: 64, 96, 128, 192, 256, 384, 512, 768 bytes */
  
  /* Global epoch state shared across all size classes.
   * epoch_advance() increments current_epoch and marks old epoch CLOSING. */
  _Atomic uint32_t current_epoch;  /* Ring index (0-15), points to active epoch */
  uint32_t epoch_count;            /* Always EPOCH_COUNT (16) */
  
  /* Per-epoch lifecycle: ACTIVE (0) or CLOSING (1).
   * CLOSING epochs reject new allocations and enable aggressive recycling. */
  _Atomic uint32_t epoch_state[EPOCH_COUNT];
  
  /* Monotonic era counter for observability.
   * Increments on every epoch_advance(), never wraps (64-bit).
   * Each epoch stores its era so we can distinguish "epoch 5 generation 100"
   * from "epoch 5 generation 105" after ring wraparound. */
  _Atomic uint64_t epoch_era_counter;      /* Global counter */
  _Atomic uint64_t epoch_era[EPOCH_COUNT]; /* Era stamp per epoch */
  
  /* Rich metadata per epoch: timestamps, labels, RSS deltas.
   * Used for debugging and correlating with application logs. */
  EpochMetadata epoch_meta[EPOCH_COUNT];
  pthread_mutex_t epoch_label_lock;        /* Protects label writes (cold path) */
  
  /* Global label registry maps strings to compact IDs (0-15).
   * Example: "request" → ID 1, "batch" → ID 2.
   * Enables fast label lookup in hot path for contention attribution. */
  LabelRegistry label_registry;
  
  /* Slab registry maps slab_id to (Slab*, generation) pairs.
   * Enables portable handle encoding and ABA protection for safe recycling. */
  SlabRegistry reg;
};

#endif /* SLAB_ALLOC_INTERNAL_H */
