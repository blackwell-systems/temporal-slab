#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * temporal-slab - Lifetime-Aware Memory Allocator
 * 
 * A specialized slab allocator that groups allocations by time to prevent
 * temporal fragmentation. Objects allocated together are placed in the same
 * slab, so when their lifetimes end, the slab can be recycled as a unit.
 * 
 * DESIGN GOAL:
 * Eliminate latency variance and RSS drift in churn-heavy workloads with
 * fixed-size allocation patterns. Not a general-purpose malloc replacement.
 * 
 * KEY PROPERTIES:
 * - Lock-free allocation fast path (sub-100ns median, <2µs p99)
 * - Bounded RSS under sustained churn (2.4% growth vs 20-50% for malloc)
 * - O(1) deterministic size class selection (no branching jitter)
 * - Safe handle validation (invalid frees return false, never crash)
 * - No background compaction or relocation (no latency spikes)
 * 
 * TRADE-OFFS:
 * - Fixed size classes only (64-768 bytes in 8 classes)
 * - 11.1% internal fragmentation (vs ~5-10% for jemalloc)
 * - No NUMA awareness (single allocator for all threads)
 * - Compared to jemalloc: sacrifices generality for deterministic behavior
 * 
 * IDEAL WORKLOADS:
 * - High-frequency trading (HFT) - sub-100ns deterministic allocation
 * - Session stores - millions of alloc/free per second
 * - Cache metadata - bounded RSS under continuous eviction
 * - Connection tracking - predictable latency under load
 * - Packet buffers - fixed sizes, high churn
 * 
 * NOT SUITABLE FOR:
 * - Variable-size allocations (use jemalloc/tcmalloc)
 * - Large objects >768 bytes (use general-purpose allocator)
 * - Drop-in malloc replacement (use jemalloc with LD_PRELOAD)
 * 
 * BASIC USAGE:
 *   SlabAllocator* a = slab_allocator_create();
 *   
 *   // Handle-based API (zero overhead, explicit control)
 *   SlabHandle h;
 *   void* p = alloc_obj(a, 128, &h);
 *   free_obj(a, h);
 *   
 *   // malloc-style API (8-byte overhead, familiar interface)
 *   void* q = slab_malloc(a, 128);
 *   slab_free(a, q);
 *   
 *   slab_allocator_free(a);
 * 
 * THREAD SAFETY:
 * - All functions are thread-safe
 * - Allocation fast path is lock-free (no mutex contention)
 * - Scales linearly to ~4 threads (cache coherence limits beyond 8)
 * - Multiple allocator instances are independent
 * 
 * PERFORMANCE:
 * See docs/results.md for detailed benchmarks and charts.
 * Quick summary: 70ns p50, 1.7µs p99, 2.4% RSS growth over 1000 churn cycles.
 * 
 * DESIGN RATIONALE:
 * See docs/foundations.md for first-principles explanation of temporal
 * fragmentation, entropy, and lifetime-aware allocation strategies.
 */

/* ==================== Configuration ==================== */

/* Size of each slab (must be page-aligned)
 * Each slab is subdivided into fixed-size slots based on size class.
 * Common page size on x86-64, ARM64, and most modern systems.
 */
#define SLAB_PAGE_SIZE 4096u

/* ==================== Epoch Management ==================== */

/* Epoch ID for temporal grouping
 * 
 * Objects allocated in the same epoch are grouped into the same slabs,
 * enabling efficient reclamation when the epoch expires.
 * 
 * PROPERTIES:
 * - Epochs are numbered 0..N-1 (ring buffer, N=16 by default)
 * - Epoch 0 is the default for backward compatibility
 * - Epochs advance via epoch_advance() call
 * - Closed epochs drain naturally (no forced compaction)
 * 
 * LIFECYCLE:
 *   uint32_t e0 = epoch_current(alloc);  // Returns active epoch
 *   void* p = alloc_obj_epoch(alloc, 128, e0, &h);
 *   
 *   epoch_advance(alloc);  // Rotate to next epoch
 *   
 *   uint32_t e1 = epoch_current(alloc);  // New allocations go here
 *   void* q = alloc_obj_epoch(alloc, 128, e1, &h2);
 * 
 * USE CASES:
 * - Session stores: allocate request objects in same epoch, reclaim when request completes
 * - Cache entries: group by insertion time, evict entire epochs
 * - Message queues: separate producer epochs, batch-free on consumer drain
 */
typedef uint32_t EpochId;

/* Epoch lifecycle state for observability
 * 
 * Epochs transition through two states:
 * - ACTIVE:  Accepting new allocations
 * - CLOSING: No new allocations, objects draining naturally
 */
typedef enum EpochLifecycleState {
  EPOCH_ACTIVE  = 0,
  EPOCH_CLOSING = 1,
} EpochLifecycleState;

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
 * All counters are monotonically increasing (never reset during allocator lifetime).
 * 
 * USAGE:
 *   PerfCounters pc;
 *   get_perf_counters(alloc, 2, &pc);  // size class 2 = 128 bytes
 *   printf("Slow path hits: %lu\n", pc.slow_path_hits);
 * 
 * DIAGNOSTIC PATTERNS:
 * 
 * 1. Memory Growth:
 *    net_slabs = new_slab_count - empty_slab_recycled
 *    RSS_growth_MB = net_slabs * 4096 / 1024 / 1024
 *    If net_slabs keeps growing: workload not reaching steady state
 * 
 * 2. Cache Effectiveness:
 *    recycle_rate = empty_slab_recycled / (empty_slab_recycled + empty_slab_overflowed)
 *    If recycle_rate < 95%: cache too small, increase capacity
 * 
 * 3. Slow Path Frequency:
 *    slow_path_ratio = slow_path_hits / total_allocations
 *    If ratio > 5%: current_partial churn too high, indicates contention
 * 
 * 4. Slab Lifecycle Health:
 *    If list_move_partial_to_full >> list_move_full_to_partial:
 *      Slabs filling up but not emptying (lifetime mismatch or leak)
 * 
 * RED FLAGS:
 * - empty_slab_overflowed > 0: Cache too small or memory leak
 * - new_slab_count - empty_slab_recycled > 10K: RSS will be >40MB for this class
 * - slow_path_hits > 10% of allocations: Fast path not effective
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
 * INITIALIZATION:
 * - Builds O(1) class lookup table (768 bytes, once per process)
 * - Allocates per-size-class structures (~4KB total)
 * - Initializes slab cache (32 slots × 8 classes = 256 slab capacity)
 * - Does NOT pre-allocate slabs (allocated on first use per class)
 * 
 * LIFETIME:
 * - Create once per subsystem (e.g., one for sessions, one for cache)
 * - Share across threads (lock-free fast path scales to ~4 threads)
 * - Destroy when subsystem shuts down
 * 
 * TYPICAL PATTERN:
 *   // At service startup
 *   SlabAllocator* session_alloc = slab_allocator_create();
 *   SlabAllocator* cache_alloc = slab_allocator_create();
 *   
 *   // During service operation (any thread)
 *   void* session = alloc_obj(session_alloc, 256, &h);
 *   
 *   // At service shutdown
 *   slab_allocator_free(session_alloc);
 *   slab_allocator_free(cache_alloc);
 * 
 * THREAD SAFETY: The returned allocator is thread-safe for all operations.
 * Multiple threads can allocate/free concurrently from the same allocator.
 */
SlabAllocator* slab_allocator_create(void);

/* Destroy allocator and release all memory
 * 
 * Unmaps all slabs and releases allocator structure. All handles become invalid.
 * 
 * PRECONDITIONS:
 * - No threads are concurrently using this allocator
 * - Caller has finished all operations on allocated objects
 * 
 * BEHAVIOR:
 * - Unmaps all slabs (partial, full, cached, overflowed)
 * - Frees allocator structure
 * - Does NOT validate that all objects were freed (leaks are caller's responsibility)
 * - Does NOT zero memory (for security-sensitive data, zero before freeing)
 * 
 * SAFETY:
 * - All handles become invalid (dereferencing them is undefined behavior)
 * - Pointers returned by alloc_obj/slab_malloc become dangling
 * - Any use after destroy may crash or corrupt memory
 * 
 * TYPICAL CLEANUP PATTERN:
 *   // Free all known allocations first
 *   for (each tracked handle) {
 *     free_obj(alloc, handle);
 *   }
 *   
 *   // Then destroy allocator
 *   slab_allocator_free(alloc);
 *   alloc = NULL;  // Prevent use-after-free
 * 
 * MEMORY LEAK DETECTION:
 * Use get_perf_counters() before destroy to check for leaks:
 *   PerfCounters pc;
 *   get_perf_counters(alloc, class_idx, &pc);
 *   uint64_t net_slabs = pc.new_slab_count - pc.empty_slab_recycled;
 *   if (net_slabs > expected) { potential_leak(); }
 * 
 * THREAD SAFETY: Caller must ensure no concurrent operations.
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

/* ==================== Core API (Epoch-Aware, Handle-Based) ==================== */

/* Allocate object in specific epoch with explicit handle
 * 
 * This is the core allocation API with zero per-allocation overhead.
 * Objects allocated in the same epoch are grouped into the same slabs.
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size       - Requested size in bytes (must be > 0 and <= 768)
 *   epoch      - Epoch ID (must be < epoch_count)
 *   out_handle - Output parameter for handle (must not be NULL)
 * 
 * RETURNS:
 *   Pointer to allocated memory (aligned to 8 bytes), or NULL on failure.
 *   On success, *out_handle is set to a valid handle for later freeing.
 * 
 * EPOCH SEMANTICS:
 *   Objects allocated in the same epoch share slabs. When an epoch's
 *   objects are freed, slabs can be recycled efficiently.
 * 
 * SIZE CLASS SELECTION (O(1), deterministic):
 *   Size is rounded up to next size class via lookup table:
 *   1-64   → 64    |  65-96   → 96    | 97-128   → 128  | 129-192  → 192
 *   193-256 → 256  |  257-384 → 384   | 385-512  → 512  | 513-768  → 768
 * 
 * PERFORMANCE (Intel Core Ultra 7, 128-byte objects):
 *   Fast path: ~70ns median (lock-free CAS loop)
 *   Slow path: ~2-5µs (new slab allocation via mmap)
 * 
 * THREAD SAFETY: Safe to call concurrently on same allocator.
 */
void* alloc_obj_epoch(SlabAllocator* alloc, uint32_t size, EpochId epoch, SlabHandle* out_handle);

/* Free object by handle
 * 
 * PARAMETERS:
 *   alloc  - Allocator instance (must be same as used for allocation)
 *   handle - Handle returned from alloc_obj_epoch()
 * 
 * RETURNS:
 *   true  - Object successfully freed
 *   false - Handle invalid (wrong allocator, double-free, corrupted handle)
 * 
 * EPOCH HANDLING:
 *   Uses slab's stored epoch_id to look up correct epoch state.
 *   Frees work across epoch boundaries (can free old epoch objects).
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
bool free_obj(SlabAllocator* alloc, SlabHandle handle);

/* ==================== Malloc-Style API ==================== */

/* Allocate memory in specific epoch (malloc-compatible)
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   size  - Requested size in bytes (must be > 0 and <= 760)
 *   epoch - Epoch ID (must be < epoch_count)
 * 
 * RETURNS:
 *   Pointer to usable memory, or NULL on failure.
 * 
 * OVERHEAD:
 *   8 bytes per allocation (handle storage in header)
 *   Max usable size: 760 bytes (768 - 8 byte header)
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
void* slab_malloc_epoch(SlabAllocator* alloc, size_t size, EpochId epoch);

/* Free memory allocated by slab_malloc_epoch()
 * 
 * PARAMETERS:
 *   alloc - Allocator instance (must match allocation call)
 *   ptr   - Pointer returned by slab_malloc_epoch(), or NULL
 * 
 * BEHAVIOR:
 *   Reads handle from 8-byte header before ptr and calls free_obj().
 *   NULL pointers are safely ignored (no-op, like standard free()).
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

/* ==================== Epoch API ==================== */

/* Get current active epoch
 * 
 * Returns the epoch ID that new allocations will be assigned to.
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
EpochId epoch_current(SlabAllocator* alloc);

/* Advance to next epoch
 * 
 * Rotates the active epoch forward (mod epoch_count).
 * Previous epoch is "closed" - no new allocations, but existing objects remain valid.
 * 
 * BEHAVIOR:
 * - Atomic increment of current_epoch counter
 * - Previous epoch marked CLOSING (no new allocations allowed)
 * - No immediate memory reclamation (epochs drain naturally as objects are freed)
 * - No compaction or relocation (objects never move)
 * 
 * USAGE PATTERN:
 *   // Allocate batch of related objects
 *   EpochId e = epoch_current(alloc);
 *   for (int i = 0; i < batch_size; i++) {
 *     void* p = alloc_obj_epoch(alloc, size, e, &handles[i]);
 *   }
 *   
 *   // Rotate to next epoch for next batch
 *   epoch_advance(alloc);
 * 
 * THREAD SAFETY: Safe to call concurrently (uses atomic increment).
 */
void epoch_advance(SlabAllocator* alloc);

/* Close specific epoch (Phase 2 RSS Reclamation)
 * 
 * Marks an epoch as CLOSING, preventing new allocations and enabling aggressive
 * reclamation when slabs become empty.
 * 
 * BEHAVIOR:
 * - Marks epoch as CLOSING (no new allocations)
 * - Empty slabs in CLOSING epochs are immediately recycled
 * - With ENABLE_RSS_RECLAMATION=1: recycled slabs have physical pages reclaimed via madvise()
 * - Does NOT free live objects (existing objects remain valid)
 * - Does NOT compact or relocate (objects never move)
 * 
 * USE CASES:
 * - Request completion: close epoch when HTTP request finishes
 * - Frame boundaries: close epoch at end of game frame
 * - Batch expiration: close epoch when batch processing completes
 * - Session timeout: close epoch when user session expires
 * 
 * EXAMPLE:
 *   // Allocate request-scoped objects
 *   EpochId req_epoch = epoch_current(alloc);
 *   void* session = alloc_obj_epoch(alloc, sizeof(Session), req_epoch, &h1);
 *   void* buffer = alloc_obj_epoch(alloc, 4096, req_epoch, &h2);
 *   
 *   process_request(session, buffer);
 *   
 *   // Free request objects
 *   free_obj(alloc, h1);
 *   free_obj(alloc, h2);
 *   
 *   // Close epoch - slabs now empty, RSS can drop
 *   epoch_close(alloc, req_epoch);
 * 
 * PHASE 2 RSS RECLAMATION:
 * When combined with ENABLE_RSS_RECLAMATION=1, closing an epoch enables deterministic
 * RSS drops as slabs drain. This is the key differentiator: temporal_slab can return
 * memory at epoch granularity (aligned with application lifetime phases), while
 * traditional allocators can only react to emergent hole patterns.
 * 
 * THREAD SAFETY: Safe to call concurrently (uses atomic store).
 */
void epoch_close(SlabAllocator* alloc, EpochId epoch);

/* Allocate object in specific epoch (handle-based)
 * 
 * Same as alloc_obj(), but allocates into specified epoch for temporal grouping.
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size       - Requested size in bytes
 *   epoch      - Epoch ID (must be < epoch_count)
 *   out_handle - Output parameter for handle
 * 
 * RETURNS:
 *   Pointer to allocated memory, or NULL on failure.
 * 
 * EPOCH VALIDATION:
 *   If epoch >= epoch_count, returns NULL.
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
void* alloc_obj_epoch(SlabAllocator* alloc, uint32_t size, EpochId epoch, SlabHandle* out_handle);

/* Allocate object in specific epoch (malloc-style)
 * 
 * Same as slab_malloc(), but allocates into specified epoch.
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   size  - Requested size in bytes
 *   epoch - Epoch ID (must be < epoch_count)
 * 
 * RETURNS:
 *   Pointer to usable memory, or NULL on failure.
 * 
 * THREAD SAFETY: Safe to call concurrently.
 */
void* slab_malloc_epoch(SlabAllocator* alloc, size_t size, EpochId epoch);

#endif /* SLAB_ALLOC_H */
