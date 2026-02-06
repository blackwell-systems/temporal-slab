CC = gcc
CFLAGS = -O3 -std=c11 -pthread -Wall -Wextra -pedantic -I../include

all: smoke_tests benchmark_accurate soak_test churn_test test_malloc_wrapper

# Core library object (implementation only, no main)
slab_lib.c: slab_alloc.c
	sed 's/^static inline //g' slab_alloc.c | \
	sed 's/^static //g' | \
	sed 's/^inline //g' > slab_lib.c

slab_lib.o: slab_lib.c
	$(CC) $(CFLAGS) -c slab_lib.c -o slab_lib.o

# Smoke tests executable
smoke_tests: smoke_tests.c slab_lib.o
	$(CC) $(CFLAGS) smoke_tests.c slab_lib.o -o smoke_tests

# Accurate benchmark executable
benchmark_accurate: benchmark_accurate.c slab_lib.o
	$(CC) $(CFLAGS) benchmark_accurate.c slab_lib.o -o benchmark_accurate

# Soak test executable (long-running stability test)
soak_test: soak_test.c slab_lib.o
	$(CC) $(CFLAGS) soak_test.c slab_lib.o -o soak_test

# Churn test executable (Phase 2.1 RSS bounds validation)
churn_test: churn_test.c slab_lib.o
	$(CC) $(CFLAGS) churn_test.c slab_lib.o -o churn_test

# Malloc wrapper test executable
test_malloc_wrapper: test_malloc_wrapper.c slab_lib.o
	$(CC) $(CFLAGS) test_malloc_wrapper.c slab_lib.o -o test_malloc_wrapper

clean:
	rm -f smoke_tests benchmark_accurate soak_test churn_test test_malloc_wrapper slab_lib.c slab_lib.o

.PHONY: all clean
