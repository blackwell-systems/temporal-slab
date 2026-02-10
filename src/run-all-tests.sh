#!/bin/bash
# Complete local regression test suite
# Maps to GitHub Actions workflows for pre-push validation

set -e

RESULTS_DIR="/tmp/zns-slab-test-$$"
mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "ZNS-Slab Complete Regression Test Suite"
echo "=========================================="
echo ""
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# ============================================
# ZNS-Slab Core Tests (ci.yml)
# ============================================
echo "=== Part 1: ZNS-Slab Core Tests ==="
cd /home/blackwd/code/ZNS-Slab/src

echo "Building core library..."
make clean
make slab_alloc.o

echo ""
echo "[1/6] Smoke Tests..."
make smoke_tests
./smoke_tests || { echo "✗ Smoke tests failed"; exit 1; }

echo ""
echo "[2/6] Epoch Tests..."
make test_epochs
./test_epochs || { echo "✗ Epoch tests failed"; exit 1; }

echo ""
echo "[3/6] Malloc Wrapper Tests..."
make test_malloc_wrapper
./test_malloc_wrapper || { echo "✗ Malloc wrapper tests failed"; exit 1; }

echo ""
echo "[4/6] Single-thread Benchmark Sanity..."
make benchmark_threads
timeout 30 ./benchmark_threads 1 > "$RESULTS_DIR/benchmark_1t.log" 2>&1 || { echo "✗ 1-thread benchmark failed"; exit 1; }
tail -15 "$RESULTS_DIR/benchmark_1t.log"

echo ""
echo "[5/6] Multi-thread Contention (8 threads)..."
timeout 60 ./benchmark_threads 8 > "$RESULTS_DIR/benchmark_8t.log" 2>&1 || { echo "✗ 8-thread benchmark failed"; exit 1; }
tail -15 "$RESULTS_DIR/benchmark_8t.log"

echo ""
echo "[6/6] Churn Test..."
make churn_test
timeout 30 ./churn_test > "$RESULTS_DIR/churn_test.log" 2>&1 || { echo "✗ Churn test failed"; exit 1; }
echo "✓ Churn test completed"

echo ""
echo "✓ All ZNS-Slab core tests passed"
echo ""

# ============================================
# Benchmark Suite (benchmark.yml)
# ============================================
echo "=== Part 2: Benchmark Suite ==="
cd /home/blackwd/code/temporal-slab-allocator-bench

echo "Building all workloads..."
make clean
make

echo ""
echo "[1/6] Latency Benchmark (churn_fixed)..."
echo "  temporal_slab (100K objects, 100 cycles)..."
timeout 120 ./workloads/churn_fixed --allocator temporal_slab --size 128 --objects 100000 --cycles 100 \
  --json "$RESULTS_DIR/churn_temporal.json" > "$RESULTS_DIR/churn_temporal.log" 2>&1 || { echo "✗ temporal_slab latency failed"; exit 1; }
  
echo "  system_malloc (100K objects, 100 cycles)..."
timeout 120 ./workloads/churn_fixed --allocator system_malloc --size 128 --objects 100000 --cycles 100 \
  --json "$RESULTS_DIR/churn_malloc.json" > "$RESULTS_DIR/churn_malloc.log" 2>&1 || { echo "✗ system_malloc latency failed"; exit 1; }

temporal_p99=$(jq -r '.latency.alloc_p99_ns' "$RESULTS_DIR/churn_temporal.json")
temporal_p999=$(jq -r '.latency.alloc_p999_ns' "$RESULTS_DIR/churn_temporal.json")
malloc_p99=$(jq -r '.latency.alloc_p99_ns' "$RESULTS_DIR/churn_malloc.json")
malloc_p999=$(jq -r '.latency.alloc_p999_ns' "$RESULTS_DIR/churn_malloc.json")

echo "  Results:"
echo "    temporal_slab: p99=${temporal_p99}ns, p999=${temporal_p999}ns"
echo "    system_malloc: p99=${malloc_p99}ns, p999=${malloc_p999}ns"

echo ""
echo "[2/6] RSS Stability (steady_state_churn)..."
echo "  temporal_slab (100K steady-state, 10% turnover, 100 cycles)..."
timeout 120 ./workloads/steady_state_churn --allocator temporal_slab --size 128 \
  --steady-state 100000 --churn-size 10000 --cycles 100 \
  --csv "$RESULTS_DIR/steady_state_temporal.csv" > "$RESULTS_DIR/steady_state_temporal.log" 2>&1 || { echo "✗ temporal_slab RSS stability failed"; exit 1; }

echo "  system_malloc (100K steady-state, 10% turnover, 100 cycles)..."
timeout 120 ./workloads/steady_state_churn --allocator system_malloc --size 128 \
  --steady-state 100000 --churn-size 10000 --cycles 100 \
  --csv "$RESULTS_DIR/steady_state_malloc.csv" > "$RESULTS_DIR/steady_state_malloc.log" 2>&1 || { echo "✗ system_malloc RSS stability failed"; exit 1; }

echo "✓ RSS stability tests completed"

echo ""
echo "[3/6] Fragmentation Resistance..."
timeout 60 ./workloads/fragmentation_adversary --allocator temporal_slab \
  --min-size 64 --max-size 768 --count 10000 \
  --csv "$RESULTS_DIR/fragmentation_temporal.csv" > "$RESULTS_DIR/fragmentation_temporal.log" 2>&1 || { echo "✗ temporal_slab fragmentation failed"; exit 1; }

timeout 60 ./workloads/fragmentation_adversary --allocator system_malloc \
  --min-size 64 --max-size 768 --count 10000 \
  --csv "$RESULTS_DIR/fragmentation_malloc.csv" > "$RESULTS_DIR/fragmentation_malloc.log" 2>&1 || { echo "✗ system_malloc fragmentation failed"; exit 1; }

echo "✓ Fragmentation tests completed"

echo ""
echo "[4/6] Epoch Reclamation Demo..."
timeout 30 ./workloads/epoch_reclamation_demo > "$RESULTS_DIR/epoch_reclamation.log" 2>&1 || { echo "✗ Epoch reclamation demo failed"; exit 1; }
tail -10 "$RESULTS_DIR/epoch_reclamation.log"

echo ""
echo "[5/6] Phase Shift Retention..."
timeout 60 ./workloads/phase_shift_retention 20 > "$RESULTS_DIR/phase_shift.log" 2>&1 || { echo "✗ Phase shift retention failed"; exit 1; }
tail -20 "$RESULTS_DIR/phase_shift.log"

echo ""
echo "[6/6] Sustained Phase Shifts (quick validation)..."
timeout 60 ./workloads/sustained_phase_shifts 20 5 20 > "$RESULTS_DIR/sustained_phase_shifts.log" 2>&1 || { echo "✗ Sustained phase shifts failed"; exit 1; }
tail -20 "$RESULTS_DIR/sustained_phase_shifts.log"

echo ""
echo "✓ All benchmark tests passed"
echo ""

# ============================================
# Summary
# ============================================
echo "=========================================="
echo "All Tests Passed!"
echo "=========================================="
echo ""
echo "Test results saved to: $RESULTS_DIR"
echo ""
echo "Key metrics (WSL2 baseline):"
echo "  temporal_slab p99:  ${temporal_p99}ns"
echo "  temporal_slab p999: ${temporal_p999}ns"
echo "  system_malloc p99:  ${malloc_p99}ns"
echo "  system_malloc p999: ${malloc_p999}ns"
echo ""
echo "Note: These are WSL2 results for regression detection."
echo "For publication metrics, use GitHub Actions (ubuntu-latest)."
echo ""
echo "To view detailed logs:"
echo "  ls -lh $RESULTS_DIR/"
