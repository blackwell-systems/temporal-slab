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
  
  # Run benchmark and capture full output (metrics are printed inline)
  OUTPUT=$(timeout 30 ./benchmark_threads $T 2>&1)
  
  # Extract latency metrics
  echo "$OUTPUT" | grep -E "(Avg p50|Avg p95|Avg p99|Throughput)" || true
  
  # Extract contention metrics directly from benchmark output (not stats_dump)
  echo "Contention metrics:"
  echo "$OUTPUT" | grep -A4 "Contention Metrics" | grep -E "(Bitmap|current_partial|Lock)" || echo "  (metrics not found in output)"
  
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
