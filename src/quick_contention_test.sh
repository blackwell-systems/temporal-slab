#!/bin/bash
# quick_contention_test.sh - Fast contention validation
#
# Runs shorter benchmark iterations to quickly validate contention metrics.

set -e

echo "Quick Contention Validation (Phase 2.2)"
echo "========================================"
echo ""

# Function to run benchmark and extract contention metrics from inline output
show_metrics() {
  local threads=$1
  echo "=== $threads Thread(s) ==="
  
  OUTPUT=$(timeout 30 ./benchmark_threads $threads 2>&1)
  
  # Extract throughput and latency
  echo "$OUTPUT" | grep -E "(Throughput|Avg p50|Avg p99)" || true
  
  # Extract contention metrics (printed inline by benchmark)
  echo "Contention metrics:"
  echo "$OUTPUT" | grep -A4 "Contention Metrics" | grep -E "(Bitmap|current_partial|Lock)" || echo "  (metrics not found)"
  
  echo ""
}

# Test 1 thread (baseline - expect zero contention)
show_metrics 1

# Test 8 threads (high contention - expect CAS retries + lock blocking)
show_metrics 8

echo "=== Validation ==="
echo "✓ Check that 8-thread run shows higher metrics than 1-thread"
echo "✓ CAS retry rate should be >0 for 8 threads"
echo "✓ Lock contention rate should be >0% for 8 threads"
