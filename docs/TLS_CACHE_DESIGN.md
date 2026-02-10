# Thread-Local Handle Cache Design

**Goal:** Improve p50 allocation latency from ~41ns to ~10-15ns by eliminating shared-state access on the fast path.

**Status:** Design phase (revised)  
**Target:** 2-4× p50 improvement while preserving all correctness guarantees  
**Gated by:** `ENABLE_TLS_CACHE` compile flag (opt-in initially)

**Design approach:** TLS handle stack (tcache-style), not TLS slab ownership

---

## Problem Statement

Current p50 = 41ns is limited by shared-state operations:
- Atomic load of `current_partial` (~5-10ns)
- CAS on bitmap (~5-10ns)
- Cache line bouncing across cores (~10-20ns)
- Occasional CAS retries (variance)

Even with perfect lock-free design, inter-core coherency dominates latency.

---

## Solution: Per-Thread Handle Cache

### Core Idea

Cache **pre-allocated handles** in thread-local stacks (one per size class):

```c
__thread TLSCache tls_cache[8];  // One stack per size class
```

**Fast path** (no atomics, no locks):
```
if (tls_stack has cached handles):
    pop handle → compute ptr → return (~10ns)
else:
    batch-refill from global allocator
```

**Key principle:** Both alloc AND free are TLS-fast. Cross-thread free works naturally (freeing thread caches the handle locally).

---

## Data Structure

```c
#if ENABLE_TLS_CACHE
#define TLS_CACHE_SIZE 32  // Tunable: 32 handles per size class

typedef struct {
    SlabHandle handles[TLS_CACHE_SIZE];  // Pre-allocated handles
    uint32_t count;                       // Current stack depth
    uint32_t epoch_id[TLS_CACHE_SIZE];   // Epoch ID per handle
} TLSCache;

// One cache per size class per thread
extern __thread TLSCache _tls_cache[8];
#endif
```

**Size:** (8 bytes + 4 bytes) × 32 × 8 = 3 KB per thread (negligible)

**Why epoch_id array:** Enables epoch_close() to selectively flush matching handles.

---

## Allocation Fast Path

```c
#if ENABLE_TLS_CACHE
static inline void* tls_try_alloc(SlabAllocator* a, uint32_t sc, 
                                   uint32_t epoch_id, SlabHandle* out_h) {
    TLSCache* tls = &_tls_cache[sc];
    
    // Fast path: TLS hit (no atomics, no locks)
    if (tls->count > 0) {
        // Pop handle from stack
        uint32_t idx = --tls->count;
        SlabHandle h = tls->handles[idx];
        
        // Validate epoch (handles from closed epochs are stale)
        if (tls->epoch_id[idx] == epoch_id || !is_epoch_closing(a, tls->epoch_id[idx])) {
            *out_h = h;
            return handle_to_ptr(a, h, sc);
        }
        
        // Stale handle, discard and try next
        // (or: flush all stale handles here)
    }
    
    // Miss: refill needed
    return NULL;
}
#endif
```

**Latency:** ~10-15ns (array access + bounds check + branch prediction)

**Why this is safe:**
- No slab ownership conflict (handle is just metadata)
- Cross-thread frees work (other thread caches handle in its TLS)
- Generation validation still works (handle carries gen)

---

## Refill Path (Slow, Rare)

When TLS cache is empty, batch-allocate from global allocator:

```c
static void tls_refill(SlabAllocator* a, uint32_t sc, uint32_t epoch_id) {
    TLSCache* tls = &_tls_cache[sc];
    
    // Batch allocate from global allocator
    #define REFILL_BATCH 16  // Allocate 16 handles at once
    
    for (uint32_t i = 0; i < REFILL_BATCH && tls->count < TLS_CACHE_SIZE; i++) {
        SlabHandle h;
        void* ptr = alloc_obj_epoch(a, sc, epoch_id, &h);
        
        if (!ptr) break;  // Allocation failed (out of memory)
        
        // Cache the handle (we'll return one immediately)
        tls->handles[tls->count] = h;
        tls->epoch_id[tls->count] = epoch_id;
        tls->count++;
    }
    
    // Now tls_try_alloc() will succeed
}
```

**Batching rationale:** Amortize global allocator overhead. One batch refill services 16 allocations with zero atomics.

---

## Free Path (NOW TLS-Fast Too!)

**Key improvement:** Frees also benefit from TLS caching.

```c
bool free_obj(SlabAllocator* a, SlabHandle h) {
    uint32_t sc = handle_get_size_class(h);
    
    #if ENABLE_TLS_CACHE
    TLSCache* tls = &_tls_cache[sc];
    
    // Fast path: cache handle in TLS
    if (tls->count < TLS_CACHE_SIZE) {
        tls->handles[tls->count] = h;
        tls->epoch_id[tls->count] = handle_get_epoch(a, h);  // Read from slab
        tls->count++;
        return true;
    }
    
    // Cache full: flush half to global, then cache this handle
    tls_flush_batch(a, sc, TLS_CACHE_SIZE / 2);
    tls->handles[tls->count++] = h;
    return true;
    #else
    // Existing atomic free path
    return free_obj_global(a, h);
    #endif
}
```

**Why this is safe:**
1. Cross-thread frees work naturally (freeing thread caches handle in its own TLS)
2. Handle validation happens when handle is **used** (on next allocation), not when cached
3. Generation logic preserved (stale handles detected on pop)
4. Flushing to global is safe (just calls existing atomic free path)

---

## Epoch Interaction (The Key to Safety)

### Problem

```c
epoch_close(0);  // Want to reclaim epoch 0
// But TLS caches may hold epoch 0 handles!
// Those handles prevent accurate reclamation accounting
```

### Solution: Flush Matching Handles on Epoch Close

On `epoch_close()`, drain all cached handles belonging to that epoch:

```c
void epoch_close(SlabAllocator* a, uint32_t epoch_id) {
    // Step 1: Mark epoch as CLOSING (existing)
    EpochState* es = ...;
    atomic_store(&es->state, EPOCH_CLOSING);
    
    // Step 2: Flush epoch handles from ALL thread TLS caches
    #if ENABLE_TLS_CACHE
    tls_flush_epoch_all_threads(a, epoch_id);
    #endif
    
    // Step 3: Proceed with reclamation (existing)
    // Now safe - no TLS caches hold epoch_id handles
}
```

### Implementation Approaches

**Option A: Thread Registry + Eager Flush** (safest, chosen)

```c
// Global thread registry (lock-protected)
typedef struct {
    pthread_t tid;
    TLSCache* caches;  // Pointer to thread's TLS cache array
} ThreadEntry;

static ThreadEntry thread_registry[MAX_THREADS];
static pthread_mutex_t registry_lock;

// On thread first use:
void tls_register_thread(void) {
    LOCK(&registry_lock);
    thread_registry[next_slot] = {pthread_self(), _tls_cache};
    UNLOCK(&registry_lock);
}

// On epoch_close():
void tls_flush_epoch_all_threads(SlabAllocator* a, uint32_t epoch_id) {
    LOCK(&registry_lock);
    
    for (each thread in registry) {
        for (each size class) {
            TLSCache* tls = &thread->caches[sc];
            
            // Scan for matching epoch handles
            for (int i = 0; i < tls->count; i++) {
                if (tls->epoch_id[i] == epoch_id) {
                    // Flush to global allocator
                    free_obj_global(a, tls->handles[i]);
                    
                    // Swap with last element and decrement
                    tls->handles[i] = tls->handles[--tls->count];
                    tls->epoch_id[i] = tls->epoch_id[tls->count];
                    i--;  // Re-check this slot
                }
            }
        }
    }
    
    UNLOCK(&registry_lock);
}
```

**Why this is correct:**
- `epoch_close()` has full visibility into all TLS caches
- Flush is atomic (under registry_lock)
- No race conditions (thread registry prevents concurrent modification)

**Option B: Lazy Flush** (simpler, but requires app-level quiescence guarantee)

```c
// On allocation fast path:
if (is_epoch_closing(a, tls->epoch_id[i])) {
    // Discard stale handle
    tls->count--;
    continue;
}
```

**Contract:** Application guarantees no threads allocate from closing epochs. Handles are lazily discovered and discarded.

**Recommendation:** **Option A** (thread registry) for allocator-enforced safety.

---

## Why This Design Is Correct

### No Bitmap Conflicts

**Problem avoided:** The original "TLS slab" design had a fatal flaw - TLS thread allocating with no atomics while other threads free with atomics = concurrent bitmap mutation.

**Solution:** TLS cache holds **handles** (metadata), not slab ownership. When you pop a handle:
1. Slab was already allocated from (bitmap already set)
2. No concurrent bitmap mutation (TLS just reuses handle)
3. Generation validation still works (stale handles rejected)

### Cross-Thread Free Just Works

**Scenario:**
```
Thread A allocates handle h1 (from TLS or global)
Thread A passes object to Thread B
Thread B frees h1 → caches in Thread B's TLS
Later: Thread B allocates → pops h1 → reuses that slot
```

This is **correct** because:
- Handle h1 is valid metadata
- Thread B now owns the right to reuse that slot
- No slab-level ownership conflict
- Generation counter prevents ABA if slab was recycled

### Epoch Close Correctness

**Invariant:** After `epoch_close(epoch_id)` completes, no TLS cache contains handles from `epoch_id`.

**Enforcement (Option A):**
```
epoch_close() → walk thread registry → scan all TLS caches → flush matching handles
```

**Guarantee:** All handles from that epoch are returned to global allocator, enabling accurate reclamation.

---

## Compile Flag Integration

```c
// In slab_alloc_internal.h
#ifndef ENABLE_TLS_CACHE
#define ENABLE_TLS_CACHE 0  // Default: disabled initially
#endif

#if ENABLE_TLS_CACHE
extern __thread TLSCache _tls_cache[8];

void tls_cache_init(void);
void tls_flush_slab(SlabAllocator* a, uint32_t sc);
void tls_flush_epoch(SlabAllocator* a, uint32_t epoch_id);
#endif
```

**Build usage:**
```bash
# Baseline (no TLS)
make

# With TLS cache
make CFLAGS="-DENABLE_TLS_CACHE=1"
```

---

## Performance Expectations

| Metric | Current | With TLS | Improvement |
|--------|---------|----------|-------------|
| **p50 alloc** | 41 ns | **10-15 ns** | **2.7-4.1×** |
| **p90 alloc** | ~200 ns | **50-100 ns** | **2-4×** |
| **p99 alloc** | 429 ns | **200-300 ns** | **1.4-2.1×** |
| **p999 alloc** | 771 ns | **400-600 ns** | **1.3-1.9×** |

TLS improves **all percentiles**, but most dramatically at **p50/p90** (pure fast path).

p99/p999 still occasionally hit slow path (slab exhaustion), so improvement is smaller.

---

## Implementation Phases

### Phase 1: Minimal TLS (MVP)
- [ ] Add `ENABLE_TLS_CACHE` flag
- [ ] Define `TLSCache` structure
- [ ] Implement `tls_try_alloc()` fast path
- [ ] Implement `tls_refill()` from `current_partial`
- [ ] Add lazy epoch flush (check in fast path)
- [ ] Benchmark p50/p99

**Success criteria:** p50 < 20ns, all existing tests pass

### Phase 2: Optimization
- [ ] Pre-scan bitmap on refill to populate `tls->remaining`
- [ ] Add `tls_owned` flag to reduce contention
- [ ] Tune TLS cache size (currently 1 slab per class)
- [ ] Add TLS hit/miss counters to observability

**Success criteria:** p50 < 15ns, <5% TLS miss rate

### Phase 3: Production Hardening
- [ ] Add `tls_flush_epoch()` for eager flush
- [ ] Test with epoch rotation during TLS usage
- [ ] Validate RSS impact (expect +10-20% from per-thread slabs)
- [ ] Document TLS behavior in MULTI_THREADING.md

**Success criteria:** Zero correctness regressions, documented tradeoffs

---

## Risks and Mitigations

### Risk 1: Increased memory per thread

**Magnitude:** 3 KB per thread (32 handles × 8 classes × 12 bytes)

**Mitigation:** 
- Negligible compared to typical thread stack (8 MB)
- Start with ENABLE_TLS_CACHE=0 by default
- Document as opt-in performance feature

### Risk 2: Epoch close latency (thread registry scan)

**Magnitude:** O(threads × size_classes × TLS_CACHE_SIZE) scan

**Mitigation:**
- Only scans on epoch_close (rare operation)
- Lock held briefly (just array scan + swaps)
- Typical: 8 threads × 8 classes × 32 handles = 2048 checks (~10µs)

### Risk 3: Thread registration overhead

**Magnitude:** One-time pthread_once per thread + registry insert

**Mitigation:**
- Happens on first allocation (amortized over lifetime)
- Registry lock held briefly
- Typical overhead: <1µs per thread initialization

### Risk 4: Handle reuse across epochs

**Not a risk:** Epoch ID stored per handle. Stale handles detected on pop or via epoch_close flush.

### Risk 5: TLS cache pollution

**Scenario:** Thread allocates/frees from many epochs, cache fills with mixed epoch handles

**Mitigation:**
- TLS_CACHE_SIZE = 32 (enough diversity)
- Flush on overflow (FIFO eviction of old handles)
- Worst case: cache miss rate increases slightly (still correct)

---

## Testing Strategy

### Unit Tests
1. TLS allocation + cross-thread free (validate handle correctness)
2. Epoch rotation during TLS usage (validate flush)
3. Multi-thread with TLS (validate no conflicts)

### Benchmarks
1. Single-thread microbenchmark (pure TLS fast path)
2. Multi-thread scaling (1→16 threads)
3. Epoch rotation frequency impact

### Regression Tests
1. All existing tests must pass with ENABLE_TLS_CACHE=1
2. smoke_tests multi-thread (validates bulk alloc/free)
3. contention tests (validates scaling doesn't regress)

---

## Success Metrics

**Latency (primary goal):**
- p50: 41ns → **<15ns** (2.7× improvement)
- p90: ~200ns → **<100ns** (2× improvement)
- p99: 429ns → **<300ns** (1.4× improvement)

**Correctness (mandatory):**
- Zero new test failures
- Zero handle invalidation bugs
- Zero epoch lifecycle violations

**Trade-offs (acceptable):**
- RSS: +10-20% from per-thread slabs
- Code complexity: +200 lines (isolated, gated)

---

## References

- **jemalloc tcache:** Per-thread cache with mag azine-style refill
- **mimalloc:** Per-thread heap with free list
- **tcmalloc:** Per-thread cache with central freelist

**Our approach differs:**
- Simpler: One slab per class (not freelist)
- Epoch-aware: Auto-flush on rotation
- Alloc-only: Frees always atomic

This makes TLS cache **mechanically simple** and **obviously correct**.

---

**Document version:** 2.0 (revised)  
**Status:** Design complete, ready for implementation  
**Next:** Implement Phase 1 (MVP)

---

## Design Revision History

**v2.0 (Feb 10, 2026):** Switched from "TLS slab ownership" to "TLS handle cache" design
- **Critical fix:** Avoided bitmap concurrency conflict (TLS alloc + remote free)
- **Critical fix:** Removed next_idx++ assumption (doesn't work with bitmap allocator)
- **Improvement:** Both alloc AND free become TLS-fast
- **Simplification:** No slab ownership protocol needed

**v1.0 (Feb 10, 2026):** Initial "TLS slab" design (INCORRECT - had two concurrency holes)
