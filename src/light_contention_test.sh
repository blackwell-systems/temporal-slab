#!/bin/bash
# light_contention_test.sh - Ultra-fast contention validation
#
# Runs minimal thread counts with shorter timeouts for quick regression checks.

set -e

echo "Light Contention Test (Quick Regression Check)"
echo "==============================================="
echo ""

# Function to run benchmark and extract key metrics
show_metrics() {
  local threads=$1
  echo "=== $threads Thread(s) ==="
  
  # Shorter timeout (15s instead of 30s)
  OUTPUT=$(timeout 15 ./benchmark_threads $threads 2>&1)
  
  # Extract just throughput and p99
  echo "$OUTPUT" | grep -E "Throughput" || true
  echo "$OUTPUT" | grep -E "Avg p99" || true
  
  # Extract lock contention rate only
  echo "$OUTPUT" | grep -A4 "Contention Metrics" | grep "Lock contention:" || echo "  (metrics not found)"
  
  echo ""
}

# Test just 1 and 4 threads (faster than 1+8)
show_metrics 1
show_metrics 4

echo "=== Quick Validation ==="
echo "✓ 1-thread should show ~0% contention"
echo "✓ 4-thread should show 5-15% contention"
echo ""
echo "For full validation, run: ./quick_contention_test.sh"
