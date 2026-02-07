# temporal-slab

temporal-slab is a **high-performance slab allocator** for fixed-size objects, designed for systems that require **predictable latency**, **safe behavior under sustained churn**, and **narrow size distributions**.

The allocator provides a lock-free fast path, conservative recycling guarantees, and explicit safety contracts. It excels in workloads with consistent object sizes (cache metadata, sessions, connection tracking).

**Current status (v0.x):**
- Optimized for single-size or narrow size distribution workloads
- Lock-free allocation with sub-100ns median latency
- Safe handle validation (no crashes on invalid frees)
- Conservative recycling (correctness over aggressive reclamation)
- **Epoch-based temporal grouping** - 16-epoch ring buffer for lifetime separation

**Not yet competitive:**
- Mixed-size, mixed-lifetime workloads (see benchmark results)
- General-purpose allocator replacement

**Roadmap to RSS Competitiveness:**

temporal-slab achieves **RSS stability** (0% growth under steady churn), **competitive normalized efficiency** (RSS/allocated comparable or better than malloc), and **epoch-scoped RSS reclamation** - returning physical memory to the OS at application-controlled lifetime boundaries.

**Three deliberate design choices cause higher absolute RSS:**
1. No runtime `munmap()` - guarantees stale handles are safe
2. Conservative slab retention - cached + overflow slabs stay mapped
3. Size-class rounding - 8-byte headers + fixed size classes

**Path forward (maintaining safety guarantees):**

**Phase 1+2: Epoch-Scoped RSS Reclamation (IMPLEMENTED)**

**Status: Complete** - Critical bug fixed, production safety hardened, validated in benchmarks

**Implementation:**
- Phase 1: `madvise(MADV_DONTNEED)` on empty slabs in CLOSING epochs
- Phase 2: `epoch_close()` API + aggressive reclamation policy
- Critical fix: Overflow slab_id corruption (CachedNode off-page storage)
- Safety improvements: 24-bit generation, 2-bit versioning, memory ordering

**Benchmark Results** (epoch_rss_bench):
- 19.15 MiB marked reclaimable across 5 epochs (4,903 slabs)
- 3.3% overall RSS drop under memory pressure (kernel advisory behavior)
- 100% cache hit rate (perfect slab reuse)
- Pattern: Small epochs approach 100% reclaim, large epochs ~2%

**Key Achievement:**
Returns memory at **epoch granularity** - when application lifetime phases end, not when allocator heuristics decide. Traditional allocators cannot provide this capability.

**Phase 3: Handle Indirection + `munmap()` (Graduate-Level)**
- Architecture: `handle → slab_id → slab_table[generation, state, ptr] → slab`
- Enables: Real `munmap()` with crash-proof stale frees
- Defer until: Phases 1-2 proven in production

**Additional features:**
- Per-class hot slab ring for reduced contention
- Configurable size class sets
- Lazy compaction for nearly-empty slabs

## Why temporal-slab Exists

### Fragmentation as Entropy

In long-running systems, memory fragmentation is not a bug—it is entropy.

Traditional allocators (e.g. malloc) treat memory as a spatial problem: they search for holes large enough to satisfy each allocation request. Over time, this leads to spatial fragmentation: memory resembles "Swiss cheese"—plenty of free space in aggregate, but scattered into unusable fragments. Allocation cost rises, cache locality degrades, and allocator metadata grows.

More subtle—and more damaging—is temporal fragmentation.

In conventional allocators, objects with vastly different lifetimes are often placed adjacent to one another. When a short-lived object is freed, its memory cannot be efficiently reclaimed if neighboring objects are long-lived. Pages accumulate mixed lifetimes and cannot be reused or released cleanly.

temporal-slab addresses this by organizing allocation around temporal affinity instead of spatial holes.

Objects are grouped into slabs based on when they are allocated, implicitly aligning their lifetimes. Rather than searching for holes, allocation proceeds sequentially within a slab. When the objects in a slab expire, the slab becomes empty as a unit and can be safely recycled.

This shifts allocation from a reactive search problem to a predictive layout strategy:

- No hole searching in fragmented address space
- No mixing of unrelated lifetimes within a slab
- Clear, deterministic reuse boundaries

Traditional allocators fight entropy by constantly reordering memory. temporal-slab manages entropy by ensuring objects expire in an organized way.

### The Missing Middle

Existing ZNS systems operate at file or extent granularity. At small object sizes (64–256 bytes), metadata and tracking costs dominate and destroy cache locality.

temporal-slab moves lifetime-aware placement to the allocator layer, where object-scale decisions are cheap and precise. This fills the missing middle between memory allocators and lifetime-aware storage systems.

### Substrate, Not Policy

temporal-slab does not require applications to provide lifetime hints. Lifetime information is implicit in allocation and reuse patterns.

By managing memory at the slab level, the allocator already knows:
- When objects are created
- When groups of objects become unused
- When entire slabs can be safely recycled

This makes temporal-slab a substrate that higher layers can build on, not a policy engine that applications must tune.

**Key outcomes:**
- Bounded RSS under churn (2.2% growth over 1000 cycles)
- Lock-free allocation fast path (70ns median)
- No crashes on stale or invalid frees
- Explicit, documented safety invariants

## Safety Contract

temporal-slab makes the following guarantees:

- **No runtime `munmap()`**  
  Slabs remain mapped for the lifetime of the allocator (no use-after-free faults). Memory is only released in `allocator_destroy()`.

- **Stale handles are safe**  
  `free_obj()` validates slab magic and slot state. Invalid or double frees return `false` and never crash.

- **Conservative recycling**  
  Only slabs that were previously FULL are recycled. PARTIAL slabs are never recycled, preventing use-after-free races.

- **No background compaction or relocation**  
  Objects never move once allocated. No surprise latency spikes from background maintenance.

- **Bounded memory**  
  RSS is bounded by `(partial + full + cache + overflow) = working set`. Cache overflow list prevents unbounded growth.

- **Thread safety**  
  Allocation fast path is lock-free. Slow-path operations use per-size-class mutexes with no global locks.

These guarantees prioritize correctness and observability over aggressive reclamation.

## Quick Start

### Handle-based API (explicit control)

```c
#include <slab_alloc.h>

SlabAllocator* alloc = slab_allocator_create();

SlabHandle h;
void* p = alloc_obj_epoch(alloc, 128, 0, &h);  // Epoch 0 for general use
// use p
free_obj(alloc, h);

slab_allocator_free(alloc);
```

### Malloc-style API (drop-in replacement)

```c
#include <slab_alloc.h>

SlabAllocator* alloc = slab_allocator_create();

void* p = slab_malloc_epoch(alloc, 128, 0);  // Epoch 0 for general use
// use p
slab_free(alloc, p);

slab_allocator_free(alloc);
```

### Epoch-aware API (temporal grouping)

```c
#include <slab_alloc.h>

SlabAllocator* alloc = slab_allocator_create();

// Long-lived backbone data in epoch 0
void* backbone = slab_malloc_epoch(alloc, 128, 0);

// Short-lived session data in rotating epochs
EpochId current = epoch_current(alloc);  // Get active epoch (starts at 0)
void* session = slab_malloc_epoch(alloc, 128, current);

// Advance epoch to "close" previous epoch (ring buffer wraps at 16)
epoch_advance(alloc);

// Next allocations use new epoch
EpochId next = epoch_current(alloc);  // Returns 1
void* new_session = slab_malloc_epoch(alloc, 128, next);

// Free in any order - epoch tracked per object
slab_free(alloc, session);
slab_free(alloc, new_session);
slab_free(alloc, backbone);

slab_allocator_free(alloc);
```

**Build:**
```bash
cd src/
make
./smoke_tests    # Validate correctness
./test_epochs    # Validate epoch isolation and lifecycle
./churn_test     # Validate RSS bounds
```

## Core Design Highlights

- **Lock-free allocation fast path** - Atomic `current_partial` slab pointer, no mutex in common case
- **Lock-free bitmap operations** - CAS loops for slot allocation/freeing within slabs
- **O(1) deterministic class selection** - Lookup table eliminates branching (HFT-critical)
- **O(1) list operations** - Direct list membership tracking via `slab->list_id` (no linear search)
- **FULL-only recycling** - Provably safe empty slab reuse (no race conditions)
- **Bounded RSS** - Cache + overflow lists prevent memory leaks under pressure
- **Opaque handles** - 64-bit encoding hides implementation details
- **Dual API** - Handle-based (zero overhead) and malloc-style (8-byte header)

## Performance Summary

![Performance Overview](docs/images/summary.png)

temporal-slab delivers three key properties for latency-sensitive workloads:

1. **Deterministic allocation latency** - Sub-100ns median, <2µs p99, no jitter
2. **Stable RSS under churn** - 2.4% growth over 1000 cycles (vs 20-50% for malloc/tcmalloc)
3. **Predictable trade-offs** - 11.1% internal fragmentation, zero external fragmentation

**Quick Results (Intel Core Ultra 7, 128-byte objects):**

| Metric | Value | Comparison |
|--------|-------|------------|
| p50 allocation | 30ns | Slightly slower than system_malloc (21ns) |
| p99 allocation | 374ns | **3.3x better than system_malloc** (1,238ns) |
| RSS growth (steady-state churn) | **0%** | Perfect stability over 100 cycles |
| Space efficiency | 88.9% | Reasonable for fixed size classes |
| Memory overhead | +37% | Higher baseline RSS vs system_malloc (21.8 MiB vs 15.9 MiB) |
| Thread scaling | Linear to 4 threads | Lock-free fast path, cache coherence limits beyond 8 threads |

**Important:** temporal-slab does not attempt to outperform general-purpose allocators everywhere. It eliminates latency variance and RSS drift for fixed-size, churn-heavy workloads.

**Full analysis:** See [docs/results.md](docs/results.md) for detailed benchmarks, charts, and interpretation guidelines.

## Size Classes

Fixed size classes optimized for sub-microsecond latency workloads:
- 64, 96, 128, 192, 256, 384, 512, 768 bytes

**Maximum allocation:**
- Handle API: **768 bytes** (no overhead)
- Malloc wrapper: **760 bytes** (768 - 8 byte header for handle storage)

**Internal fragmentation:**
- Average efficiency: **88.9%** across realistic size distribution
- Average waste: **11.1%** (vs. malloc: ~15-25%)

**Class selection:**
- **O(1) deterministic lookup** (no per-class branch overhead)
- 768-byte lookup table (fits in L1 cache)
- Zero jitter from class selection logic

## Architecture

### Alignment Across the Hierarchy

temporal-slab applies a single organizing principle—temporal affinity—across the memory hierarchy:

- Objects are grouped by expected lifetime
- Slabs act as the unit of allocation, reuse, and eventual release
- The same model extends naturally from DRAM to PMEM or CXL memory

This mirrors how zone-based storage groups data by lifetime at larger granularities, but operates at the scale of cache lines and pages.

**temporal-slab is ZNS-inspired rather than ZNS-dependent:** it delivers the benefits of lifetime-aware placement without requiring specific hardware.

### Memory Layout
- **Slab = 4KB page** with header, bitmap, and object slots
- **Bitmap allocation** - Lock-free slot claiming with atomic CAS
- **Intrusive lists** - PARTIAL and FULL lists per size class
- **Slab cache** - 32 pages per size class (128KB total)
- **Overflow list** - Bounded tracking when cache fills

### Lock-Free Fast Path
1. Load `current_partial` pointer (atomic acquire)
2. Allocate slot with CAS on bitmap
3. Return pointer (no locks in common case)

### Recycling Strategy
- **FULL list** - Slabs with zero free slots (never published to `current_partial`)
- **Recycling** - Only from FULL list (provably safe, no races)
- **PARTIAL slabs** - Stay on list when empty, reused naturally
- **Result** - Bounded RSS without recycling race conditions

## API Reference

### Lifecycle
```c
SlabAllocator* slab_allocator_create(void);
void slab_allocator_free(SlabAllocator* alloc);

void allocator_init(SlabAllocator* alloc);    // For custom storage
void allocator_destroy(SlabAllocator* alloc);
```

### Handle-Based API (zero overhead)
```c
void* alloc_obj_epoch(SlabAllocator* alloc, uint32_t size, EpochId epoch, SlabHandle* out_handle);
bool free_obj(SlabAllocator* alloc, SlabHandle handle);
```
- Zero overhead (no hidden headers)
- Explicit handle management
- Returns false on invalid/stale handles
- Epoch parameter for temporal grouping

### Malloc-Style API (8-byte overhead)
```c
void* slab_malloc_epoch(SlabAllocator* alloc, size_t size, EpochId epoch);
void slab_free(SlabAllocator* alloc, void* ptr);
```
- 8-byte header overhead per allocation
- Max size: 504 bytes (512 - 8 byte header)
- NULL-safe: `slab_free(a, NULL)` is no-op
- Epoch parameter for temporal grouping

### Epoch API (temporal grouping)
```c
typedef uint32_t EpochId;

EpochId epoch_current(SlabAllocator* alloc);
void epoch_advance(SlabAllocator* alloc);
```

**Epoch semantics:**
- **16-epoch ring buffer** - EpochIds wrap at 16 (0-15)
- **Temporal grouping** - Objects allocated in same epoch share slabs
- **Thread-safe** - `epoch_advance()` uses atomic increment
- **No forced expiration** - Advancing epoch doesn't free objects
- **Natural drainage** - Old epochs drain as objects are freed

**Use cases:**

**1. Session-based allocation:**
```c
// Allocate long-lived server state in epoch 0
void* server_ctx = slab_malloc_epoch(alloc, 256, 0);

// Rotate epochs every 1000 requests
for (int i = 0; i < 10000; i++) {
  if (i % 1000 == 0) epoch_advance(alloc);
  
  EpochId e = epoch_current(alloc);
  void* req_ctx = slab_malloc_epoch(alloc, 128, e);
  
  // Process request...
  
  slab_free(alloc, req_ctx);
}
```

**2. Cache with lifetime buckets:**
```c
// Separate short-lived from long-lived cache entries
EpochId short_lived_epoch = 1;  // Rotates frequently
EpochId long_lived_epoch = 0;   // Stable

void* hot_entry = slab_malloc_epoch(alloc, 192, short_lived_epoch);
void* cold_entry = slab_malloc_epoch(alloc, 192, long_lived_epoch);
```

**3. Message queue with batching:**
```c
// Each batch gets its own epoch
epoch_advance(alloc);  // Start new batch
EpochId batch = epoch_current(alloc);

for (int i = 0; i < batch_size; i++) {
  void* msg = slab_malloc_epoch(alloc, 256, batch);
  enqueue(msg);
}

// When batch processed, all messages free together
// Slabs from this epoch become empty as a unit
```

**Design rationale:**
- Objects with similar lifetimes → same slab
- When epoch expires → all objects free together → slabs recycle efficiently
- No explicit TTL management required
- Future: `epoch_close()` can enable aggressive recycling of old epochs

### Performance Counters
```c
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;
  uint64_t current_partial_full;
  uint64_t empty_slab_recycled;
  uint64_t empty_slab_overflowed;
} PerfCounters;

void get_perf_counters(SlabAllocator* alloc, uint32_t size_class, PerfCounters* out);
```

## Scope

temporal-slab is intentionally focused on **memory allocation only**.

It provides:
- Fixed-size object allocation
- Lock-free fast paths
- Deterministic reclamation behavior
- Bounded RSS under sustained churn
- Strong safety guarantees (no use-after-free, safe stale handle validation)

It does **not** implement:
- Eviction policies
- Cache logic
- TTL management
- Tiered storage or persistence
- NUMA placement strategies

Higher-level systems (caches, tiered allocators, eviction frameworks) are expected to be built *on top* of temporal-slab in separate projects.


## Limitations

- **Max object size**: 768 bytes (handle API) or 760 bytes (malloc wrapper)
- **Fixed size classes** - Not suitable for arbitrary sizes (by design)
- **No realloc** - Size changes require alloc + copy + free
- **Linux only** - RSS measurement uses /proc/self/statm
- **No NUMA awareness** - Single allocator for all threads

## Use Cases

temporal-slab is designed for subsystems with fixed-size allocation patterns:
- **High-frequency trading (HFT)** - Sub-100ns deterministic allocation, no jitter
- Session stores
- Connection metadata
- Cache entries
- Message queues
- Packet buffers
- Systems that cannot tolerate allocator-induced latency spikes or RSS drift

**Why HFT-ready:**
- O(1) deterministic class selection (no unpredictable branching)
- Lock-free fast path (no mutex contention)
- No background compaction (no surprise latency spikes)
- 88.9% space efficiency (11.1% internal fragmentation)
- 8 size classes cover 48-768 byte range with <25% waste per allocation

**When jemalloc/tcmalloc are better choices:**
- Variable-size allocations (temporal-slab: fixed classes only)
- Objects >768 bytes (temporal-slab: specialized for small objects)
- NUMA systems (temporal-slab: no per-node awareness)
- Drop-in malloc replacement (jemalloc: LD_PRELOAD, huge ecosystem)
- General-purpose workloads (jemalloc: decades of production tuning)

**Core trade-off:** Compared to jemalloc, temporal-slab sacrifices generality in exchange for deterministic latency and bounded RSS under sustained churn.

## Non-Goals

temporal-slab intentionally does not attempt to:
- Replace general-purpose allocators (malloc, jemalloc)
- Support arbitrary object sizes
- Perform background compaction or relocation
- Guess object lifetimes heuristically

It is designed to be predictable, conservative, and explicit—a specialized tool for a specific class of problems.

## Build and Test

```bash
cd src/
make                    # Build all tests
./smoke_tests          # Correctness tests
./test_epochs          # Epoch isolation and lifecycle tests
./churn_test           # RSS bounds validation
./test_malloc_wrapper  # malloc/free API tests
./benchmark_accurate   # Performance measurement
```

## Project Structure

```
include/
  slab_alloc.h           - Public API
src/
  slab_alloc.c           - Implementation
  slab_alloc_internal.h  - Internal structures
  smoke_tests.c          - Correctness tests
  test_epochs.c          - Epoch isolation and lifecycle tests
  churn_test.c           - RSS bounds validation
  test_malloc_wrapper.c  - malloc/free API tests
  benchmark_accurate.c   - Performance measurement
  Makefile               - Build system
```

## Design Principles

1. **Safety over optimization** - FULL-only recycling eliminates races
2. **Explicit contracts** - Never munmap during runtime, bounded RSS
3. **Observable behavior** - Invalid frees return false, never crash
4. **Lock-free fast path** - Atomic operations only, no mutex contention
5. **Bounded resources** - Cache + overflow guarantee RSS limits
