#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * temporal-slab - Lifetime-Aware Memory Allocator
 * 
 * A lock-free slab allocator that groups allocations by time to prevent
 * temporal fragmentation. Objects allocated together are placed in the same
 * slab, so when their lifetimes end, the slab can be recycled as a unit.
 * 
 * KEY PROPERTIES:
 * - Lock-free allocation fast path (sub-100ns)
 * - Bounded RSS under sustained churn
 * - O(1) deterministic size class selection
 * - Safe handle validation (no crashes on invalid frees)
 * - No background compaction or relocation
 * 
 * BASIC USAGE:
 *   SlabAllocator* a = slab_allocator_create();
 *   
 *   // Handle-based API (zero overhead)
 *   SlabHandle h;
 *   void* p = alloc_obj(a, 128, &h);
 *   free_obj(a, h);
 *   
 *   // malloc-style API (8-byte overhead per allocation)
 *   void* q = slab_malloc(a, 128);
 *   slab_free(a, q);
 *   
 *   slab_allocator_free(a);
 * 
 * THREAD SAFETY:
 * - All functions are thread-safe
 * - Allocation fast path is lock-free (no mutex contention)
 * - Multiple allocators can be used independently
 * 
 * See docs/foundations.md for design rationale.
 */

/* ==================== Configuration ==================== */

/* Size of each slab (must be page-aligned)
 * Each slab is subdivided into fixed-size slots based on size class.
 * Common page size on x86-64, ARM64, and most modern systems.
 */
#define SLAB_PAGE_SIZE 4096u

/* ==================== Opaque Types ==================== */

/* Opaque allocator handle
 * 
 * Internal structure is hidden to allow implementation changes without
 * breaking API compatibility. Contains per-size-class state including
 * partial/full lists, slab cache, and performance counters.
 * 
 * Create with slab_allocator_create(), destroy with slab_allocator_free().
 */
typedef struct SlabAllocator SlabAllocator;

/* Opaque handle for allocated objects
 * 
 * A handle is a 64-bit value that encodes the location of an allocation.
 * Unlike raw pointers, handles can be safely validated at free time:
 * - Invalid handles return false instead of crashing
 * - Double-frees are detected and rejected
 * - Handles from different allocators are rejected
 * 
 * ENCODING (internal, subject to change):
 *   [63:16] Slab pointer (48 bits, user-space addresses)
 *   [15:8]  Slot index (8 bits, max 255 objects/slab)
 *   [7:0]   Size class (8 bits, currently 0-7)
 * 
 * PROPERTIES:
 * - Zero handle (0x0) is invalid (NULL sentinel)
 * - Handles remain valid for validation even after free
 *   (slabs are never unmapped during allocator lifetime)
 * - Handles are not portable across processes or machines
 * 
 * USE HANDLE API IF:
 * - You need zero overhead (no per-allocation metadata)
 * - You want explicit error handling on invalid frees
 * - You're willing to track handles separately from pointers
 * 
 * USE MALLOC API IF:
 * - You want drop-in malloc/free compatibility
 * - 8 bytes overhead per allocation is acceptable
 */
typedef uint64_t SlabHandle;

/* Performance counters for a single size class
 * 
 * These counters help attribute tail latency and diagnose allocator behavior.
 * All counters are monotonically increasing (never reset).
 * 
 * USAGE:
 *   PerfCounters pc;
 *   get_perf_counters(alloc, 2, &pc);  // size class 2 = 128 bytes
 *   printf("Slow path hits: %lu\n", pc.slow_path_hits);
 * 
 * INTERPRETATION:
 * - High slow_path_hits → contention or frequent slab exhaustion
 * - new_slab_count - empty_slab_recycled → net memory growth
 * - empty_slab_overflowed → cache too small for workload
 */
typedef struct PerfCounters {
  uint64_t slow_path_hits;              /* Total times fast path failed (lock acquired) */
  uint64_t new_slab_count;              /* Total slabs allocated from OS (mmap calls) */
  uint64_t list_move_partial_to_full;   /* Slabs that became completely full */
  uint64_t list_move_full_to_partial;   /* Slabs with at least one free after being full */
  uint64_t current_partial_null;        /* Fast path found no current_partial slab */
  uint64_t current_partial_full;        /* Fast path found full current_partial slab */
  uint64_t empty_slab_recycled;         /* Empty slabs pushed to cache for reuse */
  uint64_t empty_slab_overflowed;       /* Empty slabs pushed to overflow (cache full) */
} PerfCounters;

/* ==================== Allocator Lifetime ==================== */

/* Create a new allocator instance
 * 
 * Allocates and initializes an allocator with 8 size classes:
 * 64, 96, 128, 192, 256, 384, 512, 768 bytes.
 * 
 * RETURNS: Pointer to allocator, or NULL on allocation failure
 * 
 * THREAD SAFETY: The returned allocator is thread-safe for all operations.
 * Multiple threads can allocate/free concurrently from the same allocator.
 * 
 * MEMORY: Allocates ~4KB for allocator structure plus cache storage.
 * Does not pre-allocate slabs (slabs are allocated on first use per class).
 */
SlabAllocator* slab_allocator_create(void);

/* Destroy allocator and release all memory
 * 
 * SAFETY:
 * - All slabs are unmapped (handles become invalid)
 * - Any use of handles after this call is undefined behavior
 * - Does NOT validate that all allocations were freed
 * 
 * THREAD SAFETY: Caller must ensure no concurrent operations on this allocator.
 */
void slab_allocator_free(SlabAllocator* alloc);

/* Initialize allocator in caller-provided storage
 * 
 * For advanced use cases where the allocator structure is embedded in a
 * larger structure or allocated via custom allocator.
 * 
 * EXAMPLE:
 *   SlabAllocator alloc;
 *   allocator_init(&alloc);
 *   // ... use allocator ...
 *   allocator_destroy(&alloc);
 * 
 * Most users should use slab_allocator_create() instead.
 */
void allocator_init(SlabAllocator* alloc);
void allocator_destroy(SlabAllocator* alloc);

/* ==================== Core API (Handle-Based) ==================== */

/* Allocate object with explicit handle
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size       - Requested size in bytes (must be > 0 and <= 768)
 *   out_handle - Output parameter for handle (must not be NULL)
 * 
 * RETURNS:
 *   Pointer to allocated memory (aligned to 8 bytes), or NULL on failure.
 *   On success, *out_handle is set to a valid handle for later freeing.
 * 
 * SIZE CLASS SELECTION:
 *   Size is rounded up to next size class (O(1) lookup):
 *   1-64   → 64    |  65-96   → 96    | 97-128   → 128  | 129-192  → 192
 *   193-256 → 256  |  257-384 → 384   | 385-512  → 512  | 513-768  → 768
 * 
 * FAILURE MODES:
 *   Returns NULL if:
 *   - size == 0 or size > 768
 *   - out_handle == NULL
 *   - System out of memory (mmap fails)
 * 
 * PERFORMANCE:
 *   Fast path: ~70ns (lock-free, no syscalls)
 *   Slow path: ~2-5µs (new slab allocation, mmap call)
 * 
 * THREAD SAFETY: Safe to call concurrently on same allocator.
 */
void* alloc_obj(SlabAllocator* alloc, uint32_t size, SlabHandle* out_handle);

/* Free object by handle
 * 
 * PARAMETERS:
 *   alloc  - Allocator instance (must be same as used for allocation)
 *   handle - Handle returned from alloc_obj()
 * 
 * RETURNS:
 *   true  - Object successfully freed
 *   false - Handle invalid (wrong allocator, double-free, corrupted handle)
 * 
 * VALIDATION:
 *   Checks slab magic number and slot state before freeing.
 *   Invalid handles are rejected safely (no crash).
 * 
 * BEHAVIOR AFTER FREE:
 *   - Memory may be reused immediately by other allocations
 *   - Handle remains invalid (repeated frees will return false)
 *   - Pointer becomes dangling (do not dereference)
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
bool free_obj(SlabAllocator* alloc, SlabHandle handle);

/* ==================== Malloc-Style API ==================== */

/* Allocate memory (malloc-compatible interface)
 * 
 * Convenience wrapper around alloc_obj() that hides handle management.
 * Stores the handle in an 8-byte header before the returned pointer.
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   size  - Requested size in bytes (must be > 0 and <= 760)
 * 
 * RETURNS:
 *   Pointer to usable memory, or NULL on failure.
 *   The returned pointer is 8 bytes after the allocation start (handle is hidden).
 * 
 * OVERHEAD:
 *   8 bytes per allocation (handle storage in header)
 *   Max usable size: 760 bytes (768 - 8 byte header)
 * 
 * EXAMPLE:
 *   void* p = slab_malloc(alloc, 100);  // Actually allocates 128-byte slot
 *   if (!p) { handle_error(); }
 *   memcpy(p, data, 100);
 *   slab_free(alloc, p);
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
void* slab_malloc(SlabAllocator* alloc, size_t size);

/* Free memory allocated by slab_malloc()
 * 
 * PARAMETERS:
 *   alloc - Allocator instance (must match allocation call)
 *   ptr   - Pointer returned by slab_malloc(), or NULL
 * 
 * BEHAVIOR:
 *   Reads handle from 8-byte header before ptr and calls free_obj().
 *   NULL pointers are safely ignored (no-op, like standard free()).
 * 
 * SAFETY:
 *   Invalid pointers are detected and rejected (no crash).
 *   Double-free is detected and ignored.
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
void slab_free(SlabAllocator* alloc, void* ptr);

/* ==================== Instrumentation ==================== */

/* Get performance counters for a size class
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size_class - Size class index (0=64B, 1=96B, 2=128B, ..., 7=768B)
 *   out        - Output buffer for counters (must not be NULL)
 * 
 * USAGE:
 *   PerfCounters pc;
 *   get_perf_counters(alloc, 2, &pc);  // Get stats for 128-byte class
 *   
 *   uint64_t net_slabs = pc.new_slab_count - pc.empty_slab_recycled;
 *   printf("Net memory growth: %lu slabs (%.2f MB)\n",
 *          net_slabs, net_slabs * 4096.0 / 1024 / 1024);
 * 
 * NOTE: Counters are snapshots at call time (not atomic across all fields).
 */
void get_perf_counters(SlabAllocator* alloc, uint32_t size_class, PerfCounters* out);

/* ==================== Utilities ==================== */

/* Calculate how many objects fit in a slab for given size
 * 
 * Accounts for slab header and bitmap overhead.
 * Useful for capacity planning and understanding memory layout.
 * 
 * EXAMPLE:
 *   uint32_t count = slab_object_count(128);  // Returns 31
 *   printf("A 128-byte slab holds %u objects\n", count);
 */
uint32_t slab_object_count(uint32_t obj_size);

/* Read process resident set size in bytes
 * 
 * PLATFORM: Linux only (reads /proc/self/statm)
 * RETURNS: RSS in bytes, or 0 if not supported or read fails
 * 
 * USAGE:
 *   uint64_t before = read_rss_bytes_linux();
 *   // ... allocate many objects ...
 *   uint64_t after = read_rss_bytes_linux();
 *   printf("RSS grew by %.2f MB\n", (after - before) / 1024.0 / 1024.0);
 * 
 * NOTE: RSS includes all mapped pages, not just allocator memory.
 */
uint64_t read_rss_bytes_linux(void);

/* Get monotonic time in nanoseconds
 * 
 * Uses CLOCK_MONOTONIC (never decreases, unaffected by time adjustments).
 * Useful for measuring allocation latency.
 * 
 * EXAMPLE:
 *   uint64_t t0 = now_ns();
 *   void* p = alloc_obj(alloc, 128, &h);
 *   uint64_t t1 = now_ns();
 *   printf("Allocation took %lu ns\n", t1 - t0);
 */
uint64_t now_ns(void);

#endif /* SLAB_ALLOC_H */
