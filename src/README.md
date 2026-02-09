# temporal-slab Source Directory

This directory contains the core allocator implementation, observability tools, and test suite.

## Core Implementation

### Production Code
- **`slab_alloc.c`** - Main allocator implementation (lock-free fast path, epoch-based lifetimes, FULL-only recycling)
- **`slab_alloc.h`** - Public API (in `../include/`, symlinked for convenience)
- **`slab_alloc_internal.h`** - Internal structures, state machines, and implementation details
- **`epoch_domain.c`** - RAII-style scoped lifetime management (optional higher-level API)
- **`slab_stats.c`** - Performance counter collection and metrics aggregation
- **`slab_diagnostics.c`** - Actionable diagnostics for production debugging

### Build Artifacts
- **`slab_lib.c`** - Generated from `slab_alloc.c` (strips `static` for linkability)
- **`slab_lib.o`** - Compiled library object (linked into tests and benchmarks)
- **`*.o`** - Compiled object files (module-level compilation units)

## Observability Tools

### Production Utilities
- **`stats_dump`** - Snapshot allocator metrics to JSON or human-readable format
  - Usage: `./stats_dump --no-text` (JSON only) or `./stats_dump` (human-readable summary)
  - Output: RSS, epoch age, slow-path rate, contention counters, cache statistics

## Test Suite

### Unit Tests (Correctness)
These tests validate core allocator behavior and safety guarantees.

- **`smoke_tests.c`** → `./smoke_tests`
  - Single-thread and multi-thread correctness (8 threads × 500K operations)
  - Handle validation (stale/double-free safety)
  - Basic API contract verification
  - **Status:** Runs in CI on every commit

- **`test_epochs.c`** → `./test_epochs`
  - Epoch isolation and lifecycle validation
  - Ensures objects allocated in different epochs use separate slabs
  - **Status:** Runs in CI on every commit

- **`test_malloc_wrapper.c`** → `./test_malloc_wrapper`
  - Malloc-style API correctness (`slab_malloc`, `slab_free`)
  - NULL-safety and API contract tests
  - **Status:** Runs in CI on every commit

### Integration Tests (Long-Running Validation)
These tests validate allocator behavior under sustained load.

- **`churn_test.c`** → `./churn_test`
  - RSS bounds validation under sustained churn (1000 cycles)
  - Measures RSS growth: 0-2.4% expected
  - **Status:** Runs in CI with 30s timeout (sanity check only)

- **`soak_test.c`** → `./soak_test`
  - Long-running stability test (hours-scale runtime)
  - Validates no memory leaks or RSS drift over time
  - **Status:** Manual testing only (not in CI due to runtime)

### Feature-Specific Tests
These tests validate specific Phase 2/3 features and are used during development.

- **`test_epoch_close.c`** → `./test_epoch_close`
  - Phase 2: RSS reclamation via `epoch_close()` API
  - Demonstrates `madvise(MADV_DONTNEED)` reclamation
  - Validates 19.15 MiB reclamation across 4,903 slabs

- **`test_epoch_metadata.c`** → `./test_epoch_metadata`
  - Phase 2: Epoch metadata tracking and lifecycle
  - Validates epoch state transitions (ACTIVE → CLOSING → CLOSED)

- **`test_rss_delta.c`** → `./test_rss_delta`
  - Phase 2: RSS measurement accuracy and reclamation verification
  - Tracks RSS deltas after `madvise()` calls

- **`test_era_stamping.c`** → `./test_era_stamping`
  - Phase 2.2: Era stamping for temporal cache tiering
  - Validates era assignment and tracking

- **`test_diagnostics.c`** → `./test_diagnostics`
  - Phase 3: Actionable diagnostics API
  - Tests diagnostic output formats and thresholds

- **`test_label_cardinality.c`** → `./test_label_cardinality`
  - Phase 3: Label-based metrics tracking
  - Validates per-label statistics collection

- **`test_public_api.c`** → `./test_public_api`
  - Public API surface validation
  - Ensures only intended symbols are exported

### Thread Safety Validation

- **`tsan_test.c`** → `./tsan_test` (built with ThreadSanitizer)
  - Data race detection under concurrent load
  - Validates lock-free algorithms are race-free
  - **Build:** `make tsan` (requires `-fsanitize=thread`)
  - **Documentation:** See `TSAN_VALIDATION.md`

## Benchmarks

### Performance Measurement
- **`benchmark_accurate.c`** → `./benchmark_accurate`
  - Single-thread latency measurement (100M allocations)
  - Reports p50/p99/p999 percentiles
  - Used for local performance validation

- **`benchmark_threads.c`** → `./benchmark_threads`
  - Multi-threaded scaling benchmark (1, 2, 4, 8, 16 threads)
  - Reports contention metrics (lock contention %, CAS retry rate)
  - Used in GitHub Actions CI for contention validation
  - **Output:** Per-size-class contention metrics, throughput, latency percentiles

### Domain Usage Examples
- **`domain_usage`** (built from `../examples/domain_usage.c`)
  - Example: RAII-style epoch domain usage
  - Demonstrates request-scoped allocation patterns

## Shell Scripts

### Contention Testing
- **`test_contention.sh`** - Multi-thread contention validation script
- **`quick_contention_test.sh`** - Fast contention smoke test
- **`demo_threading.sh`** - Interactive threading demonstration

## Build System

- **`Makefile`** - Builds all tests, benchmarks, and utilities
  - Default target (`make`): Builds core test suite + benchmarks
  - TSAN target (`make tsan`): Builds ThreadSanitizer-instrumented tests

### Build Targets (Default)
```bash
make                     # Build all default targets
./smoke_tests           # Run correctness tests
./test_epochs           # Run epoch isolation tests  
./test_malloc_wrapper   # Run malloc API tests
./churn_test            # Run RSS bounds validation (long-running)
./benchmark_accurate    # Run latency benchmark
./benchmark_threads 4   # Run 4-thread contention benchmark
./stats_dump            # Dump current allocator stats
```

### Build Targets (ThreadSanitizer)
```bash
make tsan               # Build TSAN-instrumented tests
./tsan_test             # Run data race detection
```

## File Status Summary

### Active (CI-Validated)
- ✅ `slab_alloc.c` - Core implementation
- ✅ `smoke_tests.c` - Runs on every commit
- ✅ `test_epochs.c` - Runs on every commit
- ✅ `test_malloc_wrapper.c` - Runs on every commit
- ✅ `churn_test.c` - Runs with timeout in CI
- ✅ `benchmark_threads.c` - Used in benchmark repo CI

### Active (Manual Testing)
- 🔧 `benchmark_accurate.c` - Local performance validation
- 🔧 `soak_test.c` - Long-running stability testing
- 🔧 `stats_dump.c` - Production observability tool
- 🔧 `tsan_test.c` - ThreadSanitizer validation

### Development/Feature-Specific
- 📦 `test_epoch_close.c` - Phase 2 feature validation
- 📦 `test_epoch_metadata.c` - Phase 2 development
- 📦 `test_rss_delta.c` - Phase 2 RSS measurement
- 📦 `test_era_stamping.c` - Phase 2.2 tiering feature
- 📦 `test_diagnostics.c` - Phase 3 observability
- 📦 `test_label_cardinality.c` - Phase 3 metrics
- 📦 `test_public_api.c` - API surface validation

## Test Classification

### Unit Tests (Fast, CI-Required)
- `smoke_tests.c` - Core correctness
- `test_epochs.c` - Epoch isolation
- `test_malloc_wrapper.c` - API contract

### Integration Tests (Slow, Manual)
- `churn_test.c` - RSS stability (1000 cycles, ~5 minutes)
- `soak_test.c` - Long-term stability (hours-scale)

### Feature Tests (Development)
- All `test_epoch_*.c`, `test_diagnostics.c`, `test_label_*.c` files
- Used during feature development, not required for base allocator correctness

### Benchmarks (Performance)
- `benchmark_accurate.c` - Single-thread latency
- `benchmark_threads.c` - Multi-thread contention

## Cleanup Opportunities

### Candidates for Removal
The following files may be obsolete depending on project phase:

1. **Old build artifacts:**
   - `slab_lib_debug.o`, `slab_lib_labeled.o` - Likely leftover from debugging sessions
   - Consider: Clean via `make clean`

2. **Feature-specific tests:**
   - If Phase 2/3 features are stable, consider moving to `archive/` or separate test directory
   - Candidates: `test_epoch_close.c`, `test_epoch_metadata.c`, `test_rss_delta.c`, `test_era_stamping.c`

3. **Development scripts:**
   - `demo_threading.sh`, `test_contention.sh`, `quick_contention_test.sh` - May belong in `scripts/` or `tools/`

### Recommendation
Before removing anything:
1. Verify current phase completion status (Phase 2.x vs Phase 3)
2. Check if any tests validate guarantees mentioned in README
3. Consider moving to `tests/archive/` rather than deleting

## Performance Results

**Latency (GitHub Actions ubuntu-latest, AMD EPYC 7763, 128-byte objects, 5 trials):**
- Allocation: **40ns** p50, **131ns** p99, **371ns** p999, **3,246ns** p9999
- 11.2× better p99 vs system_malloc (131ns vs 1,463ns)
- 11.9× better p999 vs system_malloc (371ns vs 4,418ns)
- 2.4× better p9999 vs system_malloc (3,246ns vs 7,935ns)

**RSS Stability (GitHub Actions, 100 cycles steady-state churn):**
- temporal-slab: **0.00%** RSS growth
- system_malloc: Unbounded drift

**Thread Contention (GitHub Actions ubuntu-latest, 128B class, 10 trials):**
- 1 thread: 0.00% lock contention, 0.0000 CAS retry rate
- 4 threads: 10.96% lock contention (median), 0.0025 CAS retry rate
- 8 threads: 13.19% lock contention (median), 0.0033 CAS retry rate
- 16 threads: 14.78% lock contention (median), 0.0074 CAS retry rate
- Healthy scaling: Contention plateaus at 15%, no pathological growth

See [../docs/results.md](../docs/results.md) for full benchmark analysis.
