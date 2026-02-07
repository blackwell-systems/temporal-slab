CC = gcc
CFLAGS = -O3 -std=c11 -pthread -Wall -Wextra -pedantic -I../include -DENABLE_RSS_RECLAMATION=1
TSAN_FLAGS = -O1 -g -std=c11 -pthread -Wall -Wextra -pedantic -I../include -DENABLE_RSS_RECLAMATION=1 -fsanitize=thread

all: smoke_tests benchmark_accurate benchmark_threads soak_test churn_test test_malloc_wrapper domain_usage stats_dump

# ThreadSanitizer builds for data race detection
tsan: tsan_test

slab_lib_tsan.o: slab_lib.c
	$(CC) $(TSAN_FLAGS) -c slab_lib.c -o slab_lib_tsan.o

tsan_test: tsan_test.c slab_lib_tsan.o
	$(CC) $(TSAN_FLAGS) tsan_test.c slab_lib_tsan.o -o tsan_test

benchmark_threads_tsan: benchmark_threads.c slab_lib_tsan.o
	$(CC) $(TSAN_FLAGS) benchmark_threads.c slab_lib_tsan.o -o benchmark_threads_tsan

# Core library object (implementation only, no main)
slab_lib.c: slab_alloc.c
	sed 's/^static inline //g' slab_alloc.c | \
	sed 's/^static //g' | \
	sed 's/^inline //g' > slab_lib.c

slab_lib.o: slab_lib.c
	$(CC) $(CFLAGS) -c slab_lib.c -o slab_lib.o

# Epoch domain module
epoch_domain.o: epoch_domain.c
	$(CC) $(CFLAGS) -c epoch_domain.c -o epoch_domain.o

# Smoke tests executable
smoke_tests: smoke_tests.c slab_lib.o
	$(CC) $(CFLAGS) smoke_tests.c slab_lib.o -o smoke_tests

# Accurate benchmark executable
benchmark_accurate: benchmark_accurate.c slab_lib.o
	$(CC) $(CFLAGS) benchmark_accurate.c slab_lib.o -o benchmark_accurate

# Multi-threaded scaling benchmark
benchmark_threads: benchmark_threads.c slab_lib.o
	$(CC) $(CFLAGS) benchmark_threads.c slab_lib.o -o benchmark_threads

# Soak test executable (long-running stability test)
soak_test: soak_test.c slab_lib.o
	$(CC) $(CFLAGS) soak_test.c slab_lib.o -o soak_test

# Churn test executable (Phase 2.1 RSS bounds validation)
churn_test: churn_test.c slab_lib.o
	$(CC) $(CFLAGS) churn_test.c slab_lib.o -o churn_test

# Malloc wrapper test executable
test_malloc_wrapper: test_malloc_wrapper.c slab_lib.o
	$(CC) $(CFLAGS) test_malloc_wrapper.c slab_lib.o -o test_malloc_wrapper

# Domain usage examples
domain_usage: ../examples/domain_usage.c slab_lib.o epoch_domain.o
	$(CC) $(CFLAGS) ../examples/domain_usage.c slab_lib.o epoch_domain.o -o domain_usage

# Phase 2.0: Observability stats snapshot tool
slab_stats.o: slab_stats.c
	$(CC) $(CFLAGS) -c slab_stats.c -o slab_stats.o

# Phase 3: Actionable diagnostics
slab_diagnostics.o: slab_diagnostics.c
	$(CC) $(CFLAGS) -c slab_diagnostics.c -o slab_diagnostics.o

stats_dump: stats_dump.c slab_lib.o slab_stats.o slab_diagnostics.o
	$(CC) $(CFLAGS) stats_dump.c slab_lib.o slab_stats.o slab_diagnostics.o -o stats_dump

# Phase 2.2: Era stamping test
test_era_stamping: test_era_stamping.c slab_lib.o
	$(CC) $(CFLAGS) test_era_stamping.c slab_lib.o -o test_era_stamping

clean:
	rm -f smoke_tests benchmark_accurate benchmark_threads soak_test churn_test test_malloc_wrapper domain_usage stats_dump slab_lib.c slab_lib.o epoch_domain.o slab_stats.o benchmark_threads_tsan slab_lib_tsan.o tsan_test

.PHONY: all clean
