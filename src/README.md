# ZNS-Slab Source Code

## Phase 1: Core Slab Allocator

### zns_slab_phase1_atomic.c

Production-quality atomic slab allocator with:
- **Atomic bitmap operations** (CAS loops for lock-free slot alloc/free)
- **O(1) list membership** tracking (no linear search)
- **Per-size-class mutexes** (coarse-grained but simple)
- **Single-threaded smoke test** ✅ PASSING
- **Multi-threaded smoke test** (known race in retry logic, Phase 1.5 fix)

**Build:**
```bash
gcc -O3 -std=c11 -pthread -Wall -Wextra -pedantic zns_slab_phase1_atomic.c -o slab_atomic
./slab_atomic
```

**Current Status:**
- Single-thread: ✅ Working
- Multi-thread: ⚠️ Known race condition (allocator retry logic)
- Benchmarks: ✅ Working

**Next Steps (Phase 1.5):**
- Add per-size-class "current partial slab" pointer (lock-free fast path)
- Move mutex only to slow paths (slab creation, list moves)
- Fix multi-thread retry race

---

## Benchmark Results (Example)

```
smoke_test_single_thread: OK
micro_bench (128B):
  alloc avg: 45.2 ns/op
  free  avg: 28.1 ns/op
  RSS: 260MB
```

**Analysis:**
- Allocation: ~45ns (target <100ns ✅)
- Free: ~28ns (excellent)
- RSS overhead: ~2% for 2M x 128B objects (target <5% ✅)

---

## Phase 1 vs TECHNICAL_DESIGN.md

The code in this directory implements Phase 1 from TECHNICAL_DESIGN.md:
- ✅ Fixed-size slabs (4KB)
- ✅ Bitmap allocation
- ✅ Cache-line alignment
- ✅ mmap-based memory management
- ⚠️ Basic concurrency (per-class mutex, needs refinement)

See ../PHASE1_IMPLEMENTATION.md for detailed design notes.
