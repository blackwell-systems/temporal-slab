# Compiler Flags Reference

This document lists all compile-time configuration flags for the ZNS-Slab allocator.

## Table of Contents

- [Feature Flags](#feature-flags)
  - [ENABLE_TLS_CACHE](#enable_tls_cache)
  - [ENABLE_SLOWPATH_SAMPLING](#enable_slowpath_sampling)
  - [ENABLE_RSS_RECLAMATION](#enable_rss_reclamation)
  - [ENABLE_DIAGNOSTIC_COUNTERS](#enable_diagnostic_counters)
  - [ENABLE_LOCK_RANK_DEBUG](#enable_lock_rank_debug)
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

---

## Quick Reference Table

| Flag | Default | Overhead | Use Case |
|------|---------|----------|----------|
| `ENABLE_TLS_CACHE` | 0 | ~10-20ns | High locality workloads |
| `ENABLE_SLOWPATH_SAMPLING` | 0 | ~0.2ns avg | Tail latency diagnosis (1/1024 sampling) |
| `ENABLE_RSS_RECLAMATION` | 0 | ~1-5µs/epoch | Memory-constrained systems |
| `ENABLE_DIAGNOSTIC_COUNTERS` | 0 | ~1-2% | RSS debugging |
| `ENABLE_LOCK_RANK_DEBUG` | 0 | ~5-10% | Deadlock detection (dev) |
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

---

## Related Documentation

- **TLS Cache**: `docs/TLS_CACHE_DESIGN.md`
- **Slowpath Sampling**: `workloads/README_SLOWPATH_SAMPLING.md`
- **RSS Tracking**: `docs/RSS_INSTRUMENTATION_SUMMARY.md`
- **Build System**: `README.md` (root)
- **Testing**: `workloads/README.md`

---

**Last updated**: 2025-02-10  
**Allocator version**: ZNS-Slab v0.3 (epoch-based, TLS-cache, adaptive-bypass)
