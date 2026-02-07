CC = gcc
CFLAGS = -O3 -std=c11 -pthread -Wall -Wextra -pedantic -I../include -DENABLE_RSS_RECLAMATION=1

all: smoke_tests benchmark_accurate benchmark_threads soak_test churn_test test_malloc_wrapper domain_usage

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

clean:
	rm -f smoke_tests benchmark_accurate benchmark_threads soak_test churn_test test_malloc_wrapper domain_usage slab_lib.c slab_lib.o epoch_domain.o

.PHONY: all clean
