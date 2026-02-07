#!/bin/bash
# run_comparison.sh - Systematic malloc vs tslab comparison
#
# Runs all 5 patterns with both allocators and captures metrics.
# Assumes observability stack is running and push-metrics.sh is active.

set -e

DURATION=60  # seconds per run
PATTERNS=("burst" "steady" "leak" "hotspot" "kernel")
RESULTS_DIR="comparison_results_$(date +%Y%m%d_%H%M%S)"

mkdir -p "$RESULTS_DIR"

echo "Malloc vs tslab comparison"
echo "=========================="
echo "Duration: ${DURATION}s per pattern"
echo "Results directory: $RESULTS_DIR"
echo ""

for pattern in "${PATTERNS[@]}"; do
    echo "=== Pattern: $pattern ==="
    
    # Run with malloc
    echo "  Running malloc..."
    ./synthetic_bench --allocator=malloc --pattern="$pattern" --duration_s="$DURATION" \
        > "$RESULTS_DIR/${pattern}_malloc.txt" 2>&1
    
    # Wait for metrics to stabilize
    sleep 5
    
    # Capture malloc metrics snapshot
    curl -s http://localhost:9091/metrics > "$RESULTS_DIR/${pattern}_malloc_metrics.txt"
    
    # Wait between runs
    sleep 10
    
    # Run with tslab
    echo "  Running tslab..."
    ./synthetic_bench --allocator=tslab --pattern="$pattern" --duration_s="$DURATION" \
        > "$RESULTS_DIR/${pattern}_tslab.txt" 2>&1
    
    # Wait for metrics to stabilize
    sleep 5
    
    # Capture tslab metrics snapshot
    curl -s http://localhost:9091/metrics > "$RESULTS_DIR/${pattern}_tslab_metrics.txt"
    
    # Wait between patterns
    sleep 10
    
    echo "  Complete"
    echo ""
done

echo "=== Comparison complete ==="
echo "Results saved to: $RESULTS_DIR"
echo ""
echo "Next steps:"
echo "1. Analyze results: cd $RESULTS_DIR && ls -lh"
echo "2. Compare outputs: diff ${pattern}_malloc.txt ${pattern}_tslab.txt"
echo "3. Extract key metrics: grep 'Request rate\|Allocation rate\|Objects leaked' *.txt"
