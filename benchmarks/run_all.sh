#!/bin/bash
#
# Run all benchmarks and generate visualizations
#
# Usage:
#   ./run_all.sh                    # Run benchmarks, generate charts
#   ./run_all.sh --baseline malloc  # Save as baseline comparison
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_ROOT/src"
RESULTS_DIR="$SCRIPT_DIR/results"
TOOLS_DIR="$PROJECT_ROOT/tools"
IMAGES_DIR="$PROJECT_ROOT/docs/images"

# Parse arguments
BASELINE=""
if [[ "$1" == "--baseline" ]] && [[ -n "$2" ]]; then
    BASELINE="$2"
    RESULTS_DIR="$SCRIPT_DIR/baseline/$BASELINE/results"
    mkdir -p "$RESULTS_DIR"
    echo "Running in baseline mode: $BASELINE"
    echo "Results will be saved to: $RESULTS_DIR"
fi

# Ensure binaries are built
echo "Building benchmarks..."
cd "$SRC_DIR"
make -s benchmark_accurate churn_test smoke_tests

# Create results directory
mkdir -p "$RESULTS_DIR"

# Run benchmarks
echo ""
echo "Running benchmark_accurate..."
./benchmark_accurate --csv "$RESULTS_DIR/latency.csv" | grep -E "(Average|p50|p99|p999|RSS|Summary|efficiency)"

echo ""
echo "Running churn_test (this takes ~30 seconds)..."
if [[ -f ./churn_test ]]; then
    timeout 60 ./churn_test 2>&1 | grep -E "(RSS|cycle.*999|PASS|FAIL)" || true
else
    echo "  (churn_test not available)"
fi

echo ""
echo "Running smoke_tests..."
timeout 10 ./smoke_tests 2>&1 | grep -E "(OK|FAIL)" || echo "  (smoke tests timed out or not available)"

# Generate visualizations (only for main run, not baselines)
if [[ -z "$BASELINE" ]]; then
    echo ""
    echo "Generating visualizations..."
    cd "$PROJECT_ROOT"
    
    # Check if matplotlib is available
    if python3 -c "import matplotlib" 2>/dev/null; then
        python3 "$TOOLS_DIR/plot_bench.py" --input "$RESULTS_DIR" --output "$IMAGES_DIR"
        echo ""
        echo "✓ Charts generated in $IMAGES_DIR"
        echo "  View them in docs/results.md"
    else
        echo ""
        echo "⚠ matplotlib not installed - skipping chart generation"
        echo "  Install with: pip install -r tools/requirements.txt"
    fi
else
    echo ""
    echo "✓ Baseline results saved to: $RESULTS_DIR"
    echo "  Use tools/compare_allocators.py to compare baselines (coming soon)"
fi

echo ""
echo "=== Benchmark run complete ==="
