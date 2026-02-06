# Benchmark Results Guide

This directory contains benchmark results for different phases of the ZNS-Slab allocator.

## Files

### Release-Quality Results (Use This)
- **`benchmark_results_phase1.5_with_cache.txt`**
  - Phase 1.5 with slab cache + performance counters
  - p99: 1423ns, p999: 2303ns
  - 97% cache hit rate
  - Tail latency fully attributed
  - **This is the current release-quality benchmark**

### Baseline (For Comparison)
- **`benchmark_results_phase1.0_baseline.txt`**
  - Phase 1.0 without slab cache
  - p99: 4895ns, p999: 7165ns
  - Every allocation calls mmap()
  - Archived for comparison purposes

### Correctness Tests
- **`src/test_results.txt`**
  - Single-thread and multi-thread smoke tests
  - Validates correctness, not performance
  - See file header for details

## Quick Comparison

| Metric | Phase 1.0 Baseline | Phase 1.5 With Cache | Improvement |
|--------|-------------------|---------------------|-------------|
| p50 alloc | 32ns | 26ns | 1.2x faster |
| p99 alloc | 4895ns | 1423ns | **3.4x faster** |
| p999 alloc | 7165ns | 2303ns | **3.1x faster** |
| mmap calls | ~64K | 32K | 2x fewer |
| Cache hit | 0% | 97% | - |

## What Changed

**Phase 1.0 → 1.5:**
- Added slab cache (32 pages per size class)
- Added 5 performance counters for attribution
- Cache eliminates mmap() from hot path
- All tail latency now attributable to mmap() on cache miss

## Methodology

Both benchmarks use identical methodology:
- mmap/munmap for accurate RSS tracking
- memset to fault pages in
- Sorted arrays for p50/p99/p999 percentiles
- Compiler barriers to prevent optimization
- 1M iterations for statistical significance

See file headers for detailed background and context.

## Which File Should I Use?

- **For claiming performance**: `benchmark_results_phase1.5_with_cache.txt`
- **For showing improvement**: Compare both files
- **For validating correctness**: `src/test_results.txt`

## Release Notes

See `RELEASE_NOTES_v1.5.md` for complete Phase 1.5 release details, architecture changes, and technical implementation.
