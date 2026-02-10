# Churn Test Analysis: Why 0 Slabs Recycled is CORRECT

## Test Results

```
RSS initial:  15.12 MiB
RSS final:    15.30 MiB
RSS growth:   1.1% (final vs initial)

New slabs allocated:    0
Empty slabs recycled:   0
Empty slabs overflowed: 0

RSS growth < 50%:           PASS (1.1%)
Empty slab recycling works: FAIL (0 slabs recycled)
```

## Why This Makes Sense

### The Test Never Calls `epoch_close()`

Looking at `churn_test.c:98-138`, the test:
1. Allocates 100K objects to epoch 0
2. Repeatedly frees 10K and reallocates 10K (1000 cycles)
3. **Never calls `epoch_close()`**

### Slab Lifecycle Without `epoch_close()`

**When a slab becomes fully empty (all slots freed):**

From `slab_alloc.c:1841-1863`:
```c
if (new_fc == s->object_count) {
    /* Slab is now fully empty. Two cases:
     * 1. Was on FULL list → move to PARTIAL (so it can be reused)
     * 2. Was on PARTIAL list → already correct, just increment empty counter
     */
    // NOTE: Does NOT recycle! Just moves to partial list or stays there.
}
```

The slab:
- Stays on the partial list (or moves there from full)
- Remains available for allocation
- **Is NOT recycled** (no `cache_push()`, no `madvise()`)

**When `epoch_close()` is called:**

From `slab_alloc.c:2258-2290`:
```c
/* Scan partial list for empty slabs */
while (cur) {
    if (atomic_load_explicit(&cur->free_count, memory_order_relaxed) == cur->object_count) {
        list_remove(&es->partial, cur);
        empty_slabs[idx++] = cur;
    }
}

/* Recycle all collected slabs */
for (size_t j = 0; j < idx; j++) {
    cache_push(sc, empty_slabs[j]);  // ← Increments empty_slab_recycled
}
```

Only `epoch_close()` triggers recycling counters.

## Why RSS is Stable Despite 0 Recycling

**Slab reuse vs slab recycling:**

| Event | RSS Impact | Recycling Counter | What Happens |
|-------|------------|-------------------|--------------|
| **Slab becomes empty (no epoch_close)** | None | No increment | Slab stays on partial list, available for reuse |
| **Slab becomes empty (with epoch_close)** | Drops (madvise) | Increments `empty_slab_recycled` | Slab removed from list, pushed to cache, potentially madvise'd |

**In this test:**
- Slabs become empty during churn
- They immediately get reallocated from (reused)
- They never accumulate enough to trigger new slab allocation
- RSS stays stable because slabs are reused, not recycled

**The test shows:**
- ✓ RSS doesn't grow unbounded (slabs are reused efficiently)
- ✓ No new slabs needed (existing slabs satisfy demand)
- ✓ Empty slab tracking works (empty_partial_count increments)
- ✗ Recycling counters are 0 (because `epoch_close()` was never called)

## Is This A Bug?

**No, this is correct behavior.**

The test's "FAIL" criteria is wrong. It expects:
```c
bool recycling_works = (total_recycled > 0);
```

But recycling **requires** `epoch_close()`. The correct criteria should be:

**Option A (Test slab reuse):**
```c
// Test that RSS stays bounded without epoch_close
bool slab_reuse_works = (growth < 50.0) && (counters.new_slab_count == 0);
```

**Option B (Test slab recycling):**
```c
// Call epoch_close() and verify recycling happens
epoch_close(&a, 0);
get_perf_counters(&a, 1, &counters);
bool recycling_works = (counters.empty_slab_recycled > 0);
```

## Two Different Tests Needed

### Test 1: Slab Reuse (Current Test)
**Purpose:** Validate that RSS stays bounded when slabs are continuously reused within the same epoch.

**Method:** Churn without `epoch_close()`, expect 0 new slabs allocated.

**Pass criteria:** RSS growth < 50%, new_slab_count == 0

**This validates:** Empty slabs on partial list are efficiently reused.

### Test 2: Slab Recycling (Missing Test)
**Purpose:** Validate that `epoch_close()` actually recycles empty slabs and drops RSS.

**Method:** 
1. Allocate many objects to epoch 0
2. Free all objects
3. Call `epoch_close(0)`
4. Measure RSS delta and recycling counters

**Pass criteria:** RSS drops, empty_slab_recycled > 0

**This validates:** `epoch_close()` removes empty slabs and returns memory to OS.

## Recommended Fix

### Option 1: Rename and Fix Criteria
Rename `churn_test` to `reuse_test`, change pass criteria:
```c
bool slab_reuse_works = (growth < 50.0) && (counters.new_slab_count == 0);
printf("Slab reuse works: %s\n", slab_reuse_works ? "PASS" : "FAIL");
```

### Option 2: Add epoch_close() and Test Recycling
Keep test name, but add `epoch_close()` call:
```c
for (int cycle = 0; cycle < CHURN_CYCLES; cycle++) {
    // ... churn logic ...
    
    // Periodically close and advance epoch to trigger recycling
    if (cycle % 100 == 0) {
        epoch_close(&a, 0);
        epoch_advance(&a);
    }
}
```

### Option 3: Split Into Two Tests
Create `test_slab_reuse.c` and `test_slab_recycling.c` with distinct purposes.

## Conclusion

**The behavior is correct.** The test results show:
1. Slabs are efficiently reused (0 new slabs allocated)
2. RSS stays bounded (1.1% growth over 1000 cycles)
3. Recycling counters are 0 because `epoch_close()` was never called

The test's "FAIL" verdict is misleading. The allocator is working as designed.
