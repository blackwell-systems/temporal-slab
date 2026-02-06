#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * ZNS-Slab Phase 1.6 - Public API
 * 
 * Specialized slab allocator for sub-4KB objects.
 * 
 * USAGE (opaque API): 
 *   SlabAllocator* a = slab_allocator_create();
 *   void* p = alloc_obj(a, size, &handle);
 *   free_obj(a, handle);
 *   slab_allocator_free(a);
 * 
 * SlabAllocator is opaque - internal layout is private and may change.
 */

/* Configuration constants */
#define SLAB_PAGE_SIZE 4096u

/* Opaque allocator - definition in slab_alloc_internal.h */
typedef struct SlabAllocator SlabAllocator;

/* Opaque handle for allocated objects
 * 
 * Handle encoding (64-bit):
 *   [63:16] Slab pointer (48 bits, user-space addresses)
 *   [15:8]  Slot index (8 bits, max 255 objects/slab)
 *   [7:0]   Size class (8 bits, currently 0-3)
 * 
 * Zero handle is invalid (NULL sentinel).
 * Handles remain valid for validation even after free (slabs stay mapped).
 */
typedef uint64_t SlabHandle;

/* Performance counters snapshot (read-only) */
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;  /* fast path saw NULL current_partial */
  uint64_t current_partial_full;  /* fast path saw full current_partial */
  uint64_t empty_slab_recycled;   /* Phase 2: empty slabs pushed to cache */
  uint64_t empty_slab_overflowed; /* Phase 2: empty slabs pushed to overflow (cache full) */
} PerfCounters;

/* -------------------- Lifetime -------------------- */

/* Create/destroy - opaque API for external users */
SlabAllocator* slab_allocator_create(void);
void slab_allocator_free(SlabAllocator* alloc);

/* Init/destroy - for internal use or when caller provides storage */
void allocator_init(SlabAllocator* alloc);
void allocator_destroy(SlabAllocator* alloc);

/* -------------------- Core API -------------------- */

/* Handle-based allocation (low-level, explicit handle management) */
void* alloc_obj(SlabAllocator* alloc, uint32_t size, SlabHandle* out_handle);
bool free_obj(SlabAllocator* alloc, SlabHandle handle);

/* -------------------- Malloc-style API -------------------- */

/* malloc/free wrapper - stores handle in 8-byte header before returned pointer
 * 
 * Usage:
 *   void* p = slab_malloc(alloc, size);
 *   slab_free(alloc, p);
 * 
 * Overhead: 8 bytes per allocation (handle storage)
 * Max usable size: 512 - 8 = 504 bytes (largest size class minus header)
 * 
 * Returns NULL if size > 504 bytes or allocation fails.
 */
void* slab_malloc(SlabAllocator* alloc, size_t size);
void slab_free(SlabAllocator* alloc, void* ptr);

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
