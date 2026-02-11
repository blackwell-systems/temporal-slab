# Patch: Continuous Empty Slab Recycling

## Problem Statement

Current temporal-slab architecture couples **recycling** (making slabs reusable) with **reclamation** (RSS drop via madvise). Without periodic `epoch_close()` calls, empty slabs remain stranded on FULL lists, forcing all allocations to hit slowpath mutex.

**Evidence from benchmark:**
- Policy: `never` (no epoch advancement/closing)
- Recycled slabs: 0
- Slowpath hits: 100% (9/9 new slabs)
- Mean latency: 1959 µs (30× slower than 65 µs baseline)

## Root Cause

`epoch_close()` is the ONLY mechanism that:
1. Scans FULL lists for empty slabs
2. Pushes them to cache via `cache_push()`
3. Makes them globally reusable

Without it, allocator devolves into "global mutex per slab" + lock convoy.

## Solution: Decouple Recycling from Reclamation

Implement **Pattern 1: Return-to-global on empty**

- **Recycling**: Continuous, lock-free signaling + periodic harvest
- **Reclamation**: Explicit via `epoch_close()` (madvise + deterministic RSS)

This preserves determinism philosophy while keeping allocator healthy.

---

## Implementation: Empty Queue + Harvest

### Step A: Add empty queue infrastructure

**File: `/home/blackwd/code/ZNS-Slab/src/slab_alloc_internal.h`**

1. Add to `struct Slab` (line ~368):
```c
  uint32_t slab_id;
  
  /* Lock-free empty queue for continuous recycling */
  _Atomic uint32_t empty_queued;     /* 0=not queued, 1=queued for harvest */
  _Atomic(Slab*) empty_next;         /* Stack link for empty queue */
};
```

2. Add to `typedef struct EpochState` (line ~453):
```c
  _Atomic uint32_t empty_partial_count;
  
  /* Lock-free empty slab queue (MPSC stack for continuous recycling) */
  _Atomic(Slab*) empty_queue_head;   /* Top of stack */
} EpochState;
```

### Step B: Signal empty transition (free path)

**File: `/home/blackwd/code/ZNS-Slab/src/slab_alloc.c`**

**Location: In `free_obj()` after line 1917** (where empty transition is detected)

Replace the comment block at lines 1899-1910 with:

```c
  /* Check if slab just became fully empty (all slots free).
   * 
   * NEW RECYCLING MODEL (Phase 2.2):
   * Signal empty transition to lock-free queue for continuous recycling.
   * This decouples recycling (making slabs reusable) from reclamation
   * (madvise + RSS drops via epoch_close).
   * 
   * Key insight: Empty slabs must be made globally available WITHOUT
   * waiting for epoch_close(), otherwise "never close" policy causes
   * catastrophic slowpath convoy (100% mutex hits, 1959 µs latency).
   */
  if (new_fc == s->object_count) {
    /* Slab just became empty! Push to empty queue for harvest.
     * Use atomic exchange to ensure single enqueue (idempotent). */
    uint32_t was_queued = atomic_exchange_explicit(&s->empty_queued, 1, memory_order_acq_rel);
    if (was_queued == 0) {
      /* First thread to mark empty: push onto epoch's empty queue (lock-free MPSC stack) */
      Slab* old_head = atomic_load_explicit(&es->empty_queue_head, memory_order_relaxed);
      do {
        atomic_store_explicit(&s->empty_next, old_head, memory_order_relaxed);
      } while (!atomic_compare_exchange_weak_explicit(&es->empty_queue_head, &old_head, s,
                                                        memory_order_release, memory_order_relaxed));
    }
    
    /* FULL→PARTIAL transition (if needed) remains for allocation correctness */
    if (s->list_id == SLAB_LIST_FULL) {
      LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");
      /* Double-check under lock (free_count may have changed) */
      uint32_t fc_now = atomic_load_explicit(&s->free_count, memory_order_acquire);
      if (fc_now == s->object_count && s->list_id == SLAB_LIST_FULL) {
        list_remove(&es->full, s);
        list_add_head(&es->partial, s);
        s->list_id = SLAB_LIST_PARTIAL;
        atomic_fetch_add_explicit(&sc->list_move_full_to_partial, 1, memory_order_relaxed);
      }
      UNLOCK_WITH_RANK(&sc->lock);
    }
    
    /* Increment empty counter for observability */
    atomic_fetch_add_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
    
#if ENABLE_DIAGNOSTIC_COUNTERS
    atomic_fetch_add_explicit(&sc->empty_slabs, 1, memory_order_relaxed);
#endif
  }
```

### Step C: Harvest empty queue (allocation slow path)

**File: `/home/blackwd/code/ZNS-Slab/src/slab_alloc.c`**

**Location: In `alloc_slow_path()` BEFORE calling `new_slab()`** (around line 1700-1720)

Add before the "Try to find existing slab on partial list" section:

```c
static void* alloc_slow_path(...) {
  ...
  LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");
  
  /* NEW: Harvest empty queue before allocating new slabs (Phase 2.2 continuous recycling).
   * 
   * Drain the lock-free empty queue and recycle confirmed-empty slabs.
   * This happens on slowpath (already holding sc->lock), so no additional
   * lock overhead. Empty slabs become globally reusable without waiting
   * for epoch_close().
   * 
   * Why this works:
   * - Empty signal is one-way (free_count can only increase on reuse)
   * - Harvest under sc->lock ensures safe unlink from lists
   * - current_partial clear prevents lock-free access to recycled slab
   * - cache_push() makes slab available to ALL threads (cross-thread recycling)
   */
  Slab* empty_head = atomic_exchange_explicit(&es->empty_queue_head, NULL, memory_order_acquire);
  if (empty_head) {
    Slab* e = empty_head;
    while (e) {
      Slab* next = atomic_load_explicit(&e->empty_next, memory_order_relaxed);
      
      /* Verify slab is still empty (free_count could have changed if reused) */
      uint32_t fc = atomic_load_explicit(&e->free_count, memory_order_acquire);
      if (fc == e->object_count) {
        /* Still empty! Safe to recycle. Unlink from wherever it is. */
        if (e->list_id == SLAB_LIST_PARTIAL) {
          list_remove(&es->partial, e);
        } else if (e->list_id == SLAB_LIST_FULL) {
          list_remove(&es->full, e);
        }
        e->list_id = SLAB_LIST_NONE;
        
        /* Clear current_partial if it points to this slab (prevents lock-free access) */
        Slab* curp = atomic_load_explicit(&es->current_partial, memory_order_relaxed);
        if (curp == e) {
          atomic_store_explicit(&es->current_partial, NULL, memory_order_release);
        }
        
        sc->total_slabs--;
        
        /* Decrement empty counter */
        atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
        
        /* Reset queued flag for next empty transition */
        atomic_store_explicit(&e->empty_queued, 0, memory_order_relaxed);
        atomic_store_explicit(&e->empty_next, NULL, memory_order_relaxed);
        
        /* Push to global cache for cross-thread reuse (THE RECYCLING RENDEZVOUS) */
        UNLOCK_WITH_RANK(&sc->lock);
        cache_push(sc, e);
        LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");
      } else {
        /* Slab became non-empty again (race with allocation). Clear queued flag. */
        atomic_store_explicit(&e->empty_queued, 0, memory_order_relaxed);
        atomic_store_explicit(&e->empty_next, NULL, memory_order_relaxed);
      }
      
      e = next;
    }
  }
  
  /* NOW proceed with normal slowpath logic (find existing partial or allocate new) */
  ...
```

### Step D: Initialize new fields

**File: `/home/blackwd/code/ZNS-Slab/src/slab_alloc.c`**

1. In `new_slab()` initialization (around line 1155):
```c
    atomic_store_explicit(&s->magic, SLAB_MAGIC, memory_order_relaxed);
    s->object_size = obj_size;
    s->object_count = expected_count;
    atomic_store_explicit(&s->free_count, expected_count, memory_order_relaxed);
    s->list_id = SLAB_LIST_NONE;
    s->cache_state = SLAB_ACTIVE;
    s->epoch_id = epoch_id;
    s->era = atomic_load_explicit(&a->epoch_era_counter, memory_order_relaxed);
    s->was_published = false;
    s->slab_id = cached_id;  /* Restored from cache metadata */
    
    /* Initialize empty queue fields */
    atomic_store_explicit(&s->empty_queued, 0, memory_order_relaxed);
    atomic_store_explicit(&s->empty_next, NULL, memory_order_relaxed);
```

2. In `allocator_init()` or epoch state initialization:
```c
    for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
      EpochState* es = &sc->epochs[e];
      list_init(&es->partial);
      list_init(&es->full);
      atomic_store_explicit(&es->current_partial, NULL, memory_order_relaxed);
      atomic_store_explicit(&es->empty_partial_count, 0, memory_order_relaxed);
      
      /* Initialize empty queue */
      atomic_store_explicit(&es->empty_queue_head, NULL, memory_order_relaxed);
    }
```

---

## Expected Results After Patch

### Test: `TSLAB_EPOCH_POLICY=never` (no epoch advancement/closing)

**Before patch:**
```
recycled=0  slowpath_hits=9  mean=1959 µs  p99=524 µs  max=1906 ms
```

**After patch (predicted):**
```
recycled >0  slowpath_hits <9  mean ~65 µs  p99 <300 µs  max <10 ms
```

### Validation Metrics

1. **`empty_slab_recycled` counter increases** (harvest working)
2. **`slowpath_hits` drops** (cache stays warm)
3. **Mean latency returns to ~65 µs** (no convoy)
4. **Tail latencies collapse** (no multi-ms spikes)

### Key Insight Proven

> **epoch_close was doing two jobs:**
> 1. Reclamation (RSS/madvise) ← still explicit
> 2. Recycling (slab reuse) ← now continuous
>
> The fix separates concerns: allocator stays healthy without closes,
> deterministic RSS drops remain available on demand.

---

## Documentation Updates Needed

1. **Update `/home/blackwd/code/ZNS-Slab/docs/foundations.md`**:
   - Add section on "Continuous Recycling vs Explicit Reclamation"
   - Explain empty queue mechanism and why it's necessary

2. **Update API docs for `epoch_close()`**:
   - Clarify it's for **reclamation** (RSS drops), not **recycling** (slab reuse)
   - Document that allocator remains healthy without closes (thanks to continuous recycling)

3. **Add benchmark result to `/home/blackwd/code/ZNS-Slab/docs/results.md`**:
   - Before/after comparison of "never" policy
   - Prove lifecycle independence

---

## Testing Plan

1. **Unit test**: Create test that allocates → frees → verifies empty queue population
2. **Integration test**: Run "never" policy for 10K requests, verify recycling counters
3. **Stress test**: Run "never" policy with 16 threads, verify no convoy
4. **Regression test**: Verify existing epoch_close behavior unchanged

---

## Notes

- **Atomic overhead**: One atomic exchange on rare empty transition (~once per 31-63 frees depending on size class)
- **Harvest cost**: Amortized across slowpath operations (already holding lock)
- **Thread safety**: MPSC stack is lock-free, harvest under existing sc->lock
- **Compatibility**: Existing `epoch_close()` harvest remains as-is (RSS reclamation path)

This patch transforms temporal-slab from "lifecycle allocator" into "lifecycle-sensitive allocator with continuous health maintenance."
