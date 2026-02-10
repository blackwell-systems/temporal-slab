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
  - [SLOWPATH_THRESHOLD_NS](#slowpath_threshold_ns)
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

**Purpose**: Records tail latency samples to diagnose slow allocations in production.

**Default**: `0` (disabled)

**Overhead**:
- Memory: ~320KB (10,000 sample ring buffer)
- Latency: ~50-100ns per sample (only when threshold exceeded)

**When to use**:
- Diagnosing p99/p999/p9999 latency outliers
- Distinguishing allocator slowness from VM/OS scheduling noise
- Production tail latency debugging (WSL2, VMs, noisy neighbors)

**Instrumentation collected**:
- Wall-clock time (CLOCK_MONOTONIC)
- Thread CPU time (CLOCK_THREAD_CPUTIME_ID)
- Size class, reason flags (lock wait, new slab, zombie repair, etc.)
- CAS retry count

**Analysis output**:
```
=== Slowpath Samples ===
Total samples: 127 (threshold: 5000ns)

Classification:
  Preempted (wall>3×cpu): 94 (74.0%) - WSL2/VM scheduling noise
  Real work (wall≈cpu):    33 (26.0%) - Actual allocator slowpath
```

**Build example**:
```bash
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING=1 -DSLOWPATH_THRESHOLD_NS=5000 -I../include"
```

**Tuning**: See [SLOWPATH_THRESHOLD_NS](#slowpath_threshold_ns) below.

**Related documentation**: `workloads/README_SLOWPATH_SAMPLING.md`

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

### SLOWPATH_THRESHOLD_NS

**Purpose**: Sets the latency threshold (nanoseconds) for slowpath sampling.

**Default**: `5000` (5µs)

**Requires**: `ENABLE_SLOWPATH_SAMPLING=1`

**Tuning guidance**:
- **5µs (default)**: Captures extreme tail (p9999+), minimal overhead
- **1µs**: Captures p999 outliers, low overhead
- **500ns**: Captures p99 outliers, moderate overhead
- **Lower**: More samples, more overhead, larger ring buffer churn

**Calibration**:
1. Run benchmark to measure average allocation latency
2. Set threshold to 2-3× average latency
3. Validate sample count is reasonable (1-100 per million ops)

**Build example**:
```bash
# Capture operations > 1µs
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING=1 -DSLOWPATH_THRESHOLD_NS=1000 -I../include"
```

---

## Build Examples

### Development Build (All Diagnostics)
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 \
             -DENABLE_SLOWPATH_SAMPLING=1 \
             -DSLOWPATH_THRESHOLD_NS=1000 \
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
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING=1 \
             -DSLOWPATH_THRESHOLD_NS=5000 \
             -O3 -g -I../include"
```

---

## Quick Reference Table

| Flag | Default | Overhead | Use Case |
|------|---------|----------|----------|
| `ENABLE_TLS_CACHE` | 0 | ~10-20ns | High locality workloads |
| `ENABLE_SLOWPATH_SAMPLING` | 0 | ~50-100ns/sample | Tail latency diagnosis |
| `ENABLE_RSS_RECLAMATION` | 0 | ~1-5µs/epoch | Memory-constrained systems |
| `ENABLE_DIAGNOSTIC_COUNTERS` | 0 | ~1-2% | RSS debugging |
| `ENABLE_LOCK_RANK_DEBUG` | 0 | ~5-10% | Deadlock detection (dev) |
| `SLAB_PAGE_SIZE` | 4096 | Varies | Platform-specific tuning |
| `SLOWPATH_THRESHOLD_NS` | 5000 | Varies | Sampling sensitivity |

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
