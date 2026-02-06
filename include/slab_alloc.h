#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * ZNS-Slab Phase 1.5 - Public API
 * 
 * Specialized slab allocator for sub-4KB objects.
 * 
 * USAGE: Stack-allocate SlabAllocator, call allocator_init(), use alloc_obj/free_obj.
 * 
 * IMPORTANT: SlabAllocator internal fields are PRIVATE.
 * Do not access fields directly. Treat as opaque and use provided API only.
 * Internal layout will change in Phase 1.6 (full opaque types).
 */

/* Configuration constants */
#define SLAB_PAGE_SIZE 4096u

/* Opaque allocator - definition in slab_alloc_internal.h */
typedef struct SlabAllocator SlabAllocator;

/* Handle for allocated objects */
typedef struct SlabHandle {
  void* slab;           /* PRIVATE */
  uint32_t slot;        /* PRIVATE */
  uint32_t size_class;  /* PRIVATE */
} SlabHandle;

/* Performance counters snapshot (read-only) */
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;  /* fast path saw NULL current_partial */
  uint64_t current_partial_full;  /* fast path saw full current_partial */
} PerfCounters;

/* -------------------- Core API -------------------- */

/* Initialize allocator - call before use */
void allocator_init(SlabAllocator* alloc);

/* Destroy allocator - frees all resources */
void allocator_destroy(SlabAllocator* alloc);

/* Allocate object, returns pointer or NULL on failure */
void* alloc_obj(SlabAllocator* alloc, uint32_t size, SlabHandle* out_handle);

/* Free object using handle */
bool free_obj(SlabAllocator* alloc, SlabHandle handle);

/* -------------------- Instrumentation -------------------- */

/* Get performance counters for size class index (0-3) */
void get_perf_counters(SlabAllocator* alloc, uint32_t size_class, PerfCounters* out);

/* -------------------- Utilities -------------------- */

/* Calculate objects per slab for given object size */
uint32_t slab_object_count(uint32_t obj_size);

/* Read process RSS in bytes (Linux only, 0 on other platforms) */
uint64_t read_rss_bytes_linux(void);

/* Get monotonic time in nanoseconds */
uint64_t now_ns(void);

#endif /* SLAB_ALLOC_H */
