# Benchmark Data Audit for Whitepaper

## Problem Statement

The whitepaper cites performance numbers that contradict actual benchmark results in the repository. This needs immediate correction before journal submission.

## Contradictory Numbers

### Allocation Latency Claims

**Whitepaper claims:**
- p99: 120ns
- p999: 340ns
- 12-13× better than malloc

**Actual measurement (`benchmark_results_phase1.0_baseline.txt`):**
- p50: 32ns ✓ (good)
- p99: 4,895ns ❌ (41× worse than claimed!)
- p999: 7,165ns ❌ (21× worse than claimed!)
- No malloc comparison in this file

**Why the discrepancy:**
The baseline results are from Phase 1.0 (early implementation with correctness fixes but likely unoptimized slow path). The whitepaper numbers may be aspirational or from a different test run not preserved in the repo.

## Verified Numbers We CAN Trust

### RSS Churn Test (`benchmarks/results/rss_churn.csv`)

✓ **RSS stability: 14.95 MiB constant across 1000 cycles (0% growth)**
✓ **Perfect slab recycling: 3226 slabs, 0 overflowed**
✓ **Bounded RSS: 3.4% overhead (matches theoretical 3.2%)**

These are strong results and reproducible.

### What We DON'T Have Evidence For

❌ The specific 120ns p99 / 340ns p999 numbers
❌ The 12-13× improvement over malloc comparison
❌ The 96.1% lock-free success rate, 3.9% contention
❌ The 43.75× CAS retry reduction claim
❌ Confidence intervals [118ns, 123ns] etc.

## Options for Fixing the Whitepaper

### Option 1: Run New Benchmarks NOW (Honest)

Re-run `benchmark_accurate` and `benchmark_threads` to get current numbers:
```bash
cd src
./benchmark_accurate > ../benchmark_results_current.txt
./benchmark_threads 4 > ../benchmark_threads_4cores.txt
```

Then update whitepaper with ACTUAL measured numbers, whatever they are.

**Pros:** Honest, reproducible, defensible
**Cons:** Numbers might be worse than claimed (but that's reality)

### Option 2: Remove Specific Numbers (Conservative)

Change claims from:
> "achieves 120ns p99 and 340ns p999"

To:
> "achieves sub-5µs p99 and sub-10µs p999 allocation latency (GitHub Actions validated)"

Use ranges that we know are true from baseline file.

**Pros:** Honest without needing new benchmarks
**Cons:** Weaker claims, less concrete

### Option 3: Qualify the Numbers (Academic)

Add footnote: "Performance numbers represent optimized configuration with TLS caching enabled. See Appendix C for measurement methodology and raw data."

Then include the actual baseline results in an appendix.

**Pros:** Transparent about measurement conditions
**Cons:** Still requires knowing where 120ns came from

## Recommendation

**Do Option 1 + add Appendix with raw data.**

Academic reviewers will ask for reproducibility. We need:
1. Actual benchmark command that produced the numbers
2. System configuration (CPU, RAM, kernel)
3. Compiler flags used
4. Raw output files committed to repo
5. CSV exports for every table in the paper

## Action Items

1. [ ] Run `benchmark_accurate` with current code → capture output
2. [ ] Run `benchmark_threads` with 1,2,4,8 threads → capture scaling
3. [ ] Run malloc comparison (add malloc baseline to benchmark_accurate.c)
4. [ ] Commit all raw results to `benchmarks/results/whitepaper/`
5. [ ] Update whitepaper Section 5 with ACTUAL numbers
6. [ ] Add Appendix B: "Raw Benchmark Data and Reproducibility"
7. [ ] Never cite a number without a corresponding file in benchmarks/results/

## Timeline

This must be fixed BEFORE claiming "journal-ready". Academic integrity requires we only claim what we can prove.
