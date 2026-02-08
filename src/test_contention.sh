#!/bin/bash
# test_contention.sh - Multi-thread contention validation
#
# Runs benchmark_threads at various thread counts and extracts contention metrics.
# Validates that CAS retry rates and lock contention increase with thread count.

set -e

echo "Phase 2.2 Contention Metrics Validation"
echo "========================================"
echo ""

# Thread counts to test
THREADS=(1 2 4 8)

echo "Running multi-threaded benchmarks..."
echo ""

for T in "${THREADS[@]}"; do
  echo "=== Testing with $T threads ==="
  
  # Run benchmark (captures perf in benchmark itself)
  ./benchmark_threads $T 2>&1 | grep -E "(Thread|p50|p95|p99|Aggregate)" || true
  
  # Extract contention metrics from stats_dump
  echo "Contention metrics (size class 128B):"
  ./stats_dump --no-text 2>/dev/null | jq -r --arg size "128" '
    .classes[] | 
    select(.object_size == 128) | 
    "  CAS alloc retries/attempt: \(.avg_alloc_cas_retries_per_attempt | . * 100 | floor / 100)\n  CAS free retries/attempt: \(.avg_free_cas_retries_per_attempt | . * 100 | floor / 100)\n  current_partial CAS failure rate: \(.current_partial_cas_failure_rate | . * 100 | floor / 100)%\n  Lock contention rate: \(.lock_contention_rate | . * 100 | floor / 100)%\n  Lock fast acquire: \(.lock_fast_acquire)\n  Lock contended: \(.lock_contended)"
  '
  
  echo ""
done

echo "=== Contention Scaling Summary ==="
echo "Expected behavior:"
echo "  1 thread:  ~0% contention (all fast paths)"
echo "  2 threads: <5% contention (occasional races)"
echo "  4 threads: 5-15% contention (moderate)"
echo "  8 threads: 15-30% contention (high, but not pathological)"
echo ""
echo "If lock contention >50% at 8 threads: Consider per-thread caching"
echo "If CAS retry rate >1.0: Pathological contention (investigate)"
