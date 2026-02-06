#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define SLAB_PAGE_SIZE 4096u

/* Forward declarations */
typedef struct Slab Slab;
typedef struct SlabList SlabList;
typedef struct SizeClassAlloc SizeClassAlloc;

/* Slab list membership */
typedef enum SlabListId {
  SLAB_LIST_NONE = 0,
  SLAB_LIST_PARTIAL = 1,
  SLAB_LIST_FULL = 2,
} SlabListId;

/* Slab structure */
struct Slab {
  Slab* prev;
  Slab* next;
  uint32_t magic;
  uint32_t version;
  uint32_t object_size;
  uint32_t object_count;
  _Atomic uint32_t free_count;
  SlabListId list_id;
  uint8_t _pad[3];
};

/* Handle returned to caller */
typedef struct SlabHandle {
  Slab* slab;
  uint32_t slot;
  uint32_t size_class;
} SlabHandle;

/* List structure */
struct SlabList {
  Slab* head;
  Slab* tail;
  size_t len;
};

/* Performance counters */
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_miss;
} PerfCounters;

/* Size class allocator */
struct SizeClassAlloc {
  uint32_t object_size;
  SlabList partial;
  SlabList full;
  _Atomic(Slab*) current_partial;
  pthread_mutex_t lock;
  size_t total_slabs;
  
  /* Performance counters */
  _Atomic uint64_t slow_path_hits;
  _Atomic uint64_t new_slab_count;
  _Atomic uint64_t list_move_partial_to_full;
  _Atomic uint64_t list_move_full_to_partial;
  _Atomic uint64_t current_partial_miss;
  
  /* Slab cache */
  Slab** slab_cache;
  size_t cache_capacity;
  size_t cache_size;
  pthread_mutex_t cache_lock;
};

/* Main allocator */
typedef struct SlabAllocator {
  SizeClassAlloc classes[4];
} SlabAllocator;

/* Public API */
void allocator_init(SlabAllocator* a);
void allocator_destroy(SlabAllocator* a);
void* alloc_obj(SlabAllocator* a, uint32_t size, SlabHandle* out);
bool free_obj(SlabAllocator* a, SlabHandle h);

/* Utility functions */
uint32_t slab_object_count(uint32_t obj_size);
uint64_t read_rss_bytes_linux(void);
uint64_t now_ns(void);

/* Performance counter access */
void get_perf_counters(SlabAllocator* a, uint32_t size_class, PerfCounters* out);

#endif /* SLAB_ALLOC_H */
