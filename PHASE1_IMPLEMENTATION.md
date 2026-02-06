# Phase 1 Implementation Guide

## Goal

Build the core slab allocator with benchmark harness. No hash table, no eviction, no network protocol—just the allocator.

**Deliverables**:
1. Working slab allocator (alloc/free)
2. Baseline benchmarks (malloc vs slab)
3. Memory overhead measurements
4. Latency micro-benchmarks

---

## Language Choice: C

**Why C for Phase 1**:
- Direct memory control (mmap, pointer arithmetic)
- No runtime overhead (no GC, no hidden allocations)
- Easy integration with benchmarking tools (perf, valgrind)
- Portable baseline before considering Rust

**Compiler**: GCC 9+ or Clang 10+  
**Standard**: C11 (`-std=c11`)  
**Flags**: `-O2 -Wall -Wextra -Werror`

---

## File Structure

```
src/
├── slab.h              # Public API
├── slab.c              # Core implementation
├── bitmap.h            # Bitmap operations
├── bitmap.c            # Bitmap implementation
└── slab_internal.h     # Internal structures

benchmarks/
├── baseline_malloc.c   # malloc baseline
├── slab_alloc_bench.c  # slab allocator benchmark
├── measure_rss.c       # RSS measurement utility
└── run_benchmarks.sh   # Automation script

tests/
├── test_bitmap.c       # Bitmap unit tests
├── test_slab.c         # Slab unit tests
└── run_tests.sh        # Test runner
```

---

## Core Data Structures (C)

### slab_internal.h

```c
#ifndef SLAB_INTERNAL_H
#define SLAB_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

// Slab header (64 bytes, cache-line aligned)
typedef struct slab_header {
    uint32_t magic;           // 0x534C4142 ("SLAB")
    uint32_t version;         // 1
    uint32_t object_size;     // 64, 128, 256, or 512
    uint32_t object_count;    // Calculated: (4096 - 192) / object_size
    uint32_t free_count;      // Number of free slots
    uint32_t padding1;
    struct slab *next;        // Next slab in list (8 bytes on 64-bit)
    uint8_t reserved[32];     // Pad to 64 bytes
} __attribute__((aligned(64))) slab_header_t;

// Slab structure (4096 bytes total)
typedef struct slab {
    slab_header_t header;     // 64 bytes
    uint8_t bitmap[128];      // 128 bytes (1024 bits max)
    uint8_t data[];           // 3904 bytes (flexible array member)
} __attribute__((aligned(4096))) slab_t;

// Slab allocator (per size class)
typedef struct slab_allocator {
    uint32_t size_class;      // Object size
    uint32_t objects_per_slab;
    slab_t *free_slabs;       // Slabs with free slots
    slab_t *full_slabs;       // Slabs with no free slots
    uint64_t total_slabs;
    uint64_t total_allocated;
} slab_allocator_t;

#endif
```

### slab.h (Public API)

```c
#ifndef SLAB_H
#define SLAB_H

#include <stddef.h>

// Opaque handle
typedef struct slab_pool slab_pool_t;

// Create slab pool for specific object size
slab_pool_t *slab_pool_create(size_t object_size);

// Destroy pool (frees all slabs)
void slab_pool_destroy(slab_pool_t *pool);

// Allocate object
void *slab_alloc(slab_pool_t *pool);

// Free object
void slab_free(slab_pool_t *pool, void *ptr);

// Get stats
typedef struct {
    uint64_t total_slabs;
    uint64_t total_objects;
    uint64_t used_objects;
    uint64_t free_objects;
    size_t memory_used;
} slab_stats_t;

void slab_get_stats(slab_pool_t *pool, slab_stats_t *stats);

#endif
```

---

## Critical Implementation Details

### 1. Bitmap Operations (bitmap.c)

```c
#include <stdint.h>
#include <strings.h>  // For ffs() / ffsl()

// Find first zero bit in bitmap
// Returns bit index, or -1 if all bits set
static inline int bitmap_find_first_zero(const uint8_t *bitmap, size_t num_bits) {
    size_t num_bytes = (num_bits + 7) / 8;
    
    for (size_t i = 0; i < num_bytes; i++) {
        if (bitmap[i] != 0xFF) {
            // Found byte with zero bit
            // Use ffs() to find first zero bit
            uint8_t inv = ~bitmap[i];
            int bit = ffs(inv) - 1;  // ffs returns 1-based index
            return (i * 8) + bit;
        }
    }
    return -1;  // All bits set
}

// Set bit (mark allocated)
static inline void bitmap_set(uint8_t *bitmap, size_t bit_index) {
    bitmap[bit_index / 8] |= (1 << (bit_index % 8));
}

// Clear bit (mark free)
static inline void bitmap_clear(uint8_t *bitmap, size_t bit_index) {
    bitmap[bit_index / 8] &= ~(1 << (bit_index % 8));
}

// Test bit
static inline int bitmap_test(const uint8_t *bitmap, size_t bit_index) {
    return (bitmap[bit_index / 8] >> (bit_index % 8)) & 1;
}
```

**Note**: For production, use `__builtin_ffs()` (GCC/Clang) or `_BitScanForward()` (MSVC) for better performance.

### 2. Slab Allocation (slab.c)

```c
#include <sys/mman.h>
#include <string.h>
#include <assert.h>

#define SLAB_SIZE 4096
#define SLAB_MAGIC 0x534C4142

static slab_t *slab_create(uint32_t object_size) {
    // Allocate 4KB page-aligned slab
    void *mem = mmap(NULL, SLAB_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    
    if (mem == MAP_FAILED) {
        return NULL;
    }
    
    slab_t *slab = (slab_t *)mem;
    
    // Initialize header
    slab->header.magic = SLAB_MAGIC;
    slab->header.version = 1;
    slab->header.object_size = object_size;
    slab->header.object_count = (SLAB_SIZE - 192) / object_size;
    slab->header.free_count = slab->header.object_count;
    slab->header.next = NULL;
    
    // Clear bitmap (all free)
    memset(slab->bitmap, 0, 128);
    
    return slab;
}

void *slab_alloc(slab_pool_t *pool) {
    slab_allocator_t *alloc = (slab_allocator_t *)pool;
    
    // Get slab with free slots
    if (alloc->free_slabs == NULL) {
        // Allocate new slab
        slab_t *new_slab = slab_create(alloc->size_class);
        if (!new_slab) {
            return NULL;
        }
        alloc->free_slabs = new_slab;
        alloc->total_slabs++;
    }
    
    slab_t *slab = alloc->free_slabs;
    
    // Find free slot
    int bit_index = bitmap_find_first_zero(slab->bitmap, slab->header.object_count);
    assert(bit_index >= 0);  // Should always succeed if free_count > 0
    
    // Mark allocated
    bitmap_set(slab->bitmap, bit_index);
    slab->header.free_count--;
    
    // Calculate object address
    void *obj = slab->data + (bit_index * slab->header.object_size);
    
    // If slab is now full, move to full list
    if (slab->header.free_count == 0) {
        alloc->free_slabs = slab->header.next;
        slab->header.next = alloc->full_slabs;
        alloc->full_slabs = slab;
    }
    
    alloc->total_allocated++;
    return obj;
}
```

### 3. Slab Free (Important: Reverse Lookup)

```c
void slab_free(slab_pool_t *pool, void *ptr) {
    slab_allocator_t *alloc = (slab_allocator_t *)pool;
    
    // Find which slab owns this pointer
    // Round down to slab boundary (4KB aligned)
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t slab_addr = addr & ~(SLAB_SIZE - 1);
    slab_t *slab = (slab_t *)slab_addr;
    
    // Verify magic
    assert(slab->header.magic == SLAB_MAGIC);
    
    // Calculate slot index
    uintptr_t offset = addr - (uintptr_t)slab->data;
    uint32_t slot_index = offset / slab->header.object_size;
    
    assert(slot_index < slab->header.object_count);
    assert(bitmap_test(slab->bitmap, slot_index));  // Must be allocated
    
    // Clear bit
    bitmap_clear(slab->bitmap, slot_index);
    slab->header.free_count++;
    
    // If slab was full, move back to free list
    if (slab->header.free_count == 1) {
        // Remove from full list (linear search, acceptable for Phase 1)
        slab_t **pp = &alloc->full_slabs;
        while (*pp && *pp != slab) {
            pp = &(*pp)->header.next;
        }
        assert(*pp == slab);
        *pp = slab->header.next;
        
        // Add to free list
        slab->header.next = alloc->free_slabs;
        alloc->free_slabs = slab;
    }
    
    alloc->total_allocated--;
}
```

---

## Benchmark Implementation

### baseline_malloc.c

```c
#include <stdlib.h>
#include <stdio.h>
#include <sys/resource.h>

#define NUM_OBJECTS 1000000
#define OBJECT_SIZE 128

long get_rss_kb() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;  // KB on Linux, bytes on macOS
}

int main() {
    void **ptrs = calloc(NUM_OBJECTS, sizeof(void*));
    
    printf("Allocating %d objects of %d bytes each...\n", 
           NUM_OBJECTS, OBJECT_SIZE);
    
    long rss_before = get_rss_kb();
    
    for (int i = 0; i < NUM_OBJECTS; i++) {
        ptrs[i] = malloc(OBJECT_SIZE);
    }
    
    long rss_after = get_rss_kb();
    
    printf("RSS before: %ld KB\n", rss_before);
    printf("RSS after:  %ld KB\n", rss_after);
    printf("Delta:      %ld KB\n", rss_after - rss_before);
    printf("Expected:   %ld KB (pure data)\n", 
           (NUM_OBJECTS * OBJECT_SIZE) / 1024);
    printf("Overhead:   %.1f%%\n", 
           ((double)(rss_after - rss_before) / 
            ((NUM_OBJECTS * OBJECT_SIZE) / 1024) - 1.0) * 100);
    
    // Keep allocated to measure steady-state RSS
    getchar();
    
    for (int i = 0; i < NUM_OBJECTS; i++) {
        free(ptrs[i]);
    }
    free(ptrs);
    
    return 0;
}
```

---

## Testing Strategy

### Phase 1 Tests

```bash
# Unit tests
gcc -o test_bitmap test_bitmap.c bitmap.c -std=c11 -Wall
./test_bitmap

gcc -o test_slab test_slab.c slab.c bitmap.c -std=c11 -Wall
./test_slab

# Benchmarks
gcc -O2 -o baseline_malloc baseline_malloc.c
gcc -O2 -o slab_bench slab_alloc_bench.c slab.c bitmap.c

./baseline_malloc
./slab_bench

# Valgrind memory check
valgrind --leak-check=full ./test_slab
```

---

## Success Criteria (Phase 1)

- [ ] Slab allocator compiles without warnings
- [ ] All unit tests pass
- [ ] Valgrind shows no leaks
- [ ] Baseline malloc benchmark complete
- [ ] Slab benchmark complete
- [ ] RSS overhead < 10% (target <5%)
- [ ] Allocation latency measured (target <100ns p99)

---

## Known Limitations (Phase 1)

1. **No concurrency**: Single-threaded only
2. **No slab recycling**: munmap() not implemented yet
3. **Linear search on free**: Acceptable for now
4. **No size class auto-selection**: Caller specifies exact size

These are Phase 2+ improvements.

---

## Next Steps After Phase 1

1. Add concurrency (per-allocator locks)
2. Implement slab recycling (munmap empty slabs)
3. Add size class auto-selection
4. Begin Phase 2 (tiered memory)

---

*This document provides concrete implementation guidance. Refer to TECHNICAL_DESIGN.md for high-level architecture.*
