# Compiler Flags Reference

This document lists all compile-time configuration flags for the ZNS-Slab allocator.

## Table of Contents

- [Feature Flags](#feature-flags)
  - [ENABLE_TLS_CACHE](#enable_tls_cache)
  - [ENABLE_SLOWPATH_SAMPLING](#enable_slowpath_sampling)
  - [ENABLE_RSS_RECLAMATION](#enable_rss_reclamation)
  - [ENABLE_DIAGNOSTIC_COUNTERS](#enable_diagnostic_counters)
  - [ENABLE_LOCK_RANK_DEBUG](#enable_lock_rank_debug)
  - [ENABLE_DRAINPROF](#enable_drainprof)
- [Tuning Parameters](#tuning-parameters)
  - [SLAB_PAGE_SIZE](#slab_page_size)
- [Build Examples](#build-examples)

---

## Feature Flags

### ENABLE_TLS_CACHE

**Purpose**: Enables thread-local caching layer for high-frequency allocation workloads.

**Default**: `0` (disabled)

**Overhead**: 
- Memory: ~16KB per thread (cache storage)
- Latency: Negligible when enabled, ~10-20ns bypass logic per operation

**When to use**:
- Workloads with high temporal locality (small working set, repeated alloc/free)
- Latency-sensitive applications (reduces p50/p95 by 30-50%)
- Single-threaded or thread-local allocation patterns

**When NOT to use**:
- Workloads with low temporal locality (random access patterns)
- Memory-constrained environments
- High object churn without reuse

**Behavior**:
- Maintains per-thread cache with adaptive bypass based on hit rate
- Uses sliding window (256 ops) to detect cache thrashing
- Falls back to shared allocator on cache miss

**Build example**:
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 -I../include"
```

**Related documentation**: `docs/TLS_CACHE_DESIGN.md`

---

### ENABLE_SLOWPATH_SAMPLING

**Purpose**: Probabilistic end-to-end allocation timing to diagnose tail latency sources and distinguish allocator work from scheduler interference.

**Default**: `0` (disabled)

**Overhead**:
- Memory: ~160 bytes per thread (TLS counters, no heap allocation)
- Latency: ~0.2ns average per allocation (1/1024 sampling × 200ns per sample)
- CPU: Negligible (<0.02% overall impact)

**Sampling strategy (Phase 2.5)**:
- **Probabilistic** (not threshold-based): Samples 1 out of every 1024 allocations regardless of latency
- **Why probabilistic**: Validates measurement infrastructure works, doesn't miss fast-but-preempted operations
- **Per-thread TLS counters**: No atomic contention, safe for multi-threaded workloads

**When to use**:
- Diagnosing p99/p999 latency outliers on WSL2 or VMs
- Distinguishing allocator slowness from OS scheduling noise
- Investigating zombie repair frequency (invariant violation signal)
- Validating that sampling infrastructure works (not just "did we see slow ops?")

**Instrumentation collected**:
- **Allocation timing** (end-to-end `alloc_obj_epoch()`):
  - Wall-clock time (CLOCK_MONOTONIC)
  - Thread CPU time (CLOCK_THREAD_CPUTIME_ID)
  - Sample count, avg/max for both metrics
- **Zombie repair timing** (explicit measurement):
  - Repair count, wall/CPU timing
  - Reason attribution: full_bitmap, list_mismatch, other

**Analysis output**:
```
=== Slowpath Sampling (1/1024) ===
Samples: 97
Avg wall: 4382 ns (max: 251618 ns)
Avg CPU:  1536 ns (max: 9293 ns)
Ratio: 2.85x
⚠ wall >> cpu: Scheduler interference detected

=== Zombie Repairs ===
Count: 0
✓ No invariant violations
```

**Interpretation**:
- **wall >> cpu** (e.g., 2.85×): WSL2/VM preemption, not allocator issue
- **wall ≈ cpu** (e.g., 1.1×): Real allocator work (locks, CAS storms, new slabs)
- **High repair_count**: Invariant violations (publication races, memory ordering bugs)

**Build example**:
```bash
# Enable probabilistic sampling
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING -I../include"

# Add to your test code:
#ifdef ENABLE_SLOWPATH_SAMPLING
  ThreadStats stats = slab_stats_thread();
  printf("Avg wall: %lu ns (max: %lu ns)\n", ...);
#endif
```

**API**:
```c
#include "slab_stats.h"

ThreadStats slab_stats_thread(void);  // Get current thread's statistics
```

**Migration from old threshold-based approach**:
- Old: `SLOWPATH_THRESHOLD_NS` required tuning, could miss fast ops
- New: No threshold needed, samples 1/1024 regardless of latency
- Old: Ring buffer, complex sample storage
- New: Simple TLS counters (avg/max only)

**Related documentation**: `workloads/README_SLOWPATH_SAMPLING.md` (comprehensive guide)

---

### ENABLE_RSS_RECLAMATION

**Purpose**: Returns memory to the OS via `madvise(MADV_DONTNEED)` on epoch close.

**Default**: `0` (disabled - memory retained for reuse)

**Overhead**:
- Latency: ~1-5µs per madvise syscall on epoch close
- RSS: Reduces resident memory at the cost of future page faults

**When to use**:
- Long-running processes with variable memory footprint
- Memory-constrained environments (containers, VMs)
- Workloads with distinct phases (load, idle, process)

**When NOT to use**:
- Latency-critical applications (epoch close becomes slower)
- Workloads with steady allocation patterns (defeats reuse)
- Benchmarks comparing RSS behavior (invalidates comparisons)

**Behavior**:
- Calls `madvise(MADV_DONTNEED)` on empty slabs during epoch close
- OS may zero pages on next fault (security), increasing future latency
- Does NOT free virtual address space (only physical memory)

**Build example**:
```bash
make CFLAGS="-DENABLE_RSS_RECLAMATION=1 -I../include"
```

**Related documentation**: `docs/RSS_INSTRUMENTATION_SUMMARY.md`

---

### ENABLE_DIAGNOSTIC_COUNTERS

**Purpose**: Tracks live bytes and committed bytes via atomic counters for RSS debugging.

**Default**: `0` (disabled)

**Overhead**:
- Latency: ~1-2% on hot paths (atomic increments on alloc/free)
- Memory: Negligible (two 64-bit atomics per allocator)

**When to use**:
- Proving bounded RSS invariants
- Debugging memory leaks or unexpected RSS growth
- Development/testing (not production)

**Counters tracked**:
- `live_bytes`: Sum of allocated object sizes
- `committed_bytes`: Sum of mmap'd slab memory

**Build example**:
```bash
make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1 -I../include"
```

---

### ENABLE_LOCK_RANK_DEBUG

**Purpose**: Validates lock ordering to detect deadlock potential at runtime.

**Default**: `0` (disabled)

**Overhead**:
- Latency: ~5-10% on lock operations
- Memory: Per-thread stack tracking

**When to use**:
- Development/debugging only
- Testing new lock-based features
- Validating multithreaded correctness

**Behavior**:
- Asserts that locks are acquired in rank order
- Panics on rank violations with diagnostic output
- Tracks lock owner thread for debugging

**Build example**:
```bash
make CFLAGS="-DENABLE_LOCK_RANK_DEBUG=1 -I../include"
```

---

### ENABLE_DRAINPROF

**Purpose**: Integrates the drainability profiler to measure Drainability Satisfaction Rate (DSR) - the fraction of epochs that are fully drained (no live allocations) when closed.

**Default**: `0` (disabled)

**Overhead**:
- **Production mode**: < 2ns per allocation (atomic increment/decrement)
- **Diagnostic mode**: ~25ns per allocation (per-allocation tracking with source locations)
- Memory: ~32 bytes per open epoch (production), proportional to pinned allocations (diagnostic)

**When to use**:
- **Production mode**: Always-on monitoring to detect structural memory leaks
  - Measure DSR over time to catch regression
  - Low enough overhead for production use
- **Diagnostic mode**: Time-bounded investigation when DSR drops
  - Identifies exact allocation sites (file:line) pinning epochs
  - Provides actionable information for fixing structural leaks

**When NOT to use**:
- Benchmarks comparing raw allocation performance
- Workloads where < 2ns overhead matters (extremely latency-sensitive)

**What it detects**:
- **Structural memory leaks**: Individual objects freed, but epochs can't be reclaimed because one long-lived allocation pins the entire epoch
- Traditional leak detectors (Valgrind, ASan) miss these because objects ARE freed
- Example: 999/1000 objects freed, but 1 remaining prevents epoch reclamation

**Behavior**:
- Instruments four lifecycle points:
  1. `epoch_advance()` → `drainprof_granule_open()`
  2. `alloc_obj_epoch()` → `drainprof_alloc_register()`
  3. `free_obj()` → `drainprof_alloc_deregister()`
  4. `epoch_close()` → `drainprof_granule_close()`
- Lock-free atomic operations on hot path (production mode)
- Zero overhead when disabled (compiled out via preprocessor)

**Metrics provided**:
- **DSR (Drainability Satisfaction Rate)**: drainable_closes / total_closes
  - 1.0 (100%): Perfect drainability
  - 0.5 (50%): Half of epochs pinned
  - 0.0 (0%): Every epoch has live allocations
- Peak open epochs, allocation/deallocation counts
- Diagnostic mode: Per-site pinning reports with source locations

**Build example**:
```bash
# Requires drainability-profiler library
# Clone from: https://github.com/blackwell-systems/drainability-profiler

cd drainability-profiler && make
cd ../temporal-slab/src

# Production mode (always-on monitoring)
make ENABLE_DRAINPROF=1 DRAINPROF_PATH=../../drainability-profiler

# Diagnostic mode (set in application code):
# drainprof_config config = { .mode = DRAINPROF_DIAGNOSTIC, ... };
# g_profiler = drainprof_create_with_config(&config);
```

**Example usage**:
```c
#ifdef ENABLE_DRAINPROF
#include <drainprof.h>

// Global profiler instance
drainprof* g_profiler = NULL;

void init_profiler(void) {
    g_profiler = drainprof_create();  // Production mode
}

void check_drainability(void) {
    drainprof_snapshot_t snap;
    drainprof_snapshot(g_profiler, &snap);

    printf("DSR: %.2f%% (%lu drainable / %lu total)\n",
           snap.dsr * 100.0, snap.drainable_closes, snap.total_closes);

    if (snap.dsr < 0.90) {
        fprintf(stderr, "WARNING: Low DSR - investigate structural leaks\n");
    }
}
#endif
```

**Integration validation**:
- Validated via CI in drainability-profiler repository
- P-sweep test: Confirms DSR = 1.0 - p for controlled violation rates
- Diagnostic test: Verifies allocation site identification

**Related projects**:
- **drainability-profiler**: https://github.com/blackwell-systems/drainability-profiler
- **Integration tests**: `drainability-profiler/examples/temporal-slab/`
- **Research paper**: [doi.org/10.5281/zenodo.18653776](https://doi.org/10.5281/zenodo.18653776)

---

## Tuning Parameters

### SLAB_PAGE_SIZE

**Purpose**: Sets the allocation granularity for slabs.

**Default**: `4096` (4KB - x86-64 Linux page size)

**Valid values**: Must be power of 2

**When to change**:
- ARM64 with 64KB pages: Set to `65536`
- Large object workloads: Larger pages reduce mmap overhead
- Memory-constrained: Smaller pages reduce internal fragmentation

**Implications**:
- Smaller pages: More mmap calls, less internal fragmentation
- Larger pages: Fewer mmap calls, more internal fragmentation
- Must match OS page size for RSS tracking accuracy

**Build example**:
```bash
make CFLAGS="-DSLAB_PAGE_SIZE=65536 -I../include"  # 64KB pages
```

---

## Build Examples

### Development Build (All Diagnostics)
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 \
             -DENABLE_SLOWPATH_SAMPLING \
             -DENABLE_DIAGNOSTIC_COUNTERS=1 \
             -DENABLE_LOCK_RANK_DEBUG=1 \
             -g -O2 -I../include"
```

### Production Build (Optimized)
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 \
             -O3 -DNDEBUG -I../include"
```

### Memory-Constrained Build (Low RSS)
```bash
make CFLAGS="-DENABLE_RSS_RECLAMATION=1 \
             -O2 -I../include"
```

### Benchmark Build (Profiling)
```bash
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING \
             -O3 -g -I../include"
```

### Drainability Monitoring Build (Production)
```bash
# Requires drainability-profiler library built first
make ENABLE_DRAINPROF=1 \
     DRAINPROF_PATH=../../drainability-profiler \
     CFLAGS="-O3 -I../include"
```

---

## Quick Reference Table

| Flag | Default | Overhead | Use Case |
|------|---------|----------|----------|
| `ENABLE_TLS_CACHE` | 0 | ~10-20ns | High locality workloads |
| `ENABLE_SLOWPATH_SAMPLING` | 0 | ~0.2ns avg | Tail latency diagnosis (1/1024 sampling) |
| `ENABLE_RSS_RECLAMATION` | 0 | ~1-5µs/epoch | Memory-constrained systems |
| `ENABLE_DIAGNOSTIC_COUNTERS` | 0 | ~1-2% | RSS debugging |
| `ENABLE_LOCK_RANK_DEBUG` | 0 | ~5-10% | Deadlock detection (dev) |
| `ENABLE_DRAINPROF` | 0 | <2ns prod, ~25ns diag | Structural leak detection |
| `SLAB_PAGE_SIZE` | 4096 | Varies | Platform-specific tuning |

---

## Feature Compatibility Matrix

| Feature | Compatible With | Incompatible With | Notes |
|---------|----------------|-------------------|-------|
| TLS Cache | All features | - | Recommended for production |
| Slowpath Sampling | All features | - | Low overhead, always safe |
| RSS Reclamation | All features | - | May conflict with perf testing |
| Diagnostic Counters | All features | - | Development only |
| Lock Rank Debug | All features | - | Development only |
| Drainprof | All features | - | Production mode safe; diagnostic for investigation |

---

## Related Documentation

- **TLS Cache**: `docs/TLS_CACHE_DESIGN.md`
- **Slowpath Sampling**: `workloads/README_SLOWPATH_SAMPLING.md`
- **RSS Tracking**: `docs/RSS_INSTRUMENTATION_SUMMARY.md`
- **Drainability Profiler**: https://github.com/blackwell-systems/drainability-profiler
- **Drainprof Integration**: https://github.com/blackwell-systems/drainability-profiler/tree/main/examples/temporal-slab
- **Build System**: `README.md` (root)
- **Testing**: `workloads/README.md`

---

**Last updated**: 2026-02-16
**Allocator version**: ZNS-Slab v0.3 (epoch-based, TLS-cache, adaptive-bypass, drainprof-instrumented)
