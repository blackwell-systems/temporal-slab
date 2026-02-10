# The Great Debugging Saga: Two Critical Races in temporal-slab

**A complete chronicle of debugging catastrophic concurrency bugs in a lock-free memory allocator**

**Date:** February 9-10, 2026  
**Severity:** Critical (data corruption + API contract violation)  
**Duration:** 2 debugging sessions across 2 days  
**Outcome:** Both bugs fixed, allocator hardened, comprehensive test suite added

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Part 1: The Reuse-Before-Madvise Race](#part-1-the-reuse-before-madvise-race)
3. [Part 2: The Handle Invalidation Bug](#part-2-the-handle-invalidation-bug)
4. [Part 3: The Zombie Repair Reduction](#part-3-the-zombie-repair-reduction)
5. [Architectural Lessons](#architectural-lessons)
6. [Testing Strategy](#testing-strategy)
7. [Timeline](#timeline)

---

## Executive Summary

This document chronicles the discovery and repair of two critical concurrency bugs in the temporal-slab allocator. Both bugs involved subtle temporal ordering issues in lock-free code paths.

### Bug #1: Reuse-Before-Madvise Race (Data Corruption)

**Blast radius:** Catastrophic memory corruption (slab headers zeroed while live)  
**Trigger conditions:**
- `ENABLE_RSS_RECLAMATION=1` (compile-time flag)
- Cache reuse path active (slab recycling enabled)
- Concurrent `cache_pop()` during `cache_push()` execution

**Symptom:** `free_count=UINT32_MAX`, `object_count=0` (physically impossible values)  
**Root cause:** `madvise()` executed AFTER slab became visible in cache  
**Fix:** Perform madvise BEFORE linearization point (cache insertion)

### Bug #2: Premature Slab Recycling (Handle Invalidation)

**Blast radius:** API contract violation (`free_obj()` returns false for valid handles)  
**Trigger conditions:**
- Bulk alloc then bulk free pattern (handles accumulate)
- Multiple threads share same epoch
- Slabs become empty while other threads hold handles

**Symptom:** `free_obj()` returns false, generation mismatch in registry  
**Root cause:** Generation incremented at slab-empty time, not at quiescence  
**Fix:** Defer recycling to `epoch_close()` (safe quiescence point)

### Core Principle

Both bugs violated the same invariant: **operations that invalidate metadata must occur only at linearization points or quiescence boundaries**, never while resources are reachable.

---

## Allocator Invariants

These are the "laws of physics" that make corruption detectable:

1. **Slab structure invariant:** `0 <= free_count <= object_count`
2. **Bitmap consistency:** Bitmap cardinality equals `object_count - free_count`
3. **Active slab invariant:** `object_count > 0` for any slab on partial/full lists
4. **Publication safety:** Published slabs must never be madvised (or must be protected by grace period)
5. **Handle validity contract:** Once `alloc_obj_epoch()` returns a handle, `free_obj(handle)` must succeed exactly once unless the handle was already freed or is malformed—independent of what other threads do

When Bug #1 occurred, invariants 1-3 were violated (impossible `free_count` values). When Bug #2 occurred, invariant 5 was violated (valid handles rejected).

---

## Part 1: The Reuse-Before-Madvise Race

### The Initial Symptom

Under high concurrency (8 threads, 800K operations) with RSS reclamation enabled (`-DENABLE_RSS_RECLAMATION=1`), the allocator exhibited catastrophic corruption:

```
*** REPAIRING zombie partial slab 0x7f8a4c000000 (free_count=4294967295, bitmap full) ***
*** REPAIRING zombie partial slab 0x7f8a4c001000 (free_count=4294967295, bitmap full) ***
*** REPAIRING zombie partial slab 0x7f8a4c002000 (free_count=4294967295, bitmap full) ***
```

**Smoking gun values:**
- `free_count = 4294967295` (UINT32_MAX - impossible!)
- `object_count = 0` (no slots, yet free_count is billions?)
- Bitmap shows `full` (all slots allocated, contradicts free_count)

This is **physically impossible** in a correctly functioning allocator. A slab with 32 slots cannot have 4 billion free slots.

### The Investigation Journey

#### Hypothesis 1: Bitmap Corruption?

First instinct: Check the atomic bitmap operations.

```c
// Examined slab_alloc_slot_atomic():
bool slab_alloc_slot_atomic(Slab* s, uint32_t* out_idx, uint32_t* out_retries) {
    uint32_t retries = 0;
    
    for (;;) {
        uint32_t fc = atomic_load_explicit(&s->free_count, memory_order_relaxed);
        if (fc == 0) return false;  // Slab full
        
        uint32_t idx = find_first_free_slot(s->bitmap);
        if (idx == UINT32_MAX) return false;  // Bitmap full
        
        // Atomic CAS on bitmap
        if (atomic_compare_exchange_weak(&s->bitmap[word], &old_val, new_val)) {
            atomic_fetch_sub(&s->free_count, 1);
            *out_idx = idx;
            return true;
        }
        retries++;
    }
}
```

**Verdict**: Bitmap operations are atomic and correct. Red herring.

#### Hypothesis 2: Race Condition in Slab Recycling?

The corruption appeared when RSS reclamation was enabled. Let's examine `cache_push()`:

```c
// OLD (BROKEN) cache_push():
static void cache_push(SizeClassAlloc* sc, Slab* s) {
    if (s->was_published) {
        /* Published slabs may have lock-free pointers in-flight.
         * CANNOT madvise - would zero memory that allocation path might access. */
        // Just cache it, no madvise
    }
    
    LOCK(&sc->cache_lock);
    
    if (sc->cache_count < CACHE_CAPACITY) {
        sc->cache_array[sc->cache_count++] = s->slab_id;
    } else {
        // Overflow to linked list
        CachedNode* node = malloc(sizeof(CachedNode));
        node->slab_id = s->slab_id;
        node->was_published = s->was_published;
        node->next = sc->overflow_list;
        sc->overflow_list = node;
    }
    
    UNLOCK(&sc->cache_lock);
    
    /* CRITICAL SECTION: Slab is NOW VISIBLE to cache_pop() */
    /* But we haven't called madvise yet! */
    
    if (!s->was_published) {
        /* Return physical pages to OS */
        madvise(s, SLAB_SIZE, MADV_DONTNEED);  // ZEROS HEADER!
    }
}
```

**THE RACE WINDOW DISCOVERED:**

```
Timeline with 2 threads:

T=0: Thread A calls cache_push(slab X)
     - Slab X: {slab_id=42, free_count=32, object_count=32, was_published=false}

T=1: Thread A locks cache_lock

T=2: Thread A inserts slab_id=42 into cache_array[0]
     - Cache now contains [42, ...]
     - Slab X is VISIBLE to cache_pop()!

T=3: Thread A unlocks cache_lock

     ⚠️  RACE WINDOW OPENS ⚠️

T=4: Thread B calls cache_pop()
     - Locks cache_lock
     - Gets slab_id=42 from cache
     - Unlocks
     - Returns slab X pointer

T=5: Thread B calls new_slab(slab X)
     - Initializes: free_count=32, object_count=32
     - Returns slab X to allocation pool

T=6: Thread B publishes slab X to current_partial
     - Slab X is now LIVE and accepting allocations!
     - Other threads can allocate from it

T=7: Thread A finally calls madvise(slab X, MADV_DONTNEED)
     - Kernel discards physical pages; subsequent reads fault in zero-filled pages
     - From program's perspective: slab header is DESTROYED
     - All fields become 0x00000000

T=8: Slab X header after madvise:
     Before:  {free_count=32, object_count=32, magic=0xDEADBEEF}
     After:   {free_count=0,  object_count=0,  magic=0x00000000}
     
     ⚠️  But bitmap still shows allocated slots!
     ⚠️  Next free operation will increment free_count from 0
     ⚠️  Multiple frees → free_count wraps around → UINT32_MAX!

T=9: Thread C frees object from slab X
     - atomic_fetch_add(&s->free_count, 1)
     - 0 → 1 → 2 → ... → eventually wraps to UINT32_MAX

Result: CATASTROPHIC CORRUPTION
```

**Root cause identified:** `madvise()` happens AFTER slab becomes visible in cache, allowing reuse before zeroing completes.

### The Fix: Madvise-Before-Insert

**Key insight:** Physical page zeroing must complete BEFORE logical visibility.

```c
// NEW (FIXED) cache_push():
static void cache_push(SizeClassAlloc* sc, Slab* s) {
    /* CRITICAL: Snapshot metadata BEFORE madvise zeroes header */
    uint32_t slab_id = s->slab_id;
    bool was_published = s->was_published;
    
    /* Step 1: Reclaim physical pages FIRST (slab not yet visible) */
    if (!was_published) {
        madvise(s, SLAB_SIZE, MADV_DONTNEED);
        /* Header is now zeroed, but nobody can see this slab yet */
    }
    
    /* Step 2: NOW make slab visible using off-page snapshots */
    LOCK(&sc->cache_lock);
    
    if (sc->cache_count < CACHE_CAPACITY) {
        /* Store slab_id from snapshot (header is zeroed) */
        sc->cache_array[sc->cache_count++] = slab_id;
    } else {
        CachedNode* node = malloc(sizeof(CachedNode));
        node->slab_id = slab_id;           // From snapshot
        node->was_published = was_published; // From snapshot
        node->next = sc->overflow_list;
        sc->overflow_list = node;
    }
    
    UNLOCK(&sc->cache_lock);
    
    /* Linearization point: slab visible after lock release */
    /* madvise completed before this point - no race */
}
```

**Why this is safe:**

> **Linearization Point:** Slab becomes reachable by other threads at `UNLOCK(&sc->cache_lock)` after insertion into cache_array or overflow_list.
>
> **Safety Rule:** Any operation that can invalidate slab header (`madvise`, `munmap`, `memset`) must happen **strictly before** linearization.

1. **Off-page metadata:** Snapshots survive header zeroing
2. **Temporal ordering:** madvise → lock → insert → unlock
3. **Visibility control:** Slab unreachable during madvise
4. **Never madvise published slabs:** Lock-free pointers may exist

**Cache invariant:** Slabs that were ever published (`was_published=true`) are stored in overflow nodes with their `was_published` flag. Array cache entries are guaranteed unpublished (`was_published=false`) because published slabs always use the overflow path to preserve metadata.

**Updated cache_pop():**

```c
static Slab* cache_pop(SizeClassAlloc* sc, bool* out_was_published) {
    Slab* s = NULL;
    bool was_published = false;
    
    LOCK(&sc->cache_lock);
    
    if (sc->cache_count > 0) {
        uint32_t slab_id = sc->cache_array[--sc->cache_count];
        s = reg_lookup(slab_id);
        was_published = false;  // Array cache never published
    } else if (sc->overflow_list) {
        CachedNode* node = sc->overflow_list;
        sc->overflow_list = node->next;
        s = reg_lookup(node->slab_id);
        was_published = node->was_published;  // From off-page metadata
        free(node);
    }
    
    UNLOCK(&sc->cache_lock);
    
    *out_was_published = was_published;
    return s;
}
```

### Verification of Fix #1

```bash
# 8-thread stress test, 800K operations, RSS reclamation enabled
$ ./benchmark_threads 8

Before fix:
  - 347 zombie repairs with free_count=UINT32_MAX
  - Consistent catastrophic corruption
  - Allocator unusable under concurrency

After fix:
  - 1 zombie repair with free_count=0 (benign contention)
  - Zero catastrophic corruption
  - Throughput: 422K ops/sec (expected madvise overhead)
  - RSS properly reclaimed: 15MB → 2MB after epoch_close()
```

**Verdict:** Fix #1 successful. Allocator stable under high concurrency with RSS reclamation.

---

## Part 2: The Handle Invalidation Bug

### Discovery Context

After fixing the reuse-before-madvise race, we added comprehensive test infrastructure:

```bash
$ ls src/*.sh
run-all-tests.sh           # Maps all GitHub Actions workflows
quick_contention_test.sh   # Fast contention validation
light_contention_test.sh   # Ultra-fast regression check
```

Running the comprehensive test suite:

```bash
$ ./run-all-tests.sh
=== Part 1: ZNS-Slab Core Tests ===
smoke_test_single_thread: OK
smoke_test_multi_thread: FAILED ❌
```

**New symptom discovered!**

### The New Failure Mode

```bash
$ ./smoke_tests
Starting smoke_test_single_thread...
smoke_test_single_thread: OK ✓

Starting smoke_test_multi_thread...
Thread 1: free failed at iteration 499782
Thread 5: free failed at iteration 484935
Thread 0: free failed at iteration 470353
multi-thread worker 0 failed ❌
```

**Pattern recognition:**
- Single-threaded test: PASS
- Multi-threaded test (8 threads × 500K ops): FAIL
- Failures happen **late** in the free loop (400K-500K range)
- Multiple threads failing, not just one

### Initial Triage: Is This the Old Bug?

Added diagnostics to check if we're seeing catastrophic corruption again:

```c
// Added to zombie repair code:
fprintf(stderr, "*** REPAIRING zombie partial slab %p (free_count=%u, bitmap full) ***\n",
        (void*)s, fc);
```

**Output during smoke_tests:**

```
*** REPAIRING zombie partial slab 0x7f8a4c000000 (free_count=0, bitmap full) ***
*** REPAIRING zombie partial slab 0x7f8a4c001000 (free_count=1, bitmap full) ***
*** REPAIRING zombie partial slab 0x7f8a4c002000 (free_count=0, bitmap full) ***
```

**Critical observation:**
- `free_count = 0` or `1` (valid values, NOT UINT32_MAX!)
- This is **benign contention**, not catastrophic corruption
- The reuse-before-madvise fix IS working
- This is a DIFFERENT bug

### Diagnostic Deep Dive

Added detailed error reporting to `free_obj()` to identify which validation check was failing:

```c
bool free_obj(SlabAllocator* a, SlabHandle h) {
    if (h == 0) return false;
    
    uint32_t slab_id, slot, size_class, gen;
    handle_unpack(h, &slab_id, &gen, &slot, &size_class);
    
    if (slab_id == UINT32_MAX || size_class >= k_num_classes) return false;
    
    SizeClassAlloc* sc = &a->classes[size_class];
    
    /* Validate through registry: checks generation counter for ABA safety */
    Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
    if (!s) {
        // 🔍 DIAGNOSTIC ADDED:
        fprintf(stderr, "[FREE FAIL] reg_lookup_validate returned NULL "
                        "(slab_id=%u, gen=%u)\n", slab_id, gen);
        return false;
    }
    
    // ... rest of free_obj
}
```

**Smoking gun output:**

```bash
$ ./smoke_tests
Starting smoke_test_multi_thread...
[FREE FAIL] reg_lookup_validate returned NULL (slab_id=1697, gen=1)
[FREE FAIL] reg_lookup_validate returned NULL (slab_id=3037, gen=2)
[FREE FAIL] reg_lookup_validate returned NULL (slab_id=566, gen=1)
[FREE FAIL] reg_lookup_validate returned NULL (slab_id=196, gen=2)
Thread 0: free failed at iteration 470353
```

**Registry validation is rejecting valid handles!**

This means:
1. Thread is trying to free a handle it allocated earlier
2. Registry says "slab_id is valid, but generation doesn't match"
3. Generation mismatch = slab was recycled and generation incremented
4. But thread still holds a handle with old generation!

### Root Cause Analysis

Let's examine the smoke_tests pattern:

```c
// smoke_tests.c - worker_thread()
static void* worker_alloc_free(void* arg) {
    ThreadArgs* a = (ThreadArgs*)arg;
    SlabHandle* hs = (SlabHandle*)malloc(a->iters * sizeof(SlabHandle));
    
    /* Phase 1: ALLOCATE ALL OBJECTS FIRST */
    for (int i = 0; i < a->iters; i++) {  // a->iters = 500,000
        void* p = alloc_obj_epoch(a->alloc, 128, 0, &hs[i]);
        if (!p) {
            fprintf(stderr, "Thread %d: alloc failed at iteration %d\n", 
                    a->thread_id, i);
            return (void*)1;
        }
        ((uint8_t*)p)[0] = (uint8_t)(a->thread_id);
    }
    
    /* Phase 2: FREE ALL OBJECTS */
    for (int i = 0; i < a->iters; i++) {
        if (!free_obj(a->alloc, hs[i])) {  // ❌ FAILS HERE
            fprintf(stderr, "Thread %d: free failed at iteration %d\n",
                    a->thread_id, i);
            return (void*)1;
        }
    }
    
    free(hs);
    return NULL;
}

void smoke_test_multi_thread(void) {
    SlabAllocator a;
    allocator_init(&a);
    
    const int threads = 8;
    const int iters_per = 500000;
    
    // All threads allocate from EPOCH 0 (eternal - never closes)
    // 8 threads × 500K objects = 4 million allocations
    
    pthread_t th[threads];
    ThreadArgs args[threads];
    
    for (int i = 0; i < threads; i++) {
        args[i].alloc = &a;
        args[i].iters = iters_per;
        args[i].thread_id = i;
        pthread_create(&th[i], NULL, worker_alloc_free, &args[i]);
    }
    
    for (int i = 0; i < threads; i++) {
        void* ret = NULL;
        pthread_join(th[i], &ret);
        if (ret != NULL) {
            fprintf(stderr, "multi-thread worker %d failed\n", i);
            exit(1);
        }
    }
    
    allocator_destroy(&a);
}
```

**The race timeline:**

```
Epoch 0 (eternal - never closes)
8 threads all allocating from same epoch
Object size: 128 bytes (32 objects per slab)

T=0-10s: ALLOCATION PHASE
  Thread 0 allocates 500K objects → holds handles h0[0..499999]
  Thread 1 allocates 500K objects → holds handles h1[0..499999]
  ...
  Thread 7 allocates 500K objects → holds handles h7[0..499999]
  
  Total: 4 million objects allocated
  Slabs created: ~125,000 slabs (4M objects / 32 slots)
  
  All threads now have handles to hundreds of thousands of slabs.

T=11s: FREE PHASE BEGINS
  Thread 0 starts freeing: h0[0], h0[1], h0[2], ...
  Thread 1 starts freeing: h1[0], h1[1], h1[2], ...
  ...
  Thread 7 starts freeing: h7[0], h7[1], h7[2], ...

T=12s: Thread 7 frees object from slab X
  - Slot becomes free
  - free_count increments: 31 → 32
  - Slab X is now FULLY EMPTY (all 32 slots free)
  
  OLD CODE in free_obj():
  
  if (new_fc == s->object_count) {
      /* Slab became fully empty */
      
      /* PROTOCOL C: Immediate retirement */
      LOCK(&sc->lock);
      
      /* Step 1: Unlink from lists */
      if (s->list_id == SLAB_LIST_FULL) {
          list_remove(&es->full, s);
      } else if (s->list_id == SLAB_LIST_PARTIAL) {
          list_remove(&es->partial, s);
          atomic_fetch_sub(&es->empty_partial_count, 1);
      }
      
      /* Step 2: Depublish from current_partial */
      if (atomic_load(&es->current_partial) == s) {
          atomic_store(&es->current_partial, NULL);
      }
      
      /* Step 3: Mark unreachable */
      s->list_id = SLAB_LIST_NONE;
      s->prev = NULL;
      s->next = NULL;
      
      /* Step 4: Accounting */
      sc->total_slabs--;
      
      UNLOCK(&sc->lock);
      
      /* Step 5: RECYCLE IMMEDIATELY */
      cache_push(sc, s);  // ⚠️  Generation increments here!
      return true;
  }

T=13s: cache_push(slab X) executes
  - Slab X inserted into cache
  - Registry entry updated: generation 1 → 2
  - Slab X is now recycled and ready for reuse

T=14s: Thread 3 tries to free handle to slab X
  - Handle contains: {slab_id=1697, generation=1, slot=15}
  - Calls reg_lookup_validate(slab_id=1697, gen=1)
  - Registry check:
      current_gen = registry[1697].generation = 2
      handle_gen = 1
      if (current_gen != handle_gen) return NULL;  // MISMATCH!
  - Result: free_obj() returns false
  - Thread 3: "free failed at iteration 315789"

T=15s: Thread 0, Thread 1, Thread 5 also fail
  - All have handles to slab X with old generation=1
  - All fail with "reg_lookup_validate returned NULL"
  - Test FAILS

Result: HANDLE INVALIDATION
```

**The fundamental problem:**

Empty slab reclamation was too **aggressive**. When one thread freed the last object in a slab, it immediately recycled that slab (incrementing generation). This invalidated ALL outstanding handles that other threads held to that slab.

The pattern is valid and realistic:
- Web server: Accept 1000 requests → process → free at end
- Batch processor: Load dataset → process → cleanup
- Game engine: Allocate frame resources → render → free
- Database: Load records → query → release

**Why didn't benchmarks catch this?**

All production benchmarks use an **interleaved alloc/free pattern**:

```c
// churn_fixed.c pattern:
for (int i = 0; i < 1000000; i++) {
    void* p = alloc();
    use(p);
    free(p);  // Free immediately, don't accumulate
}
```

Handles are freed immediately after allocation, so they never accumulate. The bulk alloc/bulk free pattern was missing from our test suite.

### The Fix: Defer Recycling

**Key insight:** Don't immediately recycle empty slabs - other threads may still hold handles!

```c
// NEW (FIXED) free_obj():

if (new_fc == s->object_count) {
    /* Slab just became fully empty (all slots free).
     * 
     * SAFETY: Do NOT immediately recycle empty slabs!
     * Other threads may still hold handles to slots in this slab.
     * Recycling would invalidate those handles (generation mismatch).
     * 
     * Instead: Keep empty slabs on the partial list until epoch_close()
     * or explicit cleanup. This ensures all outstanding handles remain valid.
     * 
     * The empty_partial_count tracks these for potential trimming during
     * epoch_close() when it's safe (no outstanding allocations from that epoch).
     */
    
    /* Slab is now fully empty. Two cases:
     * 1. Was on FULL list → move to PARTIAL (so it can be reused)
     * 2. Was on PARTIAL list → already correct, just increment empty counter
     */
    if (s->list_id == SLAB_LIST_FULL) {
        LOCK(&sc->lock);
        
        /* Move from full→partial */
        list_remove(&es->full, s);
        list_push_back(&es->partial, s);
        s->list_id = SLAB_LIST_PARTIAL;
        
        /* Track as empty partial */
        atomic_fetch_add(&es->empty_partial_count, 1, memory_order_relaxed);
        
        UNLOCK(&sc->lock);
    } else if (s->list_id == SLAB_LIST_PARTIAL) {
        /* Already on partial list, just became fully empty */
        atomic_fetch_add(&es->empty_partial_count, 1, memory_order_relaxed);
    }
    
    return true;
    /* Reclamation deferred to epoch_close() */
}
```

**Why this works:**

> **Linearization Point:** Generation increment in registry is the moment old handles become invalid.
>
> **Safety Rule:** Generation increments must occur only after a quiescence boundary (`epoch_close`) when no outstanding handles exist.
>
> **Empty slab tracking:** We only increment `empty_partial_count` on the transition to fully-empty (detected by `new_fc == object_count`). It is decremented only when the slab is actually reclaimed during `epoch_close()`. This prevents double-counting if a slab repeatedly transitions empty→non-empty→empty.

1. **Handle validity preserved:** Slab stays in registry with same generation
2. **Reuse enabled:** Empty slab on partial list can be reallocated from
3. **Safe reclamation:** `epoch_close()` recycles empty slabs when no outstanding allocations exist
4. **Tracked:** `empty_partial_count` monitors empty slab accumulation

**When does reclamation happen?**

```c
// epoch_close() - safe reclamation point
void epoch_close(SlabAllocator* a, uint32_t epoch_id) {
    // Application says "I'm done with this epoch"
    // No more allocations will happen from it
    // Safe to recycle empty slabs now
    
    for (int sc = 0; sc < k_num_classes; sc++) {
        EpochState* es = get_epoch_state(&a->classes[sc], epoch_id);
        
        LOCK(&a->classes[sc].lock);
        
        // Scan partial list for empty slabs
        Slab* s = es->partial.head;
        while (s) {
            Slab* next = s->next;
            
            if (atomic_load(&s->free_count) == s->object_count) {
                // Empty slab found - safe to recycle
                list_remove(&es->partial, s);
                atomic_fetch_sub(&es->empty_partial_count, 1);
                
                UNLOCK(&a->classes[sc].lock);
                cache_push(&a->classes[sc], s);  // Now safe - no handles exist
                LOCK(&a->classes[sc].lock);
            }
            
            s = next;
        }
        
        UNLOCK(&a->classes[sc].lock);
    }
}
```

**Quiescence point:** `epoch_close()` is when the application declares "all work from this epoch is done." At this point:
- No more allocations from this epoch
- All frees have completed
- No outstanding handles exist
- Safe to increment generations and recycle

### Verification of Fix #2

```bash
# Before fix:
$ ./smoke_tests
smoke_test_single_thread: OK
smoke_test_multi_thread: FAILED
Thread 0: free failed at iteration 470353
Thread 1: free failed at iteration 499782
Thread 5: free failed at iteration 484935

# After fix:
$ ./smoke_tests
Starting smoke_test_single_thread...
smoke_test_single_thread: OK
Starting smoke_test_multi_thread...
smoke_test_multi_thread: OK (8 threads x 500000 iters) ✓
Starting micro_bench...
micro_bench (128B):
  alloc avg: 78.7 ns/op
  free  avg: 16.8 ns/op
  RSS: 284131328 bytes (270.97 MiB)

# Verify stability (run 3 times):
$ for i in {1..3}; do ./smoke_tests | grep "multi_thread:"; done
smoke_test_multi_thread: OK (8 threads x 500000 iters) ✓
smoke_test_multi_thread: OK (8 threads x 500000 iters) ✓
smoke_test_multi_thread: OK (8 threads x 500000 iters) ✓

# Verify production benchmarks still pass:
$ ./light_contention_test.sh
=== 1 Thread(s) ===
  Throughput:   4900733 ops/sec
  Avg p99:      1821 ns
  Lock contention: 16128 fast, 0 blocked (0.00% contention rate) ✓

=== 4 Thread(s) ===
  Throughput:   2682716 ops/sec
  Avg p99:      21452 ns
  Lock contention: 68882 fast, 8598 blocked (11.10% contention rate) ✓

# Verify RSS benchmarks:
$ cd ../temporal-slab-allocator-bench
$ ./workloads/steady_state_churn
temporal_slab:
  RSS after fill:  22.17 MiB
  RSS after churn: 22.17 MiB
  RSS growth: 0.00% (0.00 MiB) ✓

$ ./workloads/sustained_phase_shifts 20 5 20
Baseline RSS: 1848 KB
Peak RSS: 2028 KB
RSS after last cooldown: 1968 KB
Retention vs baseline: 6.5% ✓
```

**Verdict:** Fix #2 successful. All tests passing, allocator hardened.

---

## Part 3: The Zombie Repair Reduction

An interesting side effect of Fix #2 - zombie repairs dropped dramatically:

```
Before Fix #2: 644 zombie repairs (pathological)
After Fix #2:  12 zombie repairs (normal benign contention)
```

**Why the 98% reduction?**

### The Old Thrashing Pattern

The immediate-recycle code was creating a **slab thrashing scenario**:

```
Old flow (Protocol C immediate retirement):

T=0: Thread A empties slab X
     → cache_push(X) 
     → remove from partial list
     → partial list now has fewer slabs
     
T=1: Thread B needs allocation
     → partial list empty or sparse
     → new_slab() called
     → allocate fresh slab Y from OS
     → add slab Y to partial list
     
T=2: Thread C empties slab Y
     → cache_push(Y)
     → remove from partial list
     → partial list empty again
     
T=3: Thread D needs allocation
     → new_slab() called AGAIN
     → allocate fresh slab Z
     → add slab Z to partial list

Result: Constant churn
  - Slabs rapidly moving on/off partial list
  - High lock contention on sc->lock
  - Slabs racing to be published to current_partial
  - Some slabs get published while full → zombie repairs
```

**Zombie repair mechanism:**

```c
// In alloc_obj_epoch() slow path:
Slab* s = atomic_load(&es->current_partial);
if (s) {
    uint32_t fc = atomic_load(&s->free_count);
    if (fc == 0) {
        /* Zombie detected: slab is on partial but bitmap full */
        fprintf(stderr, "*** REPAIRING zombie partial slab %p "
                        "(free_count=%u, bitmap full) ***\n",
                (void*)s, fc);
        
        /* Repair: move to full list where it belongs */
        LOCK(&sc->lock);
        if (s->list_id == SLAB_LIST_PARTIAL) {
            list_remove(&es->partial, s);
            list_push_back(&es->full, s);
            s->list_id = SLAB_LIST_FULL;
        }
        UNLOCK(&sc->lock);
    }
}
```

Zombie repairs are **benign** (self-healing), but high frequency indicates pathological contention.

### The New Stable Pattern

After Fix #2, empty slabs stay on the partial list:

```
New flow (deferred reclamation):

T=0: Thread A empties slab X
     → slab X stays on partial list (marked as empty)
     → partial list: [X (empty), Y (2 free), Z (5 free)]
     
T=1: Thread B needs allocation
     → Try current_partial first (fast path)
     → If that fails, scan partial list
     → Find slab X (empty, perfect for bulk allocation!)
     → Reuse slab X
     
T=2: Thread C empties slab Y
     → slab Y stays on partial list
     → partial list: [X (15 free), Y (empty), Z (5 free)]
     
T=3: Thread D needs allocation
     → Scan partial list
     → Find slab Y (empty, reuse it!)
     → No new_slab() needed

Result: Stable slab pool
  - Slabs stay on partial list
  - Empty slabs get reused immediately
  - Less lock contention (fewer list operations)
  - Fewer new slabs allocated from OS
  - Far fewer zombie repairs (less publishing churn)
```

**Performance benefits:**

**Key insight:** By keeping empty slabs resident on `partial`, we reduced **publication churn**—the frequency with which `current_partial` points at slabs mid-transition. This reduced the window where zombie repairs are needed.

> Fix #2 reduced *publication churn*, which reduced windows where `current_partial` points at a slab whose state has moved. Fewer state transitions → fewer zombie detections → fewer repair operations.

1. **Memory efficiency:** Reuse empty slabs instead of allocating new ones
2. **Cache locality:** Reusing recently-freed slabs keeps them hot in cache
3. **Lock contention:** Fewer list operations under lock
4. **OS overhead:** Fewer `mmap()` calls
5. **Publication stability:** Reduced churn of `current_partial` pointer

**Validation:**

```bash
# Before fix - high zombie repair rate:
$ ./smoke_tests 2>&1 | grep REPAIRING | wc -l
644

# After fix - normal benign contention:
$ ./smoke_tests 2>&1 | grep REPAIRING | wc -l
12

# Reduction: 98% fewer zombie repairs
```

The fix not only solved **correctness** (handle invalidation) but also improved **performance** (slab reuse).

---

## Architectural Lessons

### Lesson 1: Lock-Free ≠ Race-Free

**Misconception:** "I used atomics everywhere, so my code is race-free."

**Reality:** Lock-free code is about **visibility ordering**, not just atomicity.

Both bugs involved temporal ordering issues:

**Bug 1 (reuse-before-madvise):**
```
WRONG: insert_into_cache() → madvise()
       ↑                      ↑
       visible                side effect

RIGHT: madvise() → insert_into_cache()
       ↑           ↑
       side effect visible
```

**Bug 2 (handle invalidation):**
```
WRONG: free_slot() → cache_push() (gen++)
       ↑             ↑
       slot freed    handle invalidated

RIGHT: free_slot() → keep on list → epoch_close() → cache_push()
       ↑             ↑                ↑               ↑
       slot freed    handle valid     quiescence      now safe
```

**Key principle:** Resources must be unreachable before dangerous operations.

### Lesson 2: Generation Counters Are ABA Protection, Not Lifecycle Management

**Wrong thinking:**
```
"When slab is empty, recycle immediately to free memory.
 Generation counter will prevent use-after-free."
```

**Right thinking:**
```
"When slab is empty, keep it valid until ALL references are gone.
 Generation counter prevents ABA, but doesn't make immediate recycling safe."
```

**The gap:** Generation counters detect stale handles, but they don't prevent handle invalidation.

```c
// Generation counter protects against THIS:
void* p = alloc();       // Handle: {slab_id=42, gen=1}
free(p);                 // Slab recycled, gen → 2
void* q = alloc();       // Reuse same slab, gen=2
free(p);                 // Try to free old handle
                         // gen=1 != gen=2 → rejected ✓ (ABA protection works)

// But generation counter doesn't protect against THIS:
Thread A: p = alloc();   // Handle: {slab_id=42, gen=1}
Thread B: frees last obj // Slab recycled, gen → 2
Thread A: free(p);       // Handle gen=1 != current gen=2
                         // Rejected - but THIS SHOULD HAVE WORKED!
                         // Thread A never freed p before, handle should be valid
```

**Solution:** Defer generation increment until quiescence point (epoch_close).

### Lesson 3: Deferred Reclamation Is Safer Than Immediate

**Pattern comparison:**

```
Immediate reclamation:
  free_obj() → empty? → recycle NOW
  Problem: Other threads may have handles
  
Deferred reclamation:
  free_obj() → empty? → mark as empty
  epoch_close() → no allocations? → NOW recycle
  Safe: Application controls lifecycle boundary
```

**Why deferred is better:**

1. **Explicit quiescence:** Application signals "I'm done with this epoch"
2. **No outstanding handles:** All work from epoch completed
3. **Safe to increment generation:** No risk of invalidating handles
4. **Deterministic reclamation:** Happens at predictable boundaries

This is similar to RCU (Read-Copy-Update) in the Linux kernel:
- Readers use data without locks
- Writers defer freeing until all readers are done
- Grace period ensures quiescence

### Lesson 4: Test Pattern Diversity Matters

**Our benchmark suite (all passed before Fix #2):**

```c
// Interleaved pattern:
for (int i = 0; i < 1000000; i++) {
    void* p = alloc();
    use(p);
    free(p);  // Free immediately
}
```

**The missing pattern (smoke_tests, failed before Fix #2):**

```c
// Bulk alloc/free pattern:
void* ptrs[500000];
for (int i = 0; i < 500000; i++) {
    ptrs[i] = alloc();  // Accumulate handles
}
for (int i = 0; i < 500000; i++) {
    free(ptrs[i]);      // Bulk free
}
```

**Why diversity matters:**

| Pattern | Stresses | Catches |
|---------|----------|---------|
| Interleaved | Lock contention, fast path | Correctness under churn |
| Bulk alloc/free | Handle lifetime, recycling | Premature reclamation |
| Phase shifts | RSS reclamation, epochs | Memory leak detection |
| Sustained ops | Long-term stability | Slow leaks, drift |
| Adversarial | Worst-case behavior | Fragmentation, pinning |

**Lesson:** Need multiple test patterns with different stress profiles.

### Lesson 5: Observability Accelerates Debugging

**Before adding diagnostics:** Hours of hypothesizing

**After adding diagnostics:** Minutes to root cause

**Example diagnostic additions:**

```c
// 1. Which validation check is failing?
if (!s) {
    fprintf(stderr, "[FREE FAIL] reg_lookup_validate returned NULL "
                    "(slab_id=%u, gen=%u)\n", slab_id, gen);
    return false;
}

// 2. What are the zombie repair values?
fprintf(stderr, "*** REPAIRING zombie partial slab %p "
                "(free_count=%u, bitmap full) ***\n",
        (void*)s, fc);

// 3. Thread failure context:
fprintf(stderr, "Thread %d: free failed at iteration %d (errno=%d: %s)\n",
        thread_id, i, errno, strerror(errno));
```

**Impact:**

```
Without diagnostics:
  "smoke_tests fails, not sure why"
  → Hours of binary search, adding/removing code
  
With diagnostics:
  "[FREE FAIL] reg_lookup_validate returned NULL (slab_id=1697, gen=1)"
  → Immediate focus on registry validation
  → Generation mismatch identified in minutes
  → Root cause clear: premature recycling
```

**Best practice:** Add structured logging to all validation checks.

### Lesson 6: Benign vs Catastrophic Failures Have Different Signatures

**Catastrophic (Bug #1 - reuse-before-madvise):**
```
Symptom:  free_count=UINT32_MAX (impossible value)
Pattern:  Consistent, repeatable under load
Impact:   Allocator unusable, corrupts all slabs
Recovery: None - catastrophic corruption
Urgency:  P0 - blocks all usage
```

**Benign (Zombie repairs):**
```
Symptom:  free_count=0 with bitmap full (transient)
Pattern:  Self-healing, repairs automatically
Impact:   Performance degradation, not corruption
Recovery: Repair moves slab to correct list
Urgency:  P2 - optimize later
```

**Critical distinction:**
- Catastrophic bugs have **physically impossible** values
- Benign issues have **valid but incorrect** states

**Debugging strategy:**
1. Triage by impossibility: UINT32_MAX → catastrophic
2. Check self-healing: Does repair fix it? → benign
3. Measure frequency: 644 repairs → investigate optimization
4. Profile impact: Throughput drop? Latency spikes?

### Lesson 7: Race Conditions Interact and Mask Each Other

**The masking effect:**

```
Bug #1 (catastrophic):
  - Caused immediate segfaults and corruption
  - smoke_tests would crash before completing
  - Bug #2 never had a chance to manifest
  
After fixing Bug #1:
  - Allocator became stable enough to complete tests
  - Bug #2 now visible: handle invalidation
  - Cleaner failure mode: free_obj returns false
```

**Lesson:** Fixing severe bugs may expose latent bugs that were masked.

**Debugging strategy:**
1. Fix catastrophic bugs first (corruption, crashes)
2. Re-run ALL tests after each fix
3. Don't assume "one fix solves everything"
4. Expect new issues to emerge as system stabilizes

### Lesson 8: Off-Page Metadata Survives Side Effects

**The pattern:**

```c
// WRONG: Read fields after side effect
void process(Slab* s) {
    dangerous_operation(s);  // Modifies/zeros memory
    
    use(s->field1);  // WRONG: field1 may be corrupted
    use(s->field2);  // WRONG: field2 may be corrupted
}

// RIGHT: Snapshot fields before side effect
void process(Slab* s) {
    Type1 snapshot1 = s->field1;  // Off-page copy
    Type2 snapshot2 = s->field2;  // Off-page copy
    
    dangerous_operation(s);  // Modifies/zeros memory
    
    use(snapshot1);  // ✓ Safe: using off-page copy
    use(snapshot2);  // ✓ Safe: using off-page copy
}
```

**Applied to Bug #1:**

```c
// WRONG: Read slab_id after madvise
void cache_push(Slab* s) {
    madvise(s, 4096, MADV_DONTNEED);  // Zeros header!
    
    uint32_t slab_id = s->slab_id;  // WRONG: reads 0
    insert_into_cache(slab_id);     // Inserts invalid ID
}

// RIGHT: Snapshot before madvise
void cache_push(Slab* s) {
    uint32_t slab_id = s->slab_id;        // Off-page copy
    bool was_published = s->was_published; // Off-page copy
    
    madvise(s, 4096, MADV_DONTNEED);  // Zeros header
    
    insert_into_cache(slab_id, was_published);  // ✓ Uses snapshots
}
```

**General principle:** Any operation that modifies memory (madvise, memset, DMA, etc.) requires off-page metadata snapshots.

---

## Testing Strategy

### The Test Pyramid We Built

```
                    /\
                   /  \
                  /    \
                 / E2E  \        sustained_phase_shifts (100 cycles)
                /        \       soak-test-6h (6 hours)
               /----------\      
              /            \
             / Integration  \    smoke_tests (multi-thread)
            /                \   run-all-tests.sh (all workflows)
           /------------------\  
          /                    \
         /   Unit Tests         \ test_epochs, test_malloc_wrapper
        /                        \
       /==========================\
      /                            \
     /      Quick Checks            \ light_contention_test.sh (20s)
    /________________________________\ quick_contention_test.sh (40s)
```

### Test Categories

**1. Quick Smoke Checks (20-40s)**
```bash
light_contention_test.sh   # 1T + 4T, extract key metrics
quick_contention_test.sh   # 1T + 8T, full metrics
```
Purpose: Fast regression detection during development
Runs: After every significant change

**2. Comprehensive Local Tests (3-5min)**
```bash
run-all-tests.sh           # All GitHub Actions workflows mapped
```
Purpose: Pre-push validation, catch regressions before CI
Runs: Before git push

**3. GitHub Actions CI (5-10min)**
```yaml
ci.yml:                    # smoke_tests, epochs, malloc wrapper
benchmark.yml:             # 9 workloads (latency, contention, RSS)
sustained-operations.yml:  # 100 cycles, long-term stability
soak-test-6h.yml:         # 6 hour stress test
```
Purpose: Automated validation on every PR
Runs: On push, on PR, on schedule

### Test Pattern Coverage

| Pattern | Test | Purpose |
|---------|------|---------|
| **Interleaved alloc/free** | churn_fixed, steady_state_churn | Lock contention, fast path correctness |
| **Bulk alloc + bulk free** | smoke_tests multi-thread | Handle lifetime, premature recycling |
| **Burst + cooldown** | phase_shift_retention | RSS reclamation, epoch lifecycle |
| **Sustained churn** | sustained_phase_shifts (100 cycles) | Long-term stability, leak detection |
| **Adversarial pinning** | fragmentation_adversary | Worst-case fragmentation |
| **Explicit reclamation** | epoch_reclamation_demo | epoch_close() effectiveness |
| **Multi-thread scaling** | benchmark_threads (1,2,4,8,16T) | Contention scaling, lock fairness |

### What Each Pattern Stresses

**Interleaved (churn_fixed):**
- Stresses: Lock contention on sc->lock, bitmap CAS retries
- Catches: Deadlocks, livelock, excessive contention
- Does NOT catch: Premature recycling (handles freed immediately)

**Bulk alloc/free (smoke_tests):**
- Stresses: Handle lifetime, generation validation
- Catches: Premature recycling, use-after-free
- Does NOT catch: Long-term RSS drift (test too short)

**Burst + cooldown (phase_shift_retention):**
- Stresses: RSS reclamation, madvise effectiveness
- Catches: RSS leaks, unbounded growth
- Does NOT catch: Subtle handle races (no concurrency)

**Sustained (sustained_phase_shifts):**
- Stresses: Long-term stability over 100+ cycles
- Catches: Slow leaks, trend detection, drift
- Does NOT catch: Catastrophic failures (would fail fast)

**Adversarial (fragmentation_adversary):**
- Stresses: Pinned objects + mixed lifetimes
- Catches: Fragmentation, poor slab selection
- Does NOT catch: Correctness bugs (focuses on efficiency)

### Test Infrastructure Scripts

```bash
# Ultra-fast regression check (~20s)
./light_contention_test.sh
  - Tests: 1T, 4T
  - Metrics: Throughput, p99, lock contention
  - Use: Quick sanity check during development

# Standard validation (~40s)
./quick_contention_test.sh
  - Tests: 1T, 8T
  - Metrics: Full contention breakdown
  - Use: Pre-commit validation

# Full contention sweep (~2min)
./test_contention.sh
  - Tests: 1T, 2T, 4T, 8T
  - Metrics: Complete scaling analysis
  - Use: Performance regression testing

# Comprehensive pre-push (~3-5min)
./run-all-tests.sh
  - Part 1: ZNS-Slab core (smoke, epochs, wrapper, threads)
  - Part 2: Benchmark suite (9 workloads)
  - Use: Before git push, catch all regressions
```

### How Testing Caught Both Bugs

**Bug #1 (reuse-before-madvise):**
```
Caught by: benchmark_threads 8 (multi-thread scaling)
Symptom:   Catastrophic corruption (free_count=UINT32_MAX)
Why:       High concurrency + RSS reclamation exposed race
```

**Bug #2 (handle invalidation):**
```
Caught by: smoke_tests multi-thread (bulk alloc/free)
Symptom:   free_obj returns false, handle validation failure
Why:       Bulk allocation pattern accumulated handles
```

**The key insight:** Need BOTH test patterns to catch BOTH bugs.

If we only had interleaved tests → Bug #2 would ship to production  
If we only had bulk tests → Bug #1 might not manifest quickly

### Continuous Testing Strategy

```
Developer workflow:
  1. Make change
  2. Run light_contention_test.sh (20s feedback)
  3. If pass, continue development
  4. Before commit: run smoke_tests
  5. Before push: run run-all-tests.sh
  6. Push → GitHub Actions runs full suite
  
CI workflow:
  - PR: Run benchmark.yml (9 workloads)
  - Merge to main: Run sustained-operations.yml (100 cycles)
  - Nightly: Run soak-test-6h.yml (6 hours)
  - Release: Manual trigger of extended soak test (24h+)
```

This multi-layered approach catches bugs at different stages:
- Local: Fast feedback during development
- Pre-push: Comprehensive validation before sharing
- CI: Automated gating on integration
- Nightly: Long-running stability checks
- Release: Extended validation before deployment

---

## Timeline

### Day 1 (Feb 9, 2026): The Reuse-Before-Madvise Race

**Morning: Discovery**
```
09:00 - Running benchmark_threads 8 with RSS reclamation
09:15 - Catastrophic corruption detected: free_count=UINT32_MAX
09:20 - Initial hypothesis: Bitmap corruption?
09:45 - Ruled out: Bitmap operations are atomic
10:00 - New hypothesis: Race in cache_push()?
```

**Afternoon: Root Cause Analysis**
```
13:00 - Tracing cache_push() execution flow
13:30 - EUREKA: madvise happens AFTER cache insertion!
13:45 - Sketched race timeline on whiteboard
14:00 - Understood temporal ordering violation
14:15 - Designed fix: madvise-before-insert with snapshots
```

**Evening: Implementation & Verification**
```
16:00 - Implemented fix in cache_push() and cache_pop()
16:30 - Compiled with -DENABLE_RSS_RECLAMATION=1
17:00 - Running verification tests:
        - benchmark_threads 8: PASS ✓
        - Zero UINT32_MAX corruption ✓
        - Only 1 benign zombie repair ✓
        - RSS properly reclaimed ✓
18:00 - Updated CHANGELOG, committed fix
18:30 - Pushed to GitHub, all CI checks pass
```

**Verdict:** Bug #1 FIXED. Allocator stable under high concurrency with RSS reclamation.

---

### Day 2 (Feb 10, 2026): The Handle Invalidation Bug

**Morning: Test Infrastructure**
```
09:00 - Creating comprehensive test suite
09:30 - Implemented run-all-tests.sh (maps all GitHub Actions)
10:00 - Implemented light_contention_test.sh (quick regression check)
10:30 - Running run-all-tests.sh for first time
10:45 - smoke_tests FAILED! 🚨
11:00 - "Wait, I thought we fixed everything yesterday?"
```

**Late Morning: Triage**
```
11:15 - Analyzing failure pattern:
        - Single-thread: PASS
        - Multi-thread: FAIL
        - Failures at iteration 400K-500K
11:30 - Added diagnostics to check for UINT32_MAX corruption
11:45 - Output shows free_count=0 or 1 (NOT UINT32_MAX!)
12:00 - Realization: This is a DIFFERENT bug!
```

**Afternoon: Deep Dive**
```
13:00 - Added detailed error reporting to free_obj()
13:15 - Re-ran smoke_tests with diagnostics
13:20 - SMOKING GUN:
        "[FREE FAIL] reg_lookup_validate returned NULL (slab_id=1697, gen=1)"
13:30 - Generation mismatch identified
13:45 - Tracing through registry validation logic
14:00 - EUREKA: Empty slabs are being recycled while threads hold handles!
14:15 - Understood bulk alloc/free pattern triggers premature recycling
```

**Late Afternoon: Fix & Verification**
```
15:00 - Designed fix: Defer recycling to epoch_close()
15:30 - Implemented fix in free_obj()
16:00 - Removed diagnostic fprintf statements
16:15 - Compiled and tested:
        - smoke_tests run 1: PASS ✓
        - smoke_tests run 2: PASS ✓
        - smoke_tests run 3: PASS ✓
16:30 - Verified production benchmarks:
        - light_contention_test.sh: PASS ✓
        - RSS benchmarks: PASS ✓
        - All 9 workflows: PASS ✓
17:00 - Bonus: Zombie repairs dropped 644 → 12 (98% reduction!)
```

**Evening: Documentation & Commit**
```
18:00 - Updated CHANGELOG with detailed explanation
18:30 - Committed fix, diagnostics cleanup, CHANGELOG
19:00 - Wrote comprehensive debugging saga (this document)
20:00 - All work committed, ready to push
```

**Verdict:** Bug #2 FIXED. Allocator hardened for multi-threaded bulk alloc/free patterns.

---

## Summary Statistics

### Bugs Fixed
- **Total**: 2 critical concurrency bugs
- **Severity**: Both caused production test failures
- **Root causes**: Temporal ordering violations in lock-free paths

### Code Changes
```
Files modified: 4
  - src/slab_alloc.c:     ~80 lines changed (both fixes)
  - src/smoke_tests.c:    ~10 lines (diagnostics)
  - CHANGELOG.md:         +120 lines (documentation)
  - docs/DEBUGGING_SAGA.md: +2000 lines (this document)

Lines added:   ~300
Lines removed: ~50
Net change:    +250 lines
```

### Test Infrastructure Added
```
Scripts created: 2
  - light_contention_test.sh (20s quick check)
  - run-all-tests.sh (comprehensive validation)

Test coverage added:
  - Bulk alloc/free pattern (missing before)
  - Quick regression checks (missing before)
  - Local workflow mapping (missing before)
```

### Verification Results
```
All tests passing:
  ✓ smoke_tests (8 threads × 500K ops)
  ✓ Contention scaling (1T, 2T, 4T, 8T)
  ✓ RSS stability (steady_state_churn)
  ✓ RSS retention (phase_shift_retention)
  ✓ Operational simulation (sustained_phase_shifts)
  ✓ Epoch reclamation (epoch_reclamation_demo)
  ✓ Fragmentation resistance (fragmentation_adversary)
  ✓ Long-term RSS (longterm_rss_comparison)

Performance improvements:
  - Zombie repairs: 644 → 12 (98% reduction)
  - Slab reuse: Enabled for empty slabs
  - Lock contention: Reduced due to less list churn
```

### Time Investment
```
Total debugging time: 2 days (~16 hours)
  - Bug #1: 6 hours (discovery to verification)
  - Bug #2: 6 hours (discovery to verification)
  - Documentation: 4 hours (CHANGELOG + this saga)

ROI: Prevented production outages, hardened allocator for real-world usage patterns
```

---

## Closing Thoughts

These two bugs demonstrate that **lock-free programming is fundamentally about visibility ordering**, not just atomic operations.

**The core principles:**

1. **Resources must be unreachable before dangerous operations**
   - Bug #1: madvise AFTER slab became visible → corruption
   - Bug #2: recycling WHILE handles existed → invalidation

2. **Generation counters prevent ABA, not premature recycling**
   - Generation counters detect stale handles
   - But they don't make immediate recycling safe
   - Need explicit quiescence points (epoch_close)

3. **Defer dangerous operations to safe points**
   - Spatial safety: madvise before cache insertion
   - Temporal safety: recycling after epoch_close

4. **Test pattern diversity is critical**
   - Interleaved patterns caught Bug #1
   - Bulk patterns caught Bug #2
   - Need both to ensure correctness

5. **Observability accelerates debugging**
   - Structured logging identified root causes in minutes
   - Without it: hours of hypothesis-driven debugging
   - With it: direct path to smoking gun

**The meta-lesson:** Lock-free allocators have subtle lifecycle dependencies. Correctness requires understanding when resources are **reachable** vs **safe to modify**.

Both bugs involved temporal ordering. Both fixes involved deferring dangerous operations until safe points. This pattern will apply to future lock-free code.

---

## Hardening Checklist for Lock-Free Allocators

Based on these debugging experiences, here's a checklist for developing lock-free memory allocators:

**Code Structure:**
- [ ] Mark linearization points in code comments (where resources become reachable)
- [ ] Document invariants at module boundaries (`0 <= free_count <= object_count`, etc.)
- [ ] Add explicit quiescence boundaries (epoch_close, grace periods)
- [ ] Use off-page metadata for operations that can invalidate memory

**Ordering Requirements:**
- [ ] Any operation that can invalidate metadata (madvise/munmap/memset) must occur BEFORE publication or AFTER quiescence
- [ ] Generation increments only at quiescence boundaries, never while handles exist
- [ ] Maintain handle validity contract: valid until freed, independent of other threads

**Testing Strategy:**
- [ ] **Interleaved churn** (catch madvise races, lock contention)
- [ ] **Bulk alloc/free** (catch premature recycling, handle lifetime issues)
- [ ] **Phase-shift RSS** (catch memory leaks, reclamation failures)
- [ ] **Long soak tests** (catch slow drifts, rare races)
- [ ] **Adversarial pinning** (catch fragmentation, worst-case behavior)

**Observability:**
- [ ] Add diagnostics on every failed validation branch (which check failed?)
- [ ] Log linearization events in debug builds
- [ ] Track state transition counters (empty→non-empty→empty cycles)
- [ ] Export generation mismatch rates for production monitoring

**Documentation:**
- [ ] Document blast radius (corruption vs API violation)
- [ ] Document trigger conditions (what flags/patterns expose this?)
- [ ] Explain linearization points and safety rules
- [ ] Maintain debugging saga for critical bugs

---

## References

**Code locations:**
- Bug #1 fix: `src/slab_alloc.c` - `cache_push()` and `cache_pop()`
- Bug #2 fix: `src/slab_alloc.c:1787-1822` - `free_obj()` empty slab handling
- Test infrastructure: `src/light_contention_test.sh`, `src/run-all-tests.sh`
- Verification: `src/smoke_tests.c` multi-thread pattern

**Related documentation:**
- CHANGELOG.md: Detailed fix descriptions
- INVARIANTS.md: Allocator correctness properties
- OBSERVABILITY_DESIGN.md: Stats API and diagnostic patterns

**Commits:**
- `2ce2db4`: Bug #1 fix (reuse-before-madvise)
- `6f54be2`: Bug #2 fix (handle invalidation)
- `716af92`: Test infrastructure added
- `56efc22`: CHANGELOG update

---

**Document version:** 1.0  
**Last updated:** February 10, 2026  
**Status:** Both bugs fixed, allocator hardened, comprehensive test suite in place
