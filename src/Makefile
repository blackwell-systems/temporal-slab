CC = gcc
CFLAGS = -O3 -std=c11 -pthread -Wall -Wextra -pedantic -I../include -DENABLE_RSS_RECLAMATION=1
LDFLAGS =
TSAN_FLAGS = -O1 -g -std=c11 -pthread -Wall -Wextra -pedantic -I../include -DENABLE_RSS_RECLAMATION=1 -fsanitize=thread

# Optional drainability profiler integration (disabled by default)
# To enable: make ENABLE_DRAINPROF=1 DRAINPROF_PATH=/path/to/drainability-profiler
ifdef ENABLE_DRAINPROF
  ifndef DRAINPROF_PATH
    DRAINPROF_PATH = ../../drainability-profiler
  endif
  CFLAGS += -DENABLE_DRAINPROF -I$(DRAINPROF_PATH)/include
  LDFLAGS += -L$(DRAINPROF_PATH) -ldrainprof
  TSAN_FLAGS += -DENABLE_DRAINPROF -I$(DRAINPROF_PATH)/include
endif

# Optional features (add to CFLAGS as needed):
#
# TLS Cache (thread-local allocation cache):
#   Build with: make CFLAGS="$(CFLAGS) -DENABLE_TLS_CACHE=1" TLS_OBJ=slab_tls_cache.o
#
# Slowpath Sampling (probabilistic tail latency diagnosis):
#   Build with: make CFLAGS="$(CFLAGS) -DENABLE_SLOWPATH_SAMPLING"
#   Note: Samples 1/1024 allocations, measures wall vs CPU time
#   See: workloads/README_SLOWPATH_SAMPLING.md
#
# Diagnostic Counters (live_bytes tracking):
#   Build with: make CFLAGS="$(CFLAGS) -DENABLE_DIAGNOSTIC_COUNTERS=1"
#
# Lock Rank Debugging (deadlock detection):
#   Build with: make CFLAGS="$(CFLAGS) -DENABLE_LOCK_RANK_DEBUG=1"

TLS_OBJ ?=

all: smoke_tests benchmark_accurate benchmark_threads soak_test churn_test test_malloc_wrapper test_epochs domain_usage stats_dump synthetic_bench

# ThreadSanitizer builds for data race detection
tsan: tsan_test

slab_lib_tsan.o: slab_lib.c
	$(CC) $(TSAN_FLAGS) -c slab_lib.c -o slab_lib_tsan.o

tsan_test: tsan_test.c slab_lib_tsan.o
	$(CC) $(TSAN_FLAGS) tsan_test.c slab_lib_tsan.o $(LDFLAGS) -o tsan_test

slab_stats_tsan.o: slab_stats.c
	$(CC) $(TSAN_FLAGS) -c slab_stats.c -o slab_stats_tsan.o

benchmark_threads_tsan: benchmark_threads.c slab_lib_tsan.o slab_stats_tsan.o
	$(CC) $(TSAN_FLAGS) benchmark_threads.c slab_lib_tsan.o slab_stats_tsan.o $(LDFLAGS) -o benchmark_threads_tsan

# Core library object (implementation only, no main)
slab_lib.c: slab_alloc.c
	sed 's/^static inline //g' slab_alloc.c | \
	sed 's/^static //g' | \
	sed 's/^inline //g' > slab_lib.c

slab_lib.o: slab_lib.c
	$(CC) $(CFLAGS) -c slab_lib.c -o slab_lib.o

# TLS cache module (optional, enabled with -DENABLE_TLS_CACHE=1)
slab_tls_cache.o: slab_tls_cache.c
	$(CC) $(CFLAGS) -c slab_tls_cache.c -o slab_tls_cache.o

# Epoch domain module
epoch_domain.o: epoch_domain.c
	$(CC) $(CFLAGS) -c epoch_domain.c -o epoch_domain.o

# Smoke tests executable
smoke_tests: smoke_tests.c slab_lib.o slab_stats.o $(TLS_OBJ)
	$(CC) $(CFLAGS) smoke_tests.c slab_lib.o slab_stats.o $(TLS_OBJ) $(LDFLAGS) -o smoke_tests

# Accurate benchmark executable
benchmark_accurate: benchmark_accurate.c slab_lib.o $(TLS_OBJ)
	$(CC) $(CFLAGS) benchmark_accurate.c slab_lib.o $(TLS_OBJ) $(LDFLAGS) -o benchmark_accurate

# Multi-threaded scaling benchmark
benchmark_threads: benchmark_threads.c slab_lib.o slab_stats.o $(TLS_OBJ)
	$(CC) $(CFLAGS) benchmark_threads.c slab_lib.o slab_stats.o $(TLS_OBJ) $(LDFLAGS) -o benchmark_threads

# Soak test executable (long-running stability test)
soak_test: soak_test.c slab_lib.o $(TLS_OBJ)
	$(CC) $(CFLAGS) soak_test.c slab_lib.o $(TLS_OBJ) $(LDFLAGS) -o soak_test

# Churn test executable (Phase 2.1 RSS bounds validation)
churn_test: churn_test.c slab_lib.o $(TLS_OBJ)
	$(CC) $(CFLAGS) churn_test.c slab_lib.o $(TLS_OBJ) $(LDFLAGS) -o churn_test

# Malloc wrapper test executable
test_malloc_wrapper: test_malloc_wrapper.c slab_lib.o $(TLS_OBJ)
	$(CC) $(CFLAGS) test_malloc_wrapper.c slab_lib.o $(TLS_OBJ) $(LDFLAGS) -o test_malloc_wrapper

# Epoch functionality tests
test_epochs: test_epochs.c slab_lib.o slab_stats.o $(TLS_OBJ)
	$(CC) $(CFLAGS) test_epochs.c slab_lib.o slab_stats.o $(TLS_OBJ) $(LDFLAGS) -o test_epochs

test_epoch_close: test_epoch_close.c slab_lib.o slab_stats.o $(TLS_OBJ)
	$(CC) $(CFLAGS) test_epoch_close.c slab_lib.o slab_stats.o $(TLS_OBJ) $(LDFLAGS) -o test_epoch_close

test_epoch_metadata: test_epoch_metadata.c slab_lib.o slab_stats.o $(TLS_OBJ)
	$(CC) $(CFLAGS) test_epoch_metadata.c slab_lib.o slab_stats.o $(TLS_OBJ) $(LDFLAGS) -o test_epoch_metadata

# Domain usage examples
domain_usage: ../examples/domain_usage.c slab_lib.o epoch_domain.o $(TLS_OBJ)
	$(CC) $(CFLAGS) ../examples/domain_usage.c slab_lib.o epoch_domain.o $(TLS_OBJ) $(LDFLAGS) -o domain_usage

# Phase 2.0: Observability stats snapshot tool
slab_stats.o: slab_stats.c
	$(CC) $(CFLAGS) -c slab_stats.c -o slab_stats.o

# Phase 3: Actionable diagnostics
slab_diagnostics.o: slab_diagnostics.c
	$(CC) $(CFLAGS) -c slab_diagnostics.c -o slab_diagnostics.o

stats_dump: stats_dump.c slab_lib.o slab_stats.o slab_diagnostics.o $(TLS_OBJ)
	$(CC) $(CFLAGS) stats_dump.c slab_lib.o slab_stats.o slab_diagnostics.o $(TLS_OBJ) $(LDFLAGS) -o stats_dump

# Phase 2.2: Era stamping test
test_era_stamping: test_era_stamping.c slab_lib.o $(TLS_OBJ)
	$(CC) $(CFLAGS) test_era_stamping.c slab_lib.o $(TLS_OBJ) $(LDFLAGS) -o test_era_stamping

# Canonical benchmark harness
synthetic_bench: ../workloads/synthetic_bench.c slab_lib.o slab_stats.o $(TLS_OBJ)
	$(CC) $(CFLAGS) ../workloads/synthetic_bench.c slab_lib.o slab_stats.o $(TLS_OBJ) $(LDFLAGS) -o ../workloads/synthetic_bench

clean:
	rm -f smoke_tests benchmark_accurate benchmark_threads soak_test churn_test test_malloc_wrapper test_epochs test_epoch_close test_epoch_metadata domain_usage stats_dump slab_lib.c slab_lib.o epoch_domain.o slab_stats.o benchmark_threads_tsan slab_lib_tsan.o slab_stats_tsan.o tsan_test ../workloads/synthetic_bench

.PHONY: all clean
