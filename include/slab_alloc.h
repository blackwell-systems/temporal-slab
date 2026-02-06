#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * ZNS-Slab Phase 1.5 - Public API
 * 
 * A specialized slab allocator for sub-4KB objects with:
 * - Lock-free fast path
 * - Per-size-class slab cache
 * - Performance counter attribution
 * - Sub-100ns median latency
 */

/* Opaque allocator handle - internal structure hidden */
typedef struct SlabAllocator SlabAllocator;

/* Concrete handle returned to caller - must be saved for free */
typedef struct SlabHandle {
  void* _internal[3];  /* Opaque - do not access directly */
} SlabHandle;

/* Performance counters snapshot - read-only */
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;
  uint64_t current_partial_full;
} PerfCounters;

/* Configuration for allocator initialization */
typedef struct SlabConfig {
  uint32_t page_size;           /* Slab page size (default: 4096) */
  uint32_t cache_capacity;      /* Pages per size class (default: 32) */
  uint32_t num_size_classes;    /* Number of size classes (default: 4) */
  const uint32_t* size_classes; /* Array of sizes (default: 64,128,256,512) */
} SlabConfig;

/* -------------------- Core API -------------------- */

/*
 * Get default configuration.
 * Modifiable - caller can override fields before passing to init.
 */
SlabConfig slab_default_config(void);

/*
 * Initialize allocator with given configuration.
 * Returns NULL on allocation failure.
 */
SlabAllocator* slab_allocator_create(const SlabConfig* config);

/*
 * Destroy allocator and free all slabs.
 * Drains cache and releases all memory.
 */
void slab_allocator_destroy(SlabAllocator* alloc);

/*
 * Allocate object of given size.
 * Returns pointer to allocated memory, or NULL on failure.
 * If out_handle is non-NULL, fills it with handle for later free.
 */
void* slab_alloc(SlabAllocator* alloc, uint32_t size, SlabHandle* out_handle);

/*
 * Free previously allocated object using handle.
 * Returns true on success, false if handle is invalid.
 */
bool slab_free(SlabAllocator* alloc, SlabHandle handle);

/* -------------------- Instrumentation -------------------- */

/*
 * Get performance counters for specific size class.
 * size_class is index (0-based), not byte size.
 * Returns false if size_class is out of range.
 */
bool slab_get_counters(SlabAllocator* alloc, uint32_t size_class, PerfCounters* out);

/*
 * Reset performance counters for specific size class.
 * Useful for benchmark phases.
 */
void slab_reset_counters(SlabAllocator* alloc, uint32_t size_class);

/* -------------------- Utilities -------------------- */

/*
 * Get size class index for given object size.
 * Returns -1 if size exceeds largest size class.
 */
int slab_size_class_for(SlabAllocator* alloc, uint32_t size);

/*
 * Get number of objects per slab for given size class.
 * Returns 0 if size_class is out of range.
 */
uint32_t slab_objects_per_slab(SlabAllocator* alloc, uint32_t size_class);

/* -------------------- Platform-Specific -------------------- */

/*
 * Read RSS (Resident Set Size) in bytes.
 * Linux-only - returns 0 on other platforms.
 */
uint64_t slab_read_rss_bytes(void);

/*
 * Get current time in nanoseconds (monotonic clock).
 */
uint64_t slab_now_ns(void);

#endif /* SLAB_ALLOC_H */
