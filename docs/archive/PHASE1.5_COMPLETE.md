# Phase 1.5 Complete - Release Artifact

## Status: ✅ DONE

Phase 1.5 is **measured + attributable + reproducible** - ready for embedding.

## What Phase 1.5 Delivers

**Median stays in tens of nanoseconds**, **tail collapsed** (3.4x improvement), **remaining tail is explainable**.

### Performance (Intel Core Ultra 7 165H, 22 cores, GCC 13.3.0 -O3)

```
Allocation:  p50=26ns   p99=1423ns   p999=2303ns
Free:        p50=24ns   p99=45ns     p999=184ns

Overhead:    3.4% memory (252MB allocator for 244MB payload)
Cache:       97% hit rate (32K mmap calls for 1M allocations)
```

### Tail Latency Attribution

All p99/p999 spikes are **attributable**:
- 32,259 mmap() calls → p99/p999 spikes
- 0 current_partial misses → fast path perfect
- Balanced transitions (32,258 each direction) → no leaks

### Architecture

**Lock-Free Fast Path:**
- Atomic `current_partial` pointer per size class
- CAS-based bitmap slot allocation (CTZ + compare-exchange)
- Precise transition detection (1→0 for FULL, 0→1 for PARTIAL)

**Slab Cache:**
- 32 pages per size class (128KB)
- Simple stack with mutex
- Pop-first strategy: cache_pop() → mmap() on miss
- 97% cache hit rate eliminates mmap from steady state

**Performance Counters:**
- 5 atomic counters (relaxed ordering, zero overhead)
- `slow_path_hits`, `new_slab_count`, `list_move_partial_to_full`,
  `list_move_full_to_partial`, `current_partial_miss`
- Benchmark reports full attribution

## One-Command Reproducibility

```bash
make bench
```

Builds allocator, runs benchmark, writes results, checks for regressions.

**Expected output:**
- p50 < 50ns (fast path healthy)
- p99 < 2000ns (cache working)
- current_partial_miss == 0 (fast path perfect)

## Files

**Source:**
- `src/slab_alloc.c` (630 lines) - Core implementation
- `src/slab_alloc.h` (103 lines) - Public API
- `src/benchmark_accurate.c` (280 lines) - Measurement harness

**Results:**
- `benchmark_results_phase1.5_with_cache.txt` - Release-quality numbers
- `RELEASE_NOTES_v1.5.md` - Full technical details + environment

**Build:**
- `Makefile` (root) - `make bench`, `make test`, `make tsan`
- `src/Makefile` - Build targets

## Regression Detection

Phase 1.5 results are **locked in** as known-good baseline.

Regressions:
1. p50 > 50ns → fast path degraded
2. p99 > 2000ns → cache not working
3. current_partial_miss > 0 → fast path broken
4. slow_path_hits != new_slab_count → logic error

## What Comes Next

Phase 1.5 is **done**. The fork is:

### Option A: Phase 1.6 Hardening (make it boringly reliable)
- TSAN validation (on native Linux)
- Soak test (multi-thread, mixed sizes, long runtime)
- Debug mode with invariant checks
- Clean API boundary (library vs consumer separation)

### Option B: Phase 2 Capabilities (build the real ZNS story)
- Size routing policy (slabs vs direct-mmap)
- Tier abstraction (DRAM vs file-backed vs ZNS)
- Object identity for migration
- Eviction hooks

**Recommended:** Do 1.6 hardening first. It's high-leverage, low-risk, and makes Phase 2 easier.

## Key Insights

1. **Tail latency must be attributable** - Counters make p99/p999 defensible
2. **Slab cache kills mmap spikes** - 97% hit rate = 3.4x improvement  
3. **Median matters more than mean** - p50=26ns shows fast path health
4. **Reproducibility requires environment docs** - CPU, compiler, flags all matter
5. **One-command builds enable trust** - `make bench` turns repo into artifact

---

**Date:** February 5, 2026  
**Platform:** Linux WSL2 6.6.87.2 x86_64  
**Compiler:** GCC 13.3.0  
**Status:** ✅ Release Quality Achieved
