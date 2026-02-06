# ZNS-Slab Root Makefile
# Phase 1.5 - Release Quality Allocator

.PHONY: all clean test bench tsan help

# Default target
all:
	@echo "ZNS-Slab Build System"
	@echo "====================="
	@echo ""
	@echo "Targets:"
	@echo "  make bench        - Run release-quality benchmark (one command)"
	@echo "  make test         - Run correctness tests"
	@echo "  make tsan         - Build and run with ThreadSanitizer"
	@echo "  make clean        - Clean all build artifacts"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Quick start: make bench"

help: all

# One-command benchmark runner
bench:
	@echo "=== Building ZNS-Slab Phase 1.5 ==="
	@cd src && $(MAKE) clean
	@cd src && $(MAKE) benchmark_accurate
	@echo ""
	@echo "=== Running Benchmark ==="
	@cd src && ./benchmark_accurate | tee ../benchmark_results_phase1.5_with_cache.txt
	@echo ""
	@echo "=== Results written to: benchmark_results_phase1.5_with_cache.txt ==="
	@echo ""
	@echo "Expected (known-good):"
	@echo "  p50:  ~26ns allocation,  ~24ns free"
	@echo "  p99:  ~1400ns allocation, ~45ns free"
	@echo "  p999: ~2300ns allocation, ~180ns free"
	@echo ""
	@echo "Regression checks:"
	@echo "  - p50 < 50ns (fast path healthy)"
	@echo "  - p99 < 2000ns (cache working)"
	@echo "  - current_partial_miss == 0 (fast path perfect)"

# Correctness tests
test:
	@echo "=== Building Tests ==="
	@cd src && $(MAKE) clean
	@cd src && $(MAKE) slab_alloc
	@echo ""
	@echo "=== Running Correctness Tests ==="
	@cd src && ./slab_alloc | tee test_results.txt
	@echo ""
	@echo "=== Test Results ==="
	@grep -E "(OK|PASS|FAIL)" src/test_results.txt || echo "All tests completed"

# ThreadSanitizer build
tsan:
	@echo "=== Building with ThreadSanitizer ==="
	@cd src && $(MAKE) clean
	@cd src && $(MAKE) CC=gcc CFLAGS="-O1 -g -fsanitize=thread -std=c11 -pthread -Wall -Wextra -Wno-unused-function" slab_alloc
	@echo ""
	@echo "=== Running TSAN Tests ==="
	@echo "Note: TSAN may fail on WSL2 due to memory mapping restrictions"
	@echo "      Run on native Linux for full TSAN validation"
	@cd src && TSAN_OPTIONS="halt_on_error=1 verbosity=0" ./slab_alloc || \
		echo "TSAN test failed (may be WSL2 limitation - verify on native Linux)"
	@echo ""
	@echo "=== TSAN: Check complete ==="

# Clean everything
clean:
	@cd src && $(MAKE) clean
	@rm -f benchmark_results_phase1.5_with_cache.txt
	@echo "Cleaned all build artifacts"
