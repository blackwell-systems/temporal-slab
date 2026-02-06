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

/* Epoch lifecycle state */
typedef enum EpochLifecycleState {
  EPOCH_ACTIVE  = 0,  /* Accepting new allocations */
  EPOCH_CLOSING = 1,  /* No new allocations, draining only */
} EpochLifecycleState;

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
  
  uint8_t _pad[2];
};

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
  _Atomic uint64_t empty_slab_cache_overflowed; /* cache full, pushed to overflow */

  /* Slab cache: free page stack to avoid mmap() in hot path */
  Slab** slab_cache;
  size_t cache_capacity;
  size_t cache_size;
  pthread_mutex_t cache_lock;
  
  /* Overflow list: bounded tracking of slabs when cache is full */
  SlabList cache_overflow;
};

/* Main allocator structure */
struct SlabAllocator {
  SizeClassAlloc classes[8];  /* Expanded from 4 to 8 for HFT granularity */
  
  /* Global epoch state */
  _Atomic uint32_t current_epoch;  /* Active epoch for new allocations */
  uint32_t epoch_count;            /* Number of epochs (EPOCH_COUNT) */
  
  /* Per-epoch lifecycle state (ACTIVE=0 or CLOSING=1) */
  _Atomic uint32_t epoch_state[EPOCH_COUNT];
};

#endif /* SLAB_ALLOC_INTERNAL_H */
