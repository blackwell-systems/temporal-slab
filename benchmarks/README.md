# temporal-slab Benchmarks

This directory contains benchmark harnesses and result visualizations for temporal-slab.

## Benchmark Suite

### Core Benchmarks

**benchmark_accurate** - Allocation latency and RSS measurement
- Measures p50/p95/p99/p999 allocation latency
- Tracks RSS growth under sustained allocation
- Reports internal fragmentation per size class
- Run: `cd src && ./benchmark_accurate`

**churn_test** - RSS stability under sustained churn
- Allocates/frees objects in cycles over 1000 iterations
- Validates bounded RSS growth (<50% growth threshold)
- Reports slab recycling statistics
- Run: `cd src && ./churn_test`

**smoke_tests** - Correctness and multi-threaded stress
- Single-threaded correctness
- Multi-threaded stress (8 threads × 500K operations)
- Basic latency measurement
- Run: `cd src && ./smoke_tests`

## Running Benchmarks

### Quick Start
```bash
cd benchmarks
./run_all.sh
```

This runs all benchmarks and generates CSV output in `results/`.

### Individual Benchmarks with CSV Export
```bash
cd src
./benchmark_accurate --csv ../benchmarks/results/latency.csv
./churn_test --csv ../benchmarks/results/rss_churn.csv
```

## Visualization

Generate charts from benchmark results:
```bash
cd tools
python3 plot_bench.py
```

Output charts appear in `docs/images/`:
- `latency_cdf.png` - Cumulative distribution of allocation latency
- `p99_vs_threads.png` - Tail latency scaling
- `rss_over_time.png` - RSS growth under churn
- `slab_lifecycle.png` - Slab allocation/recycling/overflow counts

## Baseline Comparisons

Compare against standard allocators (requires jemalloc, tcmalloc installed):

```bash
# System malloc
./run_all.sh --baseline malloc

# jemalloc
LD_PRELOAD=libjemalloc.so ./run_all.sh --baseline jemalloc

# tcmalloc
LD_PRELOAD=libtcmalloc.so ./run_all.sh --baseline tcmalloc
```

Results are saved to `baseline/{allocator}/` for comparison.

## CSV Output Format

### latency.csv
```
allocator,threads,size_class,op,avg_ns,p50_ns,p95_ns,p99_ns,p999_ns,ops_per_sec
temporal-slab,1,128,alloc,73.7,70,85,212,2537,13574660
temporal-slab,1,128,free,29.8,26,44,84,253,33557047
```

### rss_churn.csv
```
allocator,cycle,rss_mib,slabs_allocated,slabs_recycled,slabs_overflowed
temporal-slab,0,14.61,0,0,0
temporal-slab,100,14.85,128,95,0
temporal-slab,999,14.95,128,125,0
```

### fragmentation.csv
```
allocator,size_class,requested,rounded,wasted,efficiency_pct
temporal-slab,64,48,64,16,75.0
temporal-slab,96,72,96,24,75.0
temporal-slab,128,112,128,16,87.5
```

## Interpreting Results

### What Makes temporal-slab Competitive

**Key metrics where temporal-slab should win:**
1. **RSS stability** - Growth <5% over 1000 churn cycles (vs 20-50% for malloc/tcmalloc)
2. **p99 latency determinism** - Low variance (no GC pauses, no compaction spikes)
3. **Bounded allocation time** - O(1) class selection eliminates jitter

**Expected trade-offs:**
1. **Internal fragmentation** - 11.1% average (fixed size classes)
2. **Max allocation size** - 768 bytes (not general-purpose)
3. **Peak RSS** - Bounded by high-water mark (never unmaps)

### Red Flags

If you see these, something is wrong:
- RSS growth >10% over churn test
- p99 latency >10µs (indicates slow path hit rate too high)
- Cache overflow >5% of recycled slabs (cache too small)
- Allocation failures (should never happen in tests)

## Hardware Configuration

Benchmarks run on:
- **CPU**: Intel Core Ultra 7 165H (P-cores @ 4.9 GHz)
- **RAM**: DDR5-5600
- **OS**: Linux 6.6.87.2 (WSL2)
- **Compiler**: GCC 13.3.0 -O3

Your results will vary based on hardware, especially:
- CPU cache size (affects lookup table performance)
- Memory bandwidth (affects multi-threaded scaling)
- TLB size (affects mmap overhead in slow path)

## Contributing Benchmarks

When adding new benchmarks:
1. Focus on allocator behavior, not application workload simulation
2. Isolate variables (single size class, controlled thread count)
3. Output CSV for visualization
4. Document what the benchmark measures and why it matters
5. Add to `run_all.sh`

See `src/benchmark_accurate.c` for reference implementation.
