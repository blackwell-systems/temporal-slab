# temporal-slab Source Code

## Implementation

Production-quality slab allocator optimized for sustained churn, predictable latency, and bounded RSS:
- **Lock-free allocation** - Atomic current_partial pointer, no mutex in fast path
- **Lock-free bitmap operations** - CAS loops for slot allocation/freeing
- **O(1) list operations** - Direct list membership tracking (no linear search)
- **FULL-only recycling** - Provably safe empty slab reuse (no race conditions)
- **Bounded RSS** - Cache + overflow lists prevent memory leaks
- **Opaque handles** - 64-bit encoding hides implementation details
- **Dual API** - Handle-based (explicit control) and malloc-style (drop-in replacement)

## When to Use temporal-slab

temporal-slab is designed for long-running systems that:
- Allocate and free many small, fixed-size objects
- Require stable RSS under sustained churn
- Are sensitive to tail latency
- Must behave safely under misuse (stale or double frees)

It is not intended to replace general-purpose allocators for variable-sized or short-lived workloads.

## Safety Guarantees

- Slabs are never unmapped during runtime
- Invalid or stale handles never crash the process
- Empty slab recycling is conservative and race-free
- All memory is released deterministically in `allocator_destroy()`

## Build

```bash
cd src/
make                    # Build all tests
./smoke_tests          # Single/multi-thread correctness
./churn_test           # RSS bounds validation
./test_malloc_wrapper  # malloc/free API tests
./benchmark_accurate   # Performance measurement
```

## Test Results

**Correctness:**
- Single-thread: PASS
- Multi-thread: PASS (8 threads × 500K iterations)
- Handle validation: PASS
- Malloc wrapper: PASS

**Performance (128-byte objects, Intel Core Ultra 7):**
- Allocation: **70ns** average
- Free: **12ns** average
- Cache hit rate: **97%+**

**Memory Efficiency:**
- RSS growth under churn: **2.2%** (15.30 → 15.64 MiB, 1000 cycles)
- Zero cache overflows (optimal utilization)

## Key Files

- `slab_alloc.c` - Core implementation
- `slab_alloc_internal.h` - Internal structures and state machine
- `smoke_tests.c` - Correctness validation
- `churn_test.c` - RSS bounds validation
- `test_malloc_wrapper.c` - Malloc/free API tests
- `benchmark_accurate.c` - Performance measurement
- `Makefile` - Build system
