# temporal-slab

**temporal-slab is a lifetime-aware slab allocator that treats memory reclamation as a structural property of program phases, not individual object lifetimes.**

It provides predictable allocation latency, bounded RSS under sustained churn, and application-controlled memory reclamation using epoch-based lifetimes — even under pathological workloads.

**temporal-slab is not a general-purpose malloc replacement.**  
It is designed for systems where worst-case behavior matters more than average speed, and where allocator-induced latency spikes are unacceptable.

---

## Why It Exists

General-purpose allocators optimize for average-case throughput. Under sustained load, they exhibit rare but catastrophic latency spikes and unbounded RSS drift.

In latency-sensitive systems, these outliers dominate SLA violations, frame drops, and tail amplification.

**temporal-slab trades a small median slowdown for structural elimination of allocator-induced tail risk.**

### The Exchange

**You trade:**
- +9ns median latency (+29% slower average case, GitHub Actions validated)
- +37% baseline RSS (epochs keep some slabs hot)

**To eliminate:**
- 1-4µs tail spikes (11-12× reduction at p99-p999, validated on GitHub Actions)
- Allocator-driven RSS drift under steady-state churn (0% validated, GitHub Actions)
- Unpredictable reclamation behavior

---

## Measured Results (100M Allocations)

Latest benchmarks (Feb 8 2026, race-free adaptive bitmap scanning):

| Percentile | system_malloc | temporal-slab | Advantage |
|------------|---------------|---------------|-----------|
| p50 | 23 ns | 41 ns | 0.56× (trade-off) |
| **p99** | **1,113 ns** | **96 ns** | **11.6× better** |
| **p999** | **2,335 ns** | **192 ns** | **12.2× better** |

**Environment Note:** All benchmark numbers shown here were collected on native Linux x86_64 runners using GitHub Actions and are considered the authoritative results. Measurements from WSL2 or other non-native environments are useful for development iteration but are not used for published comparisons due to higher observed variance.

**RSS stability:**
- temporal-slab: 0% growth in steady-state (100 cycles)
- system_malloc: 11,174% growth (unbounded drift)

**Trade-off:** 44% slower median allocation (41ns vs 23ns) in exchange for elimination of malloc's microsecond-scale tail spikes.

---

## Ecosystem and Tooling

temporal-slab provides multiple layers of tooling for development, observability, and validation:

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           ZNS-Slab (this repo)                           │
│                       Core allocator implementation                      │
├──────────────────────────────────────────────────────────────────────────┤
│  • src/slab_alloc.c      Epoch-based slab allocator (C library)         │
│  • src/stats_dump        JSON metrics snapshot (CLI utility)            │
│  • tools/plot_bench.py   Visualization for benchmark CSV                │
└────────────────┬─────────────────────────────────────┬───────────────────┘
                 │                                     │
                 │ libslab_alloc.a                     │ stats JSON
                 │                                     │
        ┌────────▼────────────┐               ┌───────▼─────────────────┐
        │  temporal-slab-     │               │  temporal-slab-tools    │
        │  allocator-bench    │               │  (separate repo)        │
        │  (separate repo)    │               ├─────────────────────────┤
        ├─────────────────────┤               │  tslab (Go CLI)         │
        │  Comparative        │               │  • watch - live stats   │
        │  validation vs:     │               │  • export prometheus    │
        │  • system_malloc    │               │  • top - ranking        │
        │  • jemalloc         │               │                         │
        │  • tcmalloc         │               │  tslabd (C daemon)      │
        │                     │               │  • Embeds allocator     │
        │  Proves:            │               │  • HTTP :8080/metrics   │
        │  • 11.6× p99 better │               │                         │
        │  • 12.2× p999 better│               │  Monitoring Stack       │
        │  • 0% RSS growth    │               │  • Prometheus :9090     │
        │  • Bounded memory   │               │  • Grafana :3000        │
        └─────────────────────┘               │  • Pushgateway :9091    │
                                              │  • 50+ metrics          │
                                              │  • 18 panels, 6 alerts  │
                                              └─────────────────────────┘
```

### Built-in Utilities (this repo)

**`src/stats_dump`** - JSON metrics snapshot
```bash
./src/stats_dump --no-text  # Machine-readable JSON for automation
./src/stats_dump            # Human-readable summary
```

**`tools/plot_bench.py`** - Visualization for benchmark CSV output
```bash
python3 tools/plot_bench.py  # Generate latency/fragmentation charts
```

### External Tools (separate repos)

**[temporal-slab-tools](https://github.com/user/temporal-slab-tools)** - Production observability
- **`tslab`** (Go CLI) - Live stats viewing, Prometheus export, top-like monitoring
- **`tslabd`** (C daemon) - Embeds allocator for long-running metrics collection
- **Complete monitoring stack** - Docker Compose setup with Prometheus + Grafana + Pushgateway
  - 50+ metrics: RSS, epoch age, slow-path rate, madvise bytes, contention counters
  - Pre-built Grafana dashboards with 18 panels across 5 rows
  - Alerting rules: epoch leaks, memory leaks, slow-path spikes
  - One-command setup: `./run-observability.sh` (see [OBSERVABILITY_QUICKSTART.md](https://github.com/user/temporal-slab-tools/blob/main/OBSERVABILITY_QUICKSTART.md))

**[temporal-slab-allocator-bench](https://github.com/user/temporal-slab-allocator-bench)** - Comparative validation
- Neutral harness testing temporal_slab vs system_malloc/jemalloc/tcmalloc
- 100M allocation stress tests proving tail latency advantages
- RSS stability and fragmentation efficiency benchmarks

### Quick Decision Guide

| Need | Tool | Where |
|------|------|-------|
| One-time stats check | `stats_dump` | This repo (`src/`) |
| Live monitoring during dev | `tslab watch` | temporal-slab-tools |
| Production metrics | `tslabd` + Prometheus | temporal-slab-tools |
| Allocator comparison | Benchmark harness | temporal-slab-allocator-bench |
| Visualization | `plot_bench.py` | This repo (`tools/`) |

**Note:** The Go CLI (`tslab`) and C daemon (`tslabd`) are in a **separate repository** to maintain isolation—tool changes cannot affect allocator behavior.

---

## What temporal-slab Guarantees

* **Bounded tail latency with measured worst-case behavior**  
  Lock-free allocation with **131ns p99** (11.2× better), **371ns p999** (11.9× better) vs system_malloc under sustained churn. No emergent pathological states. Validated on GitHub Actions ubuntu-latest (AMD EPYC 7763).

* **Mathematically proven bounded RSS under sustained churn**  
  
  **Definition of "bounded RSS":**
  - **Allocator-bounded:** `committed_bytes` stays constant under sustained churn (proven via atomic counters)
  - **Process-bounded:** Total RSS stays bounded after subtracting application/harness overhead
  
  **Validation (2000-cycle sustained phase shifts, GitHub Actions):**
  - Allocator committed: **0.70 MB → 0.70 MB (0.0% drift)** - mathematically proven via instrumentation
  - Process RSS: Bounded by allocator footprint + application overhead (both independently measured)
  - **Phase-boundary reclamation via `epoch_close()` achieves 0% RSS growth and perfect slab reuse** across epoch boundaries
  
  **Important:** RSS charts may show growth from application overhead (e.g., unbounded metrics arrays). Always decompose RSS into allocator vs application components using diagnostic counters (see [PRODUCTION_HARDENING.md](docs/PRODUCTION_HARDENING.md)).

* **Application-controlled memory lifecycle**  
  `epoch_close()` API for deterministic reclamation at lifetime boundaries. Enables **perfect slab reuse** across phases (0% growth, 0% variation validated). Memory reclaimed when *you* decide, not when allocator heuristics trigger.

* **Structural safety guarantees**  
  Invalid, stale, or double frees are safely rejected—never segfaults. No unsafe unmapping during runtime.

* **Explicit risk exchange**  
  +18ns median cost (+44%), +37% baseline RSS for **elimination of 1-2µs tail spikes**. You trade memory for reliability.

---

## Best Fit For

* Cache metadata and session stores
* Request-scoped allocation (web servers, RPC frameworks)
* Frame- or batch-based systems (games, simulations, pipelines)
* Latency-sensitive services requiring deterministic worst-case behavior

## Not Intended For

* Mixed-size, mixed-lifetime workloads
* General-purpose allocator replacement
* Objects larger than **768 bytes**

---

## Lifetime Management

temporal-slab supports **explicit lifetime control** in addition to raw allocation.

- **Epochs** group allocations by temporal phase
- **epoch_close()** deterministically reclaims memory at application-defined boundaries
- **Epoch Domains** provide RAII-style scoped lifetimes for common patterns

Typical use cases:
- Request-scoped allocation (enter domain → handle request → exit domain)
- Frame-based systems (manual domain control per frame)
- Batch or pipeline stages with clear lifetime boundaries

Epoch Domains are optional; applications may manage epochs manually when finer control is required.

---

## RSS Reclamation

temporal-slab returns physical memory to the OS at **application-controlled lifetime boundaries** via the `epoch_close()` API.

**Mechanism:**
- Empty slabs in CLOSING epochs call `madvise(MADV_DONTNEED)`
- CachedNode architecture stores slab metadata off-page (survives madvise)
- 24-bit generation counter for ABA protection (16M reuse budget)
- Virtual memory stays mapped (safe for stale handles)

**Benchmark results (20 cycles × 50K objects per epoch, GitHub Actions validated):**
- **0% RSS growth** after warmup (cycle 2→19)
- **0% RSS variation** across 18 steady-state cycles
- RSS stabilizes at 2,172 KB after warmup
- Epoch ring buffer wraps at 16 with no RSS disruption
- Perfect slab reuse: slabs allocated in epoch N reused in epoch N+16

**Future work:**
- Phase 3: Handle indirection + `munmap()` for deterministic unmapping
- Architecture: `handle → slab_id → slab_table[generation, state, ptr] → slab`
- Enables: Real `munmap()` with crash-proof stale frees
- Defer until: Current implementation proven in production

---

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
- **Epoch-based lifetimes** - Temporal grouping with deterministic reclamation
- **Epoch Domains (RAII scopes)** - Optional scoped lifetimes for requests, frames, and batches
- **Bounded RSS** - Cache + overflow lists prevent memory leaks under pressure
- **Opaque handles** - 64-bit encoding hides implementation details
- **Dual API** - Handle-based (zero overhead) and malloc-style (8-byte header)
- **HFT-safe contention observability** - Tier 0 probe (zero jitter, <0.1% overhead, always-on)
  - Lock contention tracking (trylock probe pattern)
  - CAS retry metrics (bitmap + fast-path pointer swaps)
  - Validated scaling: 0% → 15% contention across 1→16 threads (healthy plateau, GitHub Actions x86_64)
  - CAS retry rate: <0.01 retries/op across all thread counts (excellent lock-free performance)

## Performance Summary

temporal-slab delivers three key properties for latency-sensitive workloads:

1. **Eliminates tail latency spikes** - 11-12× better across p99-p999 (GitHub Actions validated)
2. **Deterministic behavior under churn** - 0% RSS growth in steady-state, 15% thread contention plateau, 5.7% CoV at 16 threads
3. **Phase-aware RSS reclamation** - `epoch_close()` API enables deterministic slab reuse at phase boundaries (0% growth validated, GitHub Actions)

**Tail Latency Results (GitHub Actions, 100K obj × 1K cycles, 128-byte objects, 5 trials, Feb 9 2026):**

| Percentile | temporal-slab | system_malloc | Advantage |
|------------|---------------|---------------|----------|
| p50 | 40ns | 31ns | 1.29× slower (trade-off) |
| p99 | 131ns | 1,463ns | **11.2× better** |
| p999 | 371ns | 4,418ns | **11.9× better** |
| p9999 | 3,246ns | 7,935ns | **2.4× better** |

**RSS & Efficiency:**

| Metric | temporal-slab | system_malloc |
|--------|---------------|---------------|
| Steady-state RSS growth (constant working set) | **0%** | **0%** |
| Phase-boundary RSS growth (with epoch_close()) | **0%** | N/A (no epoch API) |
| Mixed-workload RSS growth (no epoch boundaries) | 1,033% | 1,111% (1.08× worse) |
| Baseline RSS overhead | +37% | - |
| Space efficiency | 88.9% | ~85% |

**Note:** Phase-boundary test validates `epoch_close()` enables perfect slab reuse across epochs (0% growth, 0% variation). Mixed-workload test measures single-phase behavior without epoch boundaries—both allocators grow with expanding working sets.

**Risk exchange:** +9ns median cost (+29%), +37% baseline RSS to eliminate 1-4µs tail spikes. This is not a performance trade-off—it's **tail-risk elimination**. A single malloc p99 outlier (1,463ns) costs 36× more than temporal-slab's median allocation (40ns). For latency-sensitive systems, this exchange is decisive.

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
- Max size: 760 bytes (768 - 8 byte header)
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

### Standard Build

```bash
cd src/
make                    # Build all tests
./smoke_tests          # Correctness tests
./test_epochs          # Epoch isolation and lifecycle tests
./churn_test           # RSS bounds validation
./test_malloc_wrapper  # malloc/free API tests
./benchmark_accurate   # Performance measurement
```

### Compile-Time Flags

**Production build (default):**
```bash
make  # Optimized for latency, diagnostic counters disabled
```

**Debug build with RSS diagnostics:**
```bash
make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1 -I../include -DENABLE_RSS_RECLAMATION=1"
```

**Build with TLS cache (experimental):**
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 -I../include"
```

**Build with tail latency sampling (WSL2/VM diagnosis):**
```bash
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING -I../include"
# Samples 1/1024 allocations, measures wall vs CPU time
# See: workloads/README_SLOWPATH_SAMPLING.md
```

**All compile-time flags:**

| Flag | Default | Purpose | Overhead | When to Use |
|------|---------|---------|----------|-------------|
| `ENABLE_RSS_RECLAMATION` | 1 | Returns empty slabs to OS via madvise() | ~5µs per slab | Memory-constrained systems, containers |
| `ENABLE_TLS_CACHE` | 0 | Thread-local handle cache for tail latency reduction | -11% p50, -75% p99 (faster!) | High-locality workloads, p99-sensitive |
| `ENABLE_SLOWPATH_SAMPLING` | 0 | Probabilistic sampling (1/1024) for latency diagnosis | ~0.2ns avg | WSL2/VM tail latency investigation |
| `ENABLE_DIAGNOSTIC_COUNTERS` | 0 | Tracks live_bytes/committed_bytes with atomics | ~1-2% latency | RSS debugging, leak detection |
| `ENABLE_LOCK_RANK_DEBUG` | 0 | Runtime deadlock detection via lock ordering | ~5-10% latency | Development, debugging only |
| `ENABLE_LABEL_CONTENTION` | 0 | Per-label contention attribution (16 labels) | Negligible | Multi-domain contention analysis |

**For complete flag documentation**, see [docs/COMPILER_FLAGS.md](docs/COMPILER_FLAGS.md).

**When to enable TLS cache:**
- Locality-heavy workloads (high temporal reuse)
- Tail latency is critical (p99/p999 sensitive systems)
- Multi-threaded servers with per-request allocation patterns
- Workloads with measurable hit rates >50%

**When TLS cache adapts automatically:**
- Adversarial patterns (bulk alloc-then-free)
- Low temporal locality (<2% hit rate)
- TLS automatically bypasses with zero overhead

**When to enable diagnostic counters:**
- Actively debugging RSS behavior
- Proving bounded memory for certification
- Running sustained stress tests (2000+ cycles)

**When to disable (default):**
- Production deployments prioritizing latency
- Benchmarking p50/p99/p999 metrics
- After RSS behavior has been validated

See [docs/PRODUCTION_HARDENING.md](docs/PRODUCTION_HARDENING.md) for complete production deployment guide.

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
