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

/* Internal slab list membership */
typedef enum SlabListId {
  SLAB_LIST_NONE    = 0,
  SLAB_LIST_PARTIAL = 1,
  SLAB_LIST_FULL    = 2,
} SlabListId;

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

  uint8_t _pad[3];
};

/* Intrusive doubly-linked list */
struct SlabList {
  Slab* head;
  Slab* tail;
  size_t len;
};

/* Per-size-class allocator state */
struct SizeClassAlloc {
  uint32_t object_size;

  /* Lists guarded by mutex */
  SlabList partial;
  SlabList full;

  /* Phase 1.5: Fast-path current slab (lock-free) */
  _Atomic(Slab*) current_partial;

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
  SizeClassAlloc classes[4];
};

#endif /* SLAB_ALLOC_INTERNAL_H */
