CC = gcc
CFLAGS = -O3 -std=c11 -pthread -Wall -Wextra -pedantic

all: slab_alloc benchmark_accurate

# Extract library functions (everything before main) and remove static/inline qualifiers
slab_lib.c: slab_alloc.c
	sed '/^int main(void)/,$$d' slab_alloc.c | \
	sed 's/^static inline //g' | \
	sed 's/^static //g' | \
	sed 's/^inline //g' > slab_lib.c

slab_lib.o: slab_lib.c
	$(CC) $(CFLAGS) -c slab_lib.c -o slab_lib.o

slab_alloc: slab_alloc.c
	$(CC) $(CFLAGS) slab_alloc.c -o slab_alloc

benchmark_accurate: benchmark_accurate.c slab_lib.o
	$(CC) $(CFLAGS) benchmark_accurate.c slab_lib.o -o benchmark_accurate

clean:
	rm -f slab_alloc benchmark_accurate slab_lib.c slab_lib.o

.PHONY: all clean
