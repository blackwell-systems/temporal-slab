# ZNS-Slab: Lock-Free Slab Allocator

A production-ready slab allocator for sub-512 byte objects with lock-free fast path and bounded memory usage.

## Features

- **Lock-free allocation fast path** - Single atomic load for cache hits
- **Sub-100ns median latency** - 70ns alloc, 12ns free (Intel Core Ultra 7)
- **Bounded RSS** - Overflow list prevents memory leaks under pressure
- **Safe handle validation** - Slabs stay mapped, stale handles detected
- **97% cache hit rate** - Per-size-class slab cache (32 pages each)
- **Provably safe recycling** - FULL-only recycling eliminates race conditions
- **Two APIs** - Handle-based (low-level) and malloc-style (drop-in)

## Quick Start

```c
#include <slab_alloc.h>

// Create allocator
SlabAllocator* alloc = slab_allocator_create();

// Option 1: malloc-style API (recommended)
void* ptr = slab_malloc(alloc, 128);
slab_free(alloc, ptr);

// Option 2: Handle-based API (explicit control)
SlabHandle h;
void* obj = alloc_obj(alloc, 128, &h);
free_obj(alloc, h);

// Cleanup
slab_allocator_free(alloc);
```

## Build and Test

```bash
cd src/
make                    # Build all tests
./smoke_tests          # Basic correctness (single/multi-thread)
./churn_test           # RSS bounds validation
./test_malloc_wrapper  # malloc/free API tests
```

## Size Classes

Fixed size classes optimized for common object sizes:
- 64 bytes
- 128 bytes  
- 256 bytes
- 512 bytes

**Maximum allocation:**
- Handle API: **512 bytes** (no overhead)
- Malloc wrapper: **504 bytes** (512 - 8 byte header for handle storage)

## Architecture

### Memory Layout
- **Slab = 4KB page** with header, bitmap, and object slots
- **Bitmap allocation** - Lock-free slot claiming with atomic CAS
- **Intrusive lists** - PARTIAL and FULL lists per size class
- **Slab cache** - 32 pages per size class (128KB total)
- **Overflow list** - Bounded tracking when cache fills

### Lock-Free Fast Path
1. Load `current_partial` pointer (atomic acquire)
2. Allocate slot with CAS on bitmap
3. Return pointer (no locks in common case)

### Recycling Strategy
- **FULL list** - Slabs with zero free slots (never published)
- **Recycling** - Only from FULL list (provably safe, no races)
- **PARTIAL slabs** - Stay on list when empty, reused naturally
- **Result** - Bounded RSS without recycling race conditions

## Safety Contract

### Never munmap During Runtime
Slabs remain mapped for the allocator's lifetime, enabling:
- Safe validation of stale handles (no segfaults)
- Lock-free handle checking in free path
- Guaranteed bounded RSS (cache + overflow + active slabs)

### Handle Validation
- Handles encode slab pointer + slot + size class (64-bit opaque)
- `free_obj` validates magic number before freeing
- Double-free and stale handle detection without crashes

### Memory Bounds
RSS bounded by: `(partial + full + cache + overflow) = working set`

Typical growth under sustained churn: **<3%** over 1000 cycles

## Performance

**Platform**: Intel Core Ultra 7 165H, Linux, GCC 13.3.0 -O3

**Latency** (128-byte objects):
- Allocation: 70ns avg
- Free: 12ns avg

**Memory Efficiency**:
- Cache hit rate: 97%+
- RSS growth under churn: 2.2% (15.30 → 15.64 MiB, 1000 cycles)
- Zero cache overflows (optimal cache utilization)

**Concurrency**: Validated with 8 threads × 500K iterations

## API Reference

### Lifecycle
```c
SlabAllocator* slab_allocator_create(void);
void slab_allocator_free(SlabAllocator* alloc);

void allocator_init(SlabAllocator* alloc);    // For custom storage
void allocator_destroy(SlabAllocator* alloc);
```

### Malloc-Style API
```c
void* slab_malloc(SlabAllocator* alloc, size_t size);
void slab_free(SlabAllocator* alloc, void* ptr);
```
- 8-byte header overhead per allocation
- Max size: 504 bytes
- NULL-safe: `slab_free(a, NULL)` is no-op

### Handle-Based API
```c
void* alloc_obj(SlabAllocator* alloc, uint32_t size, SlabHandle* out_handle);
bool free_obj(SlabAllocator* alloc, SlabHandle handle);
```
- Zero overhead (no hidden headers)
- Explicit handle management
- Returns false on invalid/stale handles

### Performance Counters
```c
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;
  uint64_t current_partial_full;
  uint64_t empty_slab_recycled;
  uint64_t empty_slab_overflowed;
} PerfCounters;

void get_perf_counters(SlabAllocator* alloc, uint32_t size_class, PerfCounters* out);
```

## Thread Safety

- **Fast path** - Lock-free (atomic operations only)
- **Slow path** - Per-size-class mutex
- **Cache operations** - Separate cache_lock per size class
- **No global locks** - All contention is size-class local

## Limitations

- **Max object size**: 512 bytes (handle API) or 504 bytes (malloc wrapper)
- **Fixed size classes** - Not suitable for arbitrary sizes
- **No realloc** - Size changes require alloc + copy + free
- **Linux only** - RSS measurement uses /proc/self/statm
- **No NUMA awareness** - Single allocator for all threads

## Project Structure

```
include/
  slab_alloc.h           - Public API
src/
  slab_alloc.c           - Implementation
  slab_alloc_internal.h  - Internal structures
  smoke_tests.c          - Correctness tests
  churn_test.c           - RSS bounds validation
  test_malloc_wrapper.c  - malloc/free API tests
  benchmark_accurate.c   - Performance measurement
  Makefile               - Build system
```

## Testing

- **smoke_tests** - Single-thread, multi-thread (8×500K), micro-benchmark
- **churn_test** - 1000 cycles of alloc/free churn, RSS tracking
- **test_malloc_wrapper** - malloc/free API correctness

All tests validate:
- Correctness under concurrency
- Bounded memory growth
- API safety (NULL, stale handles, double-free)

## Design Principles

1. **Correctness over optimization** - FULL-only recycling eliminates races
2. **Conservative safety** - Never munmap during runtime
3. **Explicit state** - Separate list_id + cache_state for clarity
4. **Opaque handles** - 64-bit encoding hides implementation details
5. **Bounded resources** - Cache + overflow guarantee RSS limits
