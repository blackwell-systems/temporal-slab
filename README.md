# temporal-slab

**Part of the [Drainability Project](https://github.com/blackwell-systems/drainability)** – Theory, measurement tools, and validation for structural memory leaks

**Research implementation validating the drainability framework for coarse-grained memory reclamation.**

temporal-slab is a lifetime-aware slab allocator that demonstrates epoch-based allocation routing can achieve bounded RSS under sustained churn by enforcing structural drainability. It provides deterministic memory reclamation at application-defined phase boundaries.

**Project Status:** Research prototype. Successfully validated drainability theory, leading to the publication *"Drainability: When Coarse-Grained Memory Reclamation Produces Bounded Retention"* (Blackwell, 2026).

---

## Research Context

This allocator was developed to validate a theoretical prediction: that allocation *routing* (which granule receives an allocation), not allocator *policy* (slab sizes, cache tuning), determines whether coarse-grained memory reclamation produces bounded or unbounded retention.

**Result:** The drainability framework paper establishes that:
- Lifetime-aligned routing → O(1) bounded RSS (validated: 66.5% recycle rate)
- Lifetime-mixed routing → Ω(t) unbounded growth (validated: 0.28% recycle rate)
- 238× differential in recycle rates under identical allocator, differing only in routing

temporal-slab implements the structural invariants that enforce drainability:
- Per-epoch slab lists (prevents cross-lifetime mixing)
- CLOSING state rejection (prevents post-boundary allocations)
- Conservative recycling (free_count == capacity check)

**Primary contribution:** The drainability paper, not the allocator. The paper's principles apply to jemalloc, tcmalloc, mimalloc—practitioners can improve their existing allocators by understanding lifetime-granularity alignment.

---

## What temporal-slab Demonstrates

### 1. Bounded RSS Under Sustained Churn (Validated)

**Drainability validation experiments:**

| Configuration | Recycle Rate | Net Slabs | RSS Behavior |
|---------------|--------------|-----------|--------------|
| Mixed lifetimes (violation) | 0.28% | ~493K (~1.9 GB) | Linear Ω(t) growth |
| Isolated lifetimes (satisfaction) | 66.5% | ~2K (~8 MB) | Bounded O(1) plateau |

**Differential:** 238× improvement in recycle rate validates drainability theory's predictions.

**Mechanism:**
- Per-epoch slab lists enforce structural isolation
- epoch_close() deterministically reclaims empty slabs
- Lifetime-aligned routing prevents granule pinning

**Sustained churn test (2000 cycles):**
- Allocator committed bytes: 0.70 MB → 0.70 MB (0.0% drift)
- RSS stable after working set builds (no unbounded growth)

### 2. Deterministic Reclamation (Unique Feature)

Memory is reclaimed when the application calls `epoch_close()`, not when allocator heuristics trigger:
- No GC pauses (application controls timing)
- No surprise compaction (reclamation at explicit boundaries)
- Observable via telemetry (epoch state, refcounts, slab counts)

**Use case:** Systems needing predictable memory lifecycle (request handlers, frame rendering, batch processing).

### 3. Safety Guarantees

- No runtime munmap (slabs stay mapped, no segfaults from stale handles)
- Invalid/double frees return false (never crash)
- Conservative recycling (only FULL slabs recycled, prevents races)
- Thread-safe with lock-free fast path

---

## Performance Characteristics

### Allocation Microbenchmarks (x86_64 Only)

**Isolated fast-path measurement** (100M allocations, GitHub Actions AMD EPYC 7763):

| Percentile | temporal-slab | system_malloc | Comparison |
|------------|---------------|---------------|------------|
| p50 | 40ns | 31ns | 1.3× slower |
| p99 | 131ns | 1,463ns | 11.2× faster |
| p999 | 371ns | 4,418ns | 11.9× faster |

**Important:** These measure pure allocation in tight loops. They do NOT predict integrated application performance.

### Integrated Request Benchmarks (x86_64, tslab-request-bench)

**Realistic HTTP request handling** (1200 requests, 4 threads, 30-45s):

| Allocator | Mean | p50 | p95 | p99 | Rank |
|-----------|------|-----|-----|-----|------|
| mimalloc | 45.00 µs | 32.77 µs | 65.54 µs | 65.54 µs | 1st |
| tcmalloc | 46.76 µs | 32.77 µs | 65.54 µs | 65.54 µs | 2nd |
| glibc | 51.65 µs | 32.77 µs | 65.54 µs | 131.07 µs | 3rd |
| jemalloc | 59.27 µs | 32.77 µs | 65.54 µs | 131.07 µs | 4th |
| **temporal-slab** | **127.46 µs** | **65.54 µs** | **131.07 µs** | **262.14 µs** | **5th** |

temporal-slab is 2.8× slower than mimalloc in end-to-end request handling. Epoch management overhead and per-epoch cache fragmentation dominate the fast-path gains.

**RSS behavior** (same benchmark):

| Allocator | Baseline | Final | Growth |
|-----------|----------|-------|--------|
| glibc | 2.06 MB | 2.10 MB | 1.70% |
| **temporal-slab** | **1.89 MB** | **2.31 MB** | **22.11%** |
| tcmalloc | 7.56 MB | 8.59 MB | 13.64% |
| mimalloc | 2.41 MB | 3.27 MB | 35.71% |

**Note:** Short test duration (30-45s) doesn't trigger sustained RSS drift. temporal-slab's advantage emerges in multi-hour/multi-day runs where lifetime mixing causes unbounded growth in other allocators.

### Performance Summary

**Trade-off:**
- Give up 2.8× request throughput
- Get bounded RSS (0% growth over days/weeks)
- Get deterministic reclamation (epoch_close() controls timing)

**When favorable:**
- Long-running services (RSS drift → OOM → restart costs > 2.8× latency)
- Memory-constrained containers (hard limits, no swap)
- Predictable memory lifecycle requirements

**When unfavorable:**
- Latency-critical systems (2.8× slower disqualifies it)
- High-throughput services (slower = fewer requests/sec)
- Short-lived processes (no time for RSS drift to matter)

---

## Platform Support

**Current:** x86_64 Linux (GitHub Actions AMD EPYC 7763)

**ARM Status:** Untested. Code uses standard C11 atomics and POSIX (portable), but:
- Performance characteristics unknown (ARM atomics differ from x86)
- Benchmark numbers above are x86_64-specific
- Potential for ARM-specific race conditions (different memory ordering)

**Recommendation:** Validate on ARM64 (Apple Silicon or AWS Graviton) before deploying.

---

## Limitations

**Hard constraints:**
- Max object size: 768 bytes (handle API) or 760 bytes (malloc wrapper)
- Fixed size classes: 64, 96, 128, 192, 256, 384, 512, 768 bytes
- Linux only (RSS tracking via /proc/self/statm)
- No NUMA awareness
- No realloc support

**Performance:**
- 2.8× slower than mimalloc in integrated request handling (x86_64)
- Epoch management overhead accumulates in real workloads
- Per-epoch slab lists reduce cache locality vs unified allocator

**Maturity:**
- Recent concurrency bugs (Feb 2026: handle invalidation, madvise races)
- No production deployments
- Limited to 8-thread stress testing so far

---

## Core Design

### Memory Model

- **Slab = 4KB page** - Header, bitmap, object slots
- **16-epoch ring buffer** - Epochs 0-15 wrap around
- **Per-epoch slab lists** - PARTIAL and FULL lists per size class per epoch
- **Bounded cache** - 32 slabs per class (prevents unbounded growth)

### Allocation Fast Path (Lock-Free)

```c
1. ci = class_index_for_size(size);           // O(1) lookup table
2. state = epoch_state[epoch];                // Check epoch ACTIVE
3. s = atomic_load(&sc->current_partial);     // Load partial slab
4. slot = bitmap_allocate_cas(s);             // CAS on bitmap
5. return s->data + (slot * size);            // Compute pointer
```

**No locks in common case.** Slow path (new slab allocation) takes per-class mutex.

### Recycling Strategy (Conservative)

```c
// Only recycle slabs that were previously FULL
if (slab->list_id == LIST_FULL && free_count == capacity) {
  move_to_cache(slab);  // Safe - no concurrent allocations possible
}
```

**Never recycle from PARTIAL list** → Prevents use-after-free races.

### Epoch Lifecycle

```c
epoch_advance(alloc);              // Current epoch → CLOSING, advance to next
EpochId e = epoch_current(alloc);  // Get new ACTIVE epoch
void* p = slab_malloc_epoch(alloc, 128, e);

// Allocations in CLOSING epochs are rejected
// Enables deterministic reclamation at boundary

epoch_close(alloc, closed_epoch);  // Scan and recycle empty slabs
```

**Structural drainability enforcement:**
- CLOSING state prevents post-boundary allocations
- Per-epoch lists prevent cross-lifetime mixing
- Reclamation is explicit, not heuristic

---

## API Reference

### Lifecycle

```c
SlabAllocator* slab_allocator_create(void);
void slab_allocator_free(SlabAllocator* alloc);

void allocator_init(SlabAllocator* alloc);    // For custom storage
void allocator_destroy(SlabAllocator* alloc);
```

### Handle-Based API (Zero Overhead)

```c
void* alloc_obj_epoch(SlabAllocator* alloc, uint32_t size, EpochId epoch, SlabHandle* out_handle);
bool free_obj(SlabAllocator* alloc, SlabHandle handle);
```
- No per-allocation headers
- Explicit handle management
- Returns false on invalid/double frees (safe)

### Malloc-Style API (8-Byte Overhead)

```c
void* slab_malloc_epoch(SlabAllocator* alloc, size_t size, EpochId epoch);
void slab_free(SlabAllocator* alloc, void* ptr);
```
- 8-byte header stores handle
- Max size: 760 bytes (768 - 8 byte header)
- NULL-safe: `slab_free(a, NULL)` is no-op

### Epoch Management

```c
typedef uint32_t EpochId;

EpochId epoch_current(SlabAllocator* alloc);
void epoch_advance(SlabAllocator* alloc);
void epoch_close(SlabAllocator* alloc, EpochId epoch);
```

**Epoch semantics:**
- 16-epoch ring buffer (IDs 0-15 wrap)
- Temporal grouping: objects in same epoch share slabs
- Thread-safe: atomic operations
- Advancing → marks previous epoch CLOSING
- Closing → scans for empty slabs, recycles them

### Epoch Domains (Optional RAII Wrappers)

```c
#include <epoch_domain.h>

epoch_domain_t* epoch_domain_create(SlabAllocator* alloc);
void epoch_domain_enter(epoch_domain_t* domain);
void epoch_domain_exit(epoch_domain_t* domain);
void epoch_domain_destroy(epoch_domain_t* domain);
```

Provides scoped lifetime management with automatic cleanup. See `examples/domain_usage.c` and `docs/EPOCH_DOMAINS.md`.

---

## Example Usage

### Request-Scoped Allocation

```c
SlabAllocator* alloc = slab_allocator_create();

// Long-lived server state in epoch 0
void* server_ctx = slab_malloc_epoch(alloc, 256, 0);

// Rotate epochs every 1000 requests
for (int i = 0; i < 100000; i++) {
  if (i % 1000 == 0) {
    epoch_advance(alloc);
  }

  EpochId e = epoch_current(alloc);
  void* req_data = slab_malloc_epoch(alloc, 128, e);
  void* req_buffer = slab_malloc_epoch(alloc, 256, e);

  // Process request...

  slab_free(alloc, req_data);
  slab_free(alloc, req_buffer);
}

slab_allocator_free(alloc);
```

**Result:** Request-scoped allocations drain together, enabling efficient slab recycling.

---

## Build and Test

```bash
cd src/
make                    # Build all tests
./smoke_tests          # Correctness validation
./test_epochs          # Epoch isolation tests
./churn_test           # RSS bounds validation
./benchmark_accurate   # Allocation microbenchmarks
```

### Compile-Time Flags

**Production build (default):**
```bash
make
```

**With RSS reclamation (madvise empty slabs):**
```bash
make CFLAGS="-DENABLE_RSS_RECLAMATION=1 -I../include"
```

**With diagnostic counters (RSS debugging):**
```bash
make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1 -I../include"
```

**With TLS cache (experimental tail latency reduction):**
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 -I../include" TLS_OBJ=slab_tls_cache.o
```

See `docs/COMPILER_FLAGS.md` for complete flag documentation.

---

## When to Consider This Allocator

### Good Fit

- **Research/validation** - Understanding drainability principles
- **Long-running services** - RSS drift causes OOM after days/weeks
- **Memory-constrained** - Containers with hard limits, no swap
- **Deterministic reclamation** - Need to control memory lifecycle explicitly
- **Fixed-size allocation** - Objects ≤768 bytes in known size classes

### Poor Fit

- **Latency-critical systems** - 2.8× slower than mimalloc (integrated)
- **General-purpose allocation** - jemalloc handles arbitrary sizes/patterns better
- **Large objects** - 768-byte limit
- **High-throughput services** - Slower = lower requests/sec
- **Production-critical** - Limited battle-testing, no production deployments

**Honest assessment:** If you need guaranteed bounded RSS and can accept 2.8× slower throughput, this provides a working solution. If throughput or tail latency is your primary concern, use mimalloc or jemalloc.

---

## Use Cases

**What this was designed for:**
- Session stores with bounded memory requirements
- Connection metadata pools
- Cache entries with lifetime classes
- Request-scoped allocation in long-running servers

**Reality:** These use cases value bounded RSS, but may not accept 2.8× throughput penalty. Market fit remains uncertain.

---

## Technical Documentation

**Core design:**
- `docs/ARCHITECTURE.md` - Memory model, state machine, invariants
- `docs/semantics.md` - Epoch lifecycle contracts, reclamation semantics

**Validation:**
- `CHANGELOG.md` - Recent bug fixes (concurrency races)
- `docs/TSAN_VALIDATION.md` - Thread safety testing

**Research:**
- See `drainability-framework` repository for the paper
- Experimental data in `drainability-framework/data/experimental/`

---

## Project Structure

```
include/
  slab_alloc.h           - Public API
  epoch_domain.h         - Optional RAII domains
src/
  slab_alloc.c           - Core allocator (~3000 LOC)
  epoch_domain.c         - Domain implementation
  slab_alloc_internal.h  - Internal structures
  smoke_tests.c          - Correctness tests
  test_epochs.c          - Epoch isolation tests
  benchmark_accurate.c   - Microbenchmarks
docs/
  semantics.md           - Authoritative contracts
  ARCHITECTURE.md        - Design documentation
examples/
  domain_usage.c         - Epoch domain examples
```

---

## Known Issues

**Concurrency (Fixed as of Feb 2026):**
- ~~Handle invalidation under 8-thread stress~~ (Fixed: Feb 10)
- ~~Reuse-before-madvise zombie slab corruption~~ (Fixed: Feb 9)

**Platform:**
- x86_64 only (ARM untested, performance unknown)
- All benchmark numbers are x86_64-specific (AMD EPYC 7763)
- 4KB page size assumption (some ARM systems use 16KB/64KB)

**Testing:**
- Limited to 8-thread stress tests so far
- No 24+ hour soak tests
- No production deployments

---

## Design Principles

1. **Safety over optimization** - Conservative recycling prevents races
2. **Explicit over heuristic** - epoch_close() is explicit, not triggered
3. **Observability** - Invalid frees return false, never crash
4. **Structural enforcement** - Per-epoch lists prevent lifetime mixing
5. **Bounded resources** - Cache + overflow guarantee RSS limits

---

## Relationship to Drainability Paper

This allocator is the **reference implementation** cited by:

> Blackwell, D. (2026). *Drainability: When Coarse-Grained Memory Reclamation Produces Bounded Retention.* Technical Report. doi:10.5281/zenodo.18653776

**Paper contributions:**
- Alignment Theorem: Reclaimability ⟺ Drainability at boundary
- Pinning Growth Theorem: Sustained violation → Ω(t) growth
- Sharp dichotomy: O(1) vs Ω(t), no intermediate regime

**Allocator's role:**
- Implements epoch-based routing (function ρ in paper)
- Validates bounded retention under drainability satisfaction
- Demonstrates 238× recycle-rate differential

The paper's principles apply to ALL coarse-grained allocators (jemalloc, tcmalloc, mimalloc). You don't need to adopt temporal-slab to benefit from drainability—you need to route allocations to align with lifetime boundaries in whatever allocator you use.

---

## Citation

If using this allocator for research or validation:

```bibtex
@software{blackwell2026temporalslab,
  author    = {Blackwell, Dayna},
  title     = {temporal-slab: Lifetime-Aware Slab Allocator},
  year      = {2026},
  note      = {Reference implementation for drainability framework}
}
```

If citing the drainability work:

```bibtex
@techreport{blackwell2026drainability,
  title     = {Drainability: When Coarse-Grained Memory Reclamation Produces Bounded Retention},
  author    = {Blackwell, Dayna},
  year      = {2026},
  doi       = {10.5281/zenodo.18653776}
}
```

---

## Non-Goals

temporal-slab intentionally does not:
- Replace general-purpose allocators (use jemalloc for that)
- Support arbitrary object sizes (fixed classes by design)
- Optimize for throughput (optimizes for memory predictability)
- Perform background compaction (deterministic, explicit reclamation)

It is a specialized research implementation demonstrating structural drainability, not a universal malloc replacement.

---

## License

MIT License - see LICENSE for details

---

## Contact

- **Drainability paper:** See drainability-framework repository
- **Issues/Questions:** File issues in this repository
- **Email:** dayna@blackwell-systems.com
