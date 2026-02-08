#!/bin/bash
# quick_contention_test.sh - Fast contention validation
#
# Runs shorter benchmark iterations to quickly validate contention metrics.

set -e

echo "Quick Contention Validation (Phase 2.2)"
echo "========================================"
echo ""

# Function to extract and display contention metrics
show_metrics() {
  local threads=$1
  echo "[$threads threads] Contention metrics:"
  ./stats_dump --no-text 2>/dev/null | jq -r '
    .classes[2] | 
    "  Alloc attempts: \(.bitmap_alloc_attempts), CAS retries: \(.bitmap_alloc_cas_retries) → \(.avg_alloc_cas_retries_per_attempt) retries/op\n  Free attempts: \(.bitmap_free_attempts), CAS retries: \(.bitmap_free_cas_retries) → \(.avg_free_cas_retries_per_attempt) retries/op\n  Lock fast: \(.lock_fast_acquire), contended: \(.lock_contended) → \(.lock_contention_rate * 100 | floor / 100)% blocked"
  '
  echo ""
}

# Test 1 thread (baseline - expect zero contention)
echo "=== 1 Thread (Baseline) ==="
./benchmark_threads 1 >/dev/null 2>&1
show_metrics 1

# Test 8 threads (high contention - expect CAS retries + lock blocking)
echo "=== 8 Threads (High Contention) ==="
./benchmark_threads 8 >/dev/null 2>&1
show_metrics 8

echo "=== Validation ==="
echo "✓ Check that 8-thread run shows higher metrics than 1-thread"
echo "✓ CAS retry rate should be >0 for 8 threads"
echo "✓ Lock contention rate should be >0% for 8 threads"
