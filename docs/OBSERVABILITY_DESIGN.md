# Phase 2: Observability System Design

## Goal

Turn existing perf counters and epoch infrastructure into a **stable observability surface** that:
- Explains tail latency and RSS behavior with attribution
- Stays cheap (fast path unaffected)
- Provides actionable diagnostics for production systems

---

## The Observability Contract

The system must answer four questions:

### A. Why did we go slow?
Attribute slow-path hits to specific causes:
- Cache miss (no cached slabs available)
- Current partial null/full (fast path retry)
- Epoch closed (allocation rejected)
- Lock contention (multiple threads competing)

### B. Why is RSS not dropping (or why did it drop)?
Track reclamation behavior:
- How many slabs were recycled?
- How many madvise calls succeeded/failed?
- How many bytes were reclaimed?
- Which epochs contributed to reclamation?

### C. What's happening per epoch?
Epoch-level visibility:
- How many slabs are in partial/full lists per epoch?
- Which epochs have allocations vs which are drained?
- What's the RSS footprint per epoch?

### D. What's the allocator shape right now?
Snapshot visibility:
- Cache depth and overflow depth per size class
- Slab count distribution (partial/full/cached)
- Reclaimable memory estimate
- Global RSS accounting

---

## Phase 2.0: Core Stats Infrastructure

### New Public Structs (include/slab_alloc.h)

```c
/* Global allocator statistics */
typedef struct SlabGlobalStats {
  /* Aggregate across all size classes */
  uint64_t total_slabs_allocated;      /* Sum of new_slab_count across classes */
  uint64_t total_slabs_recycled;       /* Sum of empty_slab_recycled */
  uint64_t total_slow_path_hits;       /* Sum of slow_path_hits */
  uint64_t total_cache_overflows;      /* Sum of empty_slab_overflowed */
  
  /* RSS accounting */
  uint64_t rss_bytes_current;          /* read_rss_bytes_linux() snapshot */
  uint64_t estimated_slab_rss_bytes;   /* (allocated - recycled) * 4096 */
  
  /* Epoch state summary */
  uint32_t current_epoch;              /* Active epoch for new allocations */
  uint32_t active_epoch_count;         /* Count of epochs in ACTIVE state */
  uint32_t closing_epoch_count;        /* Count of epochs in CLOSING state */
} SlabGlobalStats;

/* Per-size-class statistics */
typedef struct SlabClassStats {
  uint32_t object_size;                /* Size class (64, 96, 128, ..., 768) */
  
  /* Existing perf counters */
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;
  uint64_t current_partial_full;
  uint64_t empty_slab_recycled;
  uint64_t empty_slab_overflowed;
  
  /* NEW: Slow-path attribution */
  uint64_t slow_path_cache_miss;       /* new_slab had to mmap */
  uint64_t slow_path_epoch_closed;     /* allocation rejected (epoch CLOSING) */
  
  /* NEW: RSS reclamation tracking */
  uint64_t madvise_calls;              /* madvise(MADV_DONTNEED) invocations */
  uint64_t madvise_bytes;              /* Total bytes madvised */
  uint64_t madvise_failures;           /* madvise() returned error */
  
  /* Cache state snapshot (requires brief lock) */
  uint32_t cache_size;                 /* Slabs in array cache */
  uint32_t cache_capacity;             /* Max array cache size */
  uint32_t cache_overflow_len;         /* Slabs in overflow list */
  
  /* Slab distribution snapshot (requires brief lock) */
  uint32_t total_partial_slabs;        /* Sum across all epochs */
  uint32_t total_full_slabs;           /* Sum across all epochs */
} SlabClassStats;

/* Per-epoch statistics (within a size class or global) */
typedef struct SlabEpochStats {
  EpochId epoch_id;                    /* Epoch index (0-15) */
  uint64_t epoch_era;                  /* Monotonic generation (prevents confusion on wrap) */
  EpochLifecycleState state;           /* ACTIVE or CLOSING */
  
  /* Slab counts (requires brief lock to read list lengths) */
  uint32_t partial_slab_count;         /* Slabs with free slots */
  uint32_t full_slab_count;            /* Slabs with zero free slots */
  
  /* Estimated memory footprint */
  uint64_t estimated_rss_bytes;        /* (partial + full) * 4096 */
  
  /* Reclamation potential */
  uint32_t reclaimable_slab_count;     /* Slabs with free_count == object_count */
  uint64_t reclaimable_bytes;          /* reclaimable_slab_count * 4096 */
} SlabEpochStats;
```

### New Public APIs (include/slab_alloc.h)

```c
/* Get global allocator statistics
 * 
 * Aggregates stats across all size classes and epochs.
 * Snapshot is not atomic (counters may increment during read).
 * 
 * COST: O(classes * epochs) = O(8 * 16) = 128 iterations
 *       Brief locks on each size class (microseconds total)
 */
void slab_stats_global(SlabAllocator* alloc, SlabGlobalStats* out);

/* Get per-size-class statistics
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size_class - Size class index (0=64B, 1=96B, ..., 7=768B)
 *   out        - Output buffer (must not be NULL)
 * 
 * COST: Brief lock on cache_lock + sc->lock (microseconds)
 */
void slab_stats_class(SlabAllocator* alloc, uint32_t size_class, SlabClassStats* out);

/* Get per-epoch statistics for a specific size class
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size_class - Size class index (0-7)
 *   epoch      - Epoch ID (0-15)
 *   out        - Output buffer
 * 
 * COST: Brief lock on sc->lock to read list lengths
 */
void slab_stats_epoch(SlabAllocator* alloc, uint32_t size_class, EpochId epoch, SlabEpochStats* out);

/* Get epoch lifecycle state
 * 
 * RETURNS: EPOCH_ACTIVE or EPOCH_CLOSING
 * COST: Single atomic load (nanoseconds)
 */
EpochLifecycleState get_epoch_state(SlabAllocator* alloc, EpochId epoch);
```

---

## Phase 2.0 Implementation Plan

### Step 1: Add new counters to SizeClassAlloc

In `src/slab_alloc_internal.h`, add to `struct SizeClassAlloc`:

```c
/* Slow-path attribution (Phase 2.0) */
_Atomic uint64_t slow_path_cache_miss;    /* new_slab needed mmap */
_Atomic uint64_t slow_path_epoch_closed;  /* allocation rejected */

/* RSS reclamation tracking (Phase 2.0) */
_Atomic uint64_t madvise_calls;
_Atomic uint64_t madvise_bytes;
_Atomic uint64_t madvise_failures;
```

### Step 2: Increment counters at key points

**In alloc_obj_epoch() slow path:**
```c
// When epoch_state is CLOSING
if (atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed) == EPOCH_CLOSING) {
    atomic_fetch_add_explicit(&sc->slow_path_epoch_closed, 1, memory_order_relaxed);
    return NULL;
}
```

**In new_slab() when mmap succeeds:**
```c
Slab* s = mmap(...);
if (s != MAP_FAILED) {
    atomic_fetch_add_explicit(&sc->slow_path_cache_miss, 1, memory_order_relaxed);
    // ... rest of initialization
}
```

**In cache_push() after madvise:**
```c
#if ENABLE_RSS_RECLAMATION
int ret = madvise(slab, SLAB_PAGE_SIZE, MADV_DONTNEED);
atomic_fetch_add_explicit(&sc->madvise_calls, 1, memory_order_relaxed);
if (ret == 0) {
    atomic_fetch_add_explicit(&sc->madvise_bytes, SLAB_PAGE_SIZE, memory_order_relaxed);
} else {
    atomic_fetch_add_explicit(&sc->madvise_failures, 1, memory_order_relaxed);
}
#endif
```

### Step 3: Implement snapshot functions

**slab_stats_global() implementation:**
```c
void slab_stats_global(SlabAllocator* a, SlabGlobalStats* out) {
    memset(out, 0, sizeof(*out));
    
    /* Aggregate across size classes */
    for (uint32_t cls = 0; cls < 8; cls++) {
        SizeClassAlloc* sc = &a->classes[cls];
        out->total_slabs_allocated += atomic_load_explicit(&sc->new_slab_count, memory_order_relaxed);
        out->total_slabs_recycled += atomic_load_explicit(&sc->empty_slab_recycled, memory_order_relaxed);
        out->total_slow_path_hits += atomic_load_explicit(&sc->slow_path_hits, memory_order_relaxed);
        out->total_cache_overflows += atomic_load_explicit(&sc->empty_slab_cache_overflowed, memory_order_relaxed);
    }
    
    /* RSS snapshot */
    out->rss_bytes_current = read_rss_bytes_linux();
    out->estimated_slab_rss_bytes = (out->total_slabs_allocated - out->total_slabs_recycled) * SLAB_PAGE_SIZE;
    
    /* Epoch state summary */
    out->current_epoch = atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
    for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
        EpochLifecycleState state = atomic_load_explicit(&a->epoch_state[e], memory_order_relaxed);
        if (state == EPOCH_ACTIVE) out->active_epoch_count++;
        else out->closing_epoch_count++;
    }
}
```

**slab_stats_class() implementation:**
```c
void slab_stats_class(SlabAllocator* a, uint32_t cls, SlabClassStats* out) {
    SizeClassAlloc* sc = &a->classes[cls];
    
    /* Read atomic counters (relaxed, non-atomic snapshot) */
    out->object_size = sc->object_size;
    out->slow_path_hits = atomic_load_explicit(&sc->slow_path_hits, memory_order_relaxed);
    out->new_slab_count = atomic_load_explicit(&sc->new_slab_count, memory_order_relaxed);
    out->list_move_partial_to_full = atomic_load_explicit(&sc->list_move_partial_to_full, memory_order_relaxed);
    out->list_move_full_to_partial = atomic_load_explicit(&sc->list_move_full_to_partial, memory_order_relaxed);
    out->current_partial_null = atomic_load_explicit(&sc->current_partial_null, memory_order_relaxed);
    out->current_partial_full = atomic_load_explicit(&sc->current_partial_full, memory_order_relaxed);
    out->empty_slab_recycled = atomic_load_explicit(&sc->empty_slab_recycled, memory_order_relaxed);
    out->empty_slab_overflowed = atomic_load_explicit(&sc->empty_slab_cache_overflowed, memory_order_relaxed);
    
    /* NEW counters */
    out->slow_path_cache_miss = atomic_load_explicit(&sc->slow_path_cache_miss, memory_order_relaxed);
    out->slow_path_epoch_closed = atomic_load_explicit(&sc->slow_path_epoch_closed, memory_order_relaxed);
    out->madvise_calls = atomic_load_explicit(&sc->madvise_calls, memory_order_relaxed);
    out->madvise_bytes = atomic_load_explicit(&sc->madvise_bytes, memory_order_relaxed);
    out->madvise_failures = atomic_load_explicit(&sc->madvise_failures, memory_order_relaxed);
    
    /* Cache state (requires brief lock) */
    pthread_mutex_lock(&sc->cache_lock);
    out->cache_size = sc->cache_size;
    out->cache_capacity = sc->cache_capacity;
    out->cache_overflow_len = sc->cache_overflow_len;
    pthread_mutex_unlock(&sc->cache_lock);
    
    /* Aggregate slab counts across epochs (requires sc->lock) */
    out->total_partial_slabs = 0;
    out->total_full_slabs = 0;
    pthread_mutex_lock(&sc->lock);
    for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
        out->total_partial_slabs += sc->epochs[e].partial.len;
        out->total_full_slabs += sc->epochs[e].full.len;
    }
    pthread_mutex_unlock(&sc->lock);
}
```

**slab_stats_epoch() implementation:**
```c
void slab_stats_epoch(SlabAllocator* a, uint32_t cls, EpochId epoch, SlabEpochStats* out) {
    SizeClassAlloc* sc = &a->classes[cls];
    
    out->epoch_id = epoch;
    out->epoch_era = 0;  /* Phase 2.2: read from a->epoch_era[epoch] */
    out->state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed);
    
    /* Read list lengths (requires brief lock) */
    pthread_mutex_lock(&sc->lock);
    EpochState* es = &sc->epochs[epoch];
    out->partial_slab_count = es->partial.len;
    out->full_slab_count = es->full.len;
    
    /* Estimate reclaimable slabs (scan partial list for empty slabs) */
    out->reclaimable_slab_count = 0;
    Slab* s = es->partial.head;
    while (s) {
        uint32_t free_count = atomic_load_explicit(&s->free_count, memory_order_relaxed);
        if (free_count == s->object_count) {
            out->reclaimable_slab_count++;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&sc->lock);
    
    out->estimated_rss_bytes = (out->partial_slab_count + out->full_slab_count) * SLAB_PAGE_SIZE;
    out->reclaimable_bytes = out->reclaimable_slab_count * SLAB_PAGE_SIZE;
}
```

### Step 4: Add example diagnostic program

**src/stats_dump.c:**
```c
#include "slab_alloc.h"
#include <stdio.h>

void print_global_stats(SlabAllocator* alloc) {
    SlabGlobalStats gs;
    slab_stats_global(alloc, &gs);
    
    printf("=== Global Allocator Stats ===\n");
    printf("Current epoch: %u\n", gs.current_epoch);
    printf("Active epochs: %u\n", gs.active_epoch_count);
    printf("Closing epochs: %u\n", gs.closing_epoch_count);
    printf("\n");
    
    printf("Total slabs allocated: %lu\n", gs.total_slabs_allocated);
    printf("Total slabs recycled: %lu\n", gs.total_slabs_recycled);
    printf("Net slabs: %lu (%.2f MB)\n",
           gs.total_slabs_allocated - gs.total_slabs_recycled,
           (gs.total_slabs_allocated - gs.total_slabs_recycled) * 4096.0 / 1024 / 1024);
    printf("\n");
    
    printf("RSS (actual): %.2f MB\n", gs.rss_bytes_current / 1024.0 / 1024);
    printf("RSS (estimated): %.2f MB\n", gs.estimated_slab_rss_bytes / 1024.0 / 1024);
    printf("\n");
    
    printf("Total slow path hits: %lu\n", gs.total_slow_path_hits);
    printf("Total cache overflows: %lu\n", gs.total_cache_overflows);
}

void print_class_stats(SlabAllocator* alloc, uint32_t cls) {
    SlabClassStats cs;
    slab_stats_class(alloc, cls, &cs);
    
    printf("=== Size Class %u (%u bytes) ===\n", cls, cs.object_size);
    
    /* Slow path attribution */
    printf("Slow path breakdown:\n");
    printf("  Total hits: %lu\n", cs.slow_path_hits);
    printf("  Cache miss: %lu (%.1f%%)\n", 
           cs.slow_path_cache_miss,
           100.0 * cs.slow_path_cache_miss / (cs.slow_path_hits + 1));
    printf("  Epoch closed: %lu (%.1f%%)\n",
           cs.slow_path_epoch_closed,
           100.0 * cs.slow_path_epoch_closed / (cs.slow_path_hits + 1));
    printf("  Current partial null: %lu\n", cs.current_partial_null);
    printf("  Current partial full: %lu\n", cs.current_partial_full);
    printf("\n");
    
    /* RSS reclamation */
    printf("RSS reclamation:\n");
    printf("  madvise calls: %lu\n", cs.madvise_calls);
    printf("  madvise bytes: %.2f MB\n", cs.madvise_bytes / 1024.0 / 1024);
    printf("  madvise failures: %lu\n", cs.madvise_failures);
    printf("\n");
    
    /* Cache effectiveness */
    printf("Cache state:\n");
    printf("  Array: %u/%u\n", cs.cache_size, cs.cache_capacity);
    printf("  Overflow: %u\n", cs.cache_overflow_len);
    printf("  Recycle rate: %.1f%%\n",
           100.0 * cs.empty_slab_recycled / (cs.empty_slab_recycled + cs.empty_slab_overflowed + 1));
    printf("\n");
    
    /* Slab distribution */
    printf("Slab distribution:\n");
    printf("  Partial: %u\n", cs.total_partial_slabs);
    printf("  Full: %u\n", cs.total_full_slabs);
    printf("  Total: %u (%.2f MB)\n",
           cs.total_partial_slabs + cs.total_full_slabs,
           (cs.total_partial_slabs + cs.total_full_slabs) * 4.096);
}

int main(void) {
    SlabAllocator* alloc = slab_allocator_create();
    
    /* Simulate workload */
    for (int i = 0; i < 10000; i++) {
        void* p = slab_malloc_epoch(alloc, 128, epoch_current(alloc));
        slab_free(alloc, p);
    }
    epoch_advance(alloc);
    
    /* Dump stats */
    print_global_stats(alloc);
    print_class_stats(alloc, 2);  /* 128-byte class */
    
    slab_allocator_free(alloc);
    return 0;
}
```

---

## Phase 2.1: Epoch Attribution + epoch_close Telemetry

### New counters for epoch_close tracking

Add to `SlabAllocator` in `slab_alloc_internal.h`:

```c
/* Phase 2.1: epoch_close telemetry */
_Atomic uint64_t epoch_close_calls;            /* Total epoch_close invocations */
_Atomic uint64_t epoch_close_scanned_slabs;    /* Total slabs scanned */
_Atomic uint64_t epoch_close_recycled_slabs;   /* Total slabs recycled */
_Atomic uint64_t epoch_close_total_ns;         /* Total time spent in epoch_close */
```

### Instrumentation in epoch_close()

```c
void epoch_close(SlabAllocator* a, EpochId epoch) {
    uint64_t t_start = now_ns();  /* Optional timing */
    uint64_t scanned = 0;
    uint64_t recycled = 0;
    
    atomic_store_explicit(&a->epoch_state[epoch], EPOCH_CLOSING, memory_order_release);
    
    for (uint32_t cls_idx = 0; cls_idx < 8; cls_idx++) {
        SizeClassAlloc* sc = &a->classes[cls_idx];
        EpochState* es = &sc->epochs[epoch];
        
        pthread_mutex_lock(&sc->lock);
        
        /* Scan partial list for empty slabs */
        Slab* s = es->partial.head;
        Slab* tmp;
        while (s) {
            scanned++;
            tmp = s->next;
            uint32_t fc = atomic_load_explicit(&s->free_count, memory_order_acquire);
            if (fc == s->object_count) {
                list_remove(s, &es->partial);
                pthread_mutex_unlock(&sc->lock);
                cache_push(sc, s);  /* May madvise */
                recycled++;
                pthread_mutex_lock(&sc->lock);
                tmp = es->partial.head;
            }
            s = tmp;
        }
        pthread_mutex_unlock(&sc->lock);
    }
    
    uint64_t t_end = now_ns();
    
    /* Record telemetry */
    atomic_fetch_add_explicit(&a->epoch_close_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->epoch_close_scanned_slabs, scanned, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->epoch_close_recycled_slabs, recycled, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->epoch_close_total_ns, t_end - t_start, memory_order_relaxed);
}
```

### Add to SlabGlobalStats

```c
/* epoch_close telemetry (Phase 2.1) */
uint64_t epoch_close_calls;
uint64_t epoch_close_scanned_slabs;
uint64_t epoch_close_recycled_slabs;
uint64_t epoch_close_avg_ns;           /* total_ns / calls */
```

---

## Phase 2.2: Era Stamping (Monotonic Epoch Observability)

### Problem

Current epoch IDs are ring indices (0-15). When epoch 0 wraps around, graphs look like time went backward.

**Example confusion:**
```
Epoch 0 (era 0):  1000 allocations
Epoch 1 (era 0):  2000 allocations
...
Epoch 15 (era 0): 500 allocations
Epoch 0 (era 1):  800 allocations  ← Same epoch_id, different era!
```

### Solution: Monotonic era counter

**Add to SlabAllocator:**
```c
/* Phase 2.2: Monotonic epoch era for observability */
_Atomic uint64_t epoch_era_counter;     /* Increments on every epoch_advance */
uint64_t epoch_era[EPOCH_COUNT];        /* Era when each epoch was last activated */
```

**Add to Slab:**
```c
/* Phase 2.2: Era stamping for observability */
uint64_t era;  /* Era when slab was created/reused */
```

**Update epoch_advance():**
```c
void epoch_advance(SlabAllocator* a) {
    uint32_t prev_epoch = atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
    uint32_t next_epoch = (prev_epoch + 1) % EPOCH_COUNT;
    
    /* Mark previous epoch as CLOSING */
    atomic_store_explicit(&a->epoch_state[prev_epoch], EPOCH_CLOSING, memory_order_release);
    
    /* Advance to next epoch */
    atomic_store_explicit(&a->current_epoch, next_epoch, memory_order_release);
    
    /* Stamp era for observability (Phase 2.2) */
    uint64_t era = atomic_fetch_add_explicit(&a->epoch_era_counter, 1, memory_order_relaxed);
    a->epoch_era[next_epoch] = era + 1;
}
```

**Update new_slab():**
```c
Slab* new_slab(SlabAllocator* a, uint32_t cls, EpochId epoch) {
    /* ... mmap and initialization ... */
    
    s->epoch_id = epoch;
    s->era = a->epoch_era[epoch];  /* Phase 2.2: stamp era */
    
    /* ... rest of setup ... */
}
```

**Update SlabEpochStats to include era:**
```c
typedef struct SlabEpochStats {
    EpochId epoch_id;
    uint64_t epoch_era;  /* ← Now meaningful! Read from a->epoch_era[epoch_id] */
    /* ... rest of fields ... */
} SlabEpochStats;
```

---

## Output Format Decision

### Option A: CLI text (human-readable)

```
=== temporal-slab Stats Snapshot ===

Global:
  Current epoch: 3 (era 127)
  Active epochs: 15 | Closing: 1
  
  Total slabs: 412 allocated, 89 recycled (net: 323 = 1.3 MB)
  RSS: 2.4 MB actual | 1.3 MB estimated
  
  Slow path: 142 hits (cache miss: 89, epoch closed: 12, partial null: 41)
  Cache overflows: 3

Size Class 128B:
  Slabs: 45 partial, 12 full (228 KB RSS)
  Cache: 28/32 array, 0 overflow (100% recycle rate)
  madvise: 15 calls, 60 KB reclaimed, 0 failures
  Slow path: 18 hits (cache miss: 12, epoch closed: 2)

Epoch 2 (era 126, CLOSING):
  Slabs: 2 partial, 0 full (8 KB RSS)
  Reclaimable: 2 slabs (8 KB)
```

### Option B: JSON (machine-readable)

```json
{
  "global": {
    "current_epoch": 3,
    "current_era": 127,
    "active_epochs": 15,
    "closing_epochs": 1,
    "slabs": {
      "allocated": 412,
      "recycled": 89,
      "net": 323
    },
    "rss_mb": {
      "actual": 2.4,
      "estimated": 1.3
    },
    "slow_path": {
      "total": 142,
      "cache_miss": 89,
      "epoch_closed": 12,
      "partial_null": 41
    }
  },
  "classes": [
    {
      "index": 2,
      "object_size": 128,
      "slabs": {
        "partial": 45,
        "full": 12,
        "rss_kb": 228
      },
      "cache": {
        "array_size": 28,
        "array_capacity": 32,
        "overflow": 0,
        "recycle_rate_pct": 100.0
      },
      "madvise": {
        "calls": 15,
        "bytes": 61440,
        "failures": 0
      }
    }
  ]
}
```

**My recommendation: Start with Option A (CLI text), add Option B (JSON) in Phase 2.3 tools repo.**

Reasoning:
- Text is easier to debug during development
- JSON adds complexity (escaping, formatting)
- Tools repo can wrap text output or call APIs directly

---

## Implementation Order

### Phase 2.0 (Minimal viable observability)

1. Add new counters to `SizeClassAlloc`
2. Increment counters at key points (madvise, slow path attribution)
3. Implement `slab_stats_global()` and `slab_stats_class()`
4. Create `src/stats_dump.c` example program
5. Test: Run churn_test, then stats_dump to see attribution

**Outcome:** Can explain "why slow?" and "why RSS not dropping?"

### Phase 2.1 (Epoch attribution)

1. Add `epoch_close_*` counters to `SlabAllocator`
2. Instrument `epoch_close()` with timing and scan/recycle counts
3. Implement `slab_stats_epoch()` with reclaimable slab estimation
4. Update `stats_dump.c` to show per-epoch breakdown

**Outcome:** Can explain "which epoch is consuming memory?" and "what did epoch_close accomplish?"

### Phase 2.2 (Era stamping)

1. Add `epoch_era_counter` and `epoch_era[]` to `SlabAllocator`
2. Add `era` field to `Slab`
3. Update `epoch_advance()` to stamp era
4. Update `new_slab()` to record era
5. Include era in `SlabEpochStats` output

**Outcome:** Graphs don't confuse epoch wrap-around with time going backward

### Phase 2.3 (Tools repo - separate project)

1. Create `temporal-slab-tools` repo
2. CLI: `tslab stats`, `tslab top`, `tslab dump --json`
3. Prometheus exporter (HTTP endpoint or textfile collector)
4. Grafana dashboard JSON with:
   - RSS over time per epoch
   - Slow-path attribution pie chart
   - madvise effectiveness gauge
   - Epoch lifecycle timeline

---

## Diagnostic Patterns Enabled

### Pattern 1: RSS not dropping after epoch_close

**Query:**
```bash
./stats_dump | grep "madvise calls"
```

**Diagnosis:**
- `madvise_calls = 0` → ENABLE_RSS_RECLAMATION not set
- `madvise_failures > 0` → kernel/permission issue
- `madvise_calls > 0` but `reclaimable_slabs = 0` → slabs still have live objects

### Pattern 2: Slow path dominated by one cause

**Query:**
```bash
./stats_dump | grep "Slow path breakdown"
```

**Diagnosis:**
- `cache_miss dominant` → increase cache_capacity
- `epoch_closed dominant` → application allocating into closed epochs
- `partial_null dominant` → high contention on current_partial

### Pattern 3: Epoch stuck with memory

**Query:**
```bash
./stats_dump | grep "Epoch.*RSS"
```

**Diagnosis:**
- `partial_slabs > 0` but `reclaimable = 0` → epoch has live objects
- `reclaimable > 0` but `recycled = 0` → epoch still ACTIVE (not closed)
- `full_slabs > 0` → slabs completely full (normal, will drain as objects free)

---

## Phase 2.3: Semantic Attribution (Epoch Intelligence)

### The Gap

**Phase 2.0 tells you WHAT is happening:**
- "Epoch 5 has 12 slabs consuming 48 KB"

**Phase 2.3 tells you WHO is responsible:**
- "Epoch 5 has been open for 10 minutes, still has 1 refcount, and is holding 12 slabs"

### Why This Matters

Without application knowledge, the allocator can now answer:
- **"Which request is leaking?"** → Epoch with age > 60s and slabs > 0
- **"Why won't this epoch close?"** → refcount = 3 (domain never exited)
- **"Which epoch owns most memory?"** → Sort by bytes_owned_estimate

This turns observability into **root cause analysis**.

### Implementation

#### Add to SlabAllocator (minimal, optional):

```c
/* Phase 2.3: Epoch metadata for semantic attribution */
typedef struct EpochMetadata {
  uint64_t open_since_ns;           /* Timestamp when epoch became ACTIVE */
  uint32_t refcount;                 /* Domain refcount (0 if no domain) */
  char label[32];                    /* Optional human-readable label */
} EpochMetadata;

struct SlabAllocator {
  /* ... existing fields ... */
  
  /* Phase 2.3: Epoch intelligence */
  EpochMetadata epoch_metadata[EPOCH_COUNT];
  pthread_mutex_t metadata_lock;  /* Protects label writes only */
};
```

#### Update epoch_advance():

```c
void epoch_advance(SlabAllocator* a) {
  uint32_t prev_epoch = atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
  uint32_t next_epoch = (prev_epoch + 1) % EPOCH_COUNT;
  
  /* Mark previous epoch as CLOSING */
  atomic_store_explicit(&a->epoch_state[prev_epoch], EPOCH_CLOSING, memory_order_release);
  
  /* Advance to next epoch */
  atomic_store_explicit(&a->current_epoch, next_epoch, memory_order_release);
  
  /* Phase 2.2: Stamp era */
  uint64_t era = atomic_fetch_add_explicit(&a->epoch_era_counter, 1, memory_order_relaxed);
  a->epoch_era[next_epoch] = era + 1;
  
  /* Phase 2.3: Record open timestamp */
  a->epoch_metadata[next_epoch].open_since_ns = now_ns();
  a->epoch_metadata[next_epoch].refcount = 0;
  a->epoch_metadata[next_epoch].label[0] = '\0';  /* Clear label */
}
```

#### Add to SlabEpochStats:

```c
typedef struct SlabEpochStats {
  /* ... existing fields ... */
  
  /* Phase 2.3: Semantic attribution */
  uint64_t open_since_ns;           /* Timestamp when epoch opened */
  uint64_t age_seconds;             /* Derived: (now - open_since_ns) / 1e9 */
  uint32_t refcount;                /* Domain refcount */
  char label[32];                   /* Human-readable label */
} SlabEpochStats;
```

#### Update slab_stats_epoch():

```c
void slab_stats_epoch(SlabAllocator* alloc, uint32_t size_class, EpochId epoch, SlabEpochStats* out) {
  /* ... existing code ... */
  
  /* Phase 2.3: Add semantic attribution */
  out->open_since_ns = alloc->epoch_metadata[epoch].open_since_ns;
  uint64_t now = now_ns();
  out->age_seconds = (now - out->open_since_ns) / 1000000000ULL;
  out->refcount = alloc->epoch_metadata[epoch].refcount;
  memcpy(out->label, alloc->epoch_metadata[epoch].label, sizeof(out->label));
  
  out->estimated_rss_bytes = (uint64_t)(out->partial_slab_count + out->full_slab_count) * SLAB_PAGE_SIZE;
  out->reclaimable_bytes = (uint64_t)out->reclaimable_slab_count * SLAB_PAGE_SIZE;
}
```

### New JSON Output Section

```json
{
  "version": 1,
  "epochs": [
    {
      "epoch_id": 5,
      "state": "CLOSING",
      "age_sec": 612,
      "refcount": 1,
      "label": "request-worker-3",
      "slabs_total": 12,
      "rss_kb": 48,
      "reclaimable_kb": 8
    },
    {
      "epoch_id": 7,
      "state": "ACTIVE",
      "age_sec": 0,
      "refcount": 0,
      "slabs_total": 0,
      "rss_kb": 0
    }
  ]
}
```

### Diagnostic Patterns Enabled

#### Pattern 4: Long-lived epoch detection

**Query:**
```bash
./stats_dump | jq '.epochs[] | select(.age_sec > 60 and .slabs_total > 0)'
```

**Diagnosis:**
- Epoch open >60s with slabs → Memory leak or forgotten domain
- Check refcount: >0 means domain never exited
- Check label: identifies which subsystem owns it

#### Pattern 5: Refcount leak detection

**Query:**
```bash
./stats_dump | jq '.epochs[] | select(.refcount > 0 and .state == "CLOSING")'
```

**Diagnosis:**
- CLOSING epoch with refcount > 0 → Domain exit never called
- Application bug: `epoch_domain_enter()` without matching `exit()`

#### Pattern 6: Epoch ownership attribution

**Query:**
```bash
./stats_dump | jq '.epochs | sort_by(.rss_kb) | reverse | .[0:5]'
```

**Diagnosis:**
- Top 5 epochs by RSS → Which subsystems consume most memory
- Label shows "worker-pool-A" vs "worker-pool-B"
- Age shows if it's temporary spike or steady leak

---

## Phase 2.4: Reclaim Attempt Telemetry

### The Gap

**Current state:** You know `madvise_bytes` and `madvise_calls`.

**What's missing:** Context for interpreting madvise effectiveness.

### Why This Matters

**Scenario:** Application calls `epoch_close()`, allocator calls madvise for 10 MB, but RSS doesn't drop.

**Without Phase 2.4:** "RSS isn't dropping, dunno why"

**With Phase 2.4:** "We called madvise for 10 MB, RSS before=50 MB, RSS after=48 MB — context available for diagnosis"

### Important: RSS Is Noisy, Not a Score

**Don't compute "kernel cooperation %" from RSS deltas.** RSS moves for reasons unrelated to your madvise:
- Stack growth / shrinkage
- libc arena allocations (malloc internals)
- Page cache fluctuations
- Transparent Huge Pages (THP) promotion/demotion
- Other mmaps (shared libraries, thread stacks, etc.)

A percentage score would generate **false conclusions**.

**Instead:** Expose raw telemetry as **context only**, not as computed truth.

### Implementation

#### Add to SlabAllocator:

```c
/* Phase 2.4: Reclaim attempt telemetry (context only, not truth) */
_Atomic uint64_t last_epoch_close_rss_before;     /* RSS snapshot before last epoch_close */
_Atomic uint64_t last_epoch_close_rss_after;      /* RSS snapshot after last epoch_close */
_Atomic uint64_t last_epoch_close_madvise_bytes;  /* Bytes passed to madvise in last close */
_Atomic uint64_t last_epoch_close_timestamp_ns;   /* When did last epoch_close happen */
```

#### Update epoch_close():

```c
void epoch_close(SlabAllocator* a, EpochId epoch) {
  /* Phase 2.4: Capture RSS before reclamation (context only) */
  uint64_t rss_before = read_rss_bytes_linux();
  uint64_t madvise_bytes_in_close = 0;
  
  atomic_store_explicit(&a->epoch_state[epoch], EPOCH_CLOSING, memory_order_relaxed);
  
  for (uint32_t cls_idx = 0; cls_idx < 8; cls_idx++) {
    SizeClassAlloc* sc = &a->classes[cls_idx];
    EpochState* es = &sc->epochs[epoch];
    
    pthread_mutex_lock(&sc->lock);
    
    /* Scan partial list for empty slabs */
    Slab* s = es->partial.head;
    while (s) {
      uint32_t fc = atomic_load_explicit(&s->free_count, memory_order_relaxed);
      if (fc == s->object_count) {
        list_remove(s, &es->partial);
        pthread_mutex_unlock(&sc->lock);
        
        /* Track bytes madvised in this close */
        uint64_t old_bytes = atomic_load_explicit(&sc->madvise_bytes, memory_order_relaxed);
        cache_push(sc, s);  /* May madvise */
        uint64_t new_bytes = atomic_load_explicit(&sc->madvise_bytes, memory_order_relaxed);
        madvise_bytes_in_close += (new_bytes - old_bytes);
        
        pthread_mutex_lock(&sc->lock);
        s = es->partial.head;
      } else {
        s = s->next;
      }
    }
    pthread_mutex_unlock(&sc->lock);
  }
  
  /* Phase 2.4: Capture RSS after reclamation (context only, not truth) */
  uint64_t rss_after = read_rss_bytes_linux();
  uint64_t timestamp_ns = now_ns();
  
  atomic_store_explicit(&a->last_epoch_close_rss_before, rss_before, memory_order_relaxed);
  atomic_store_explicit(&a->last_epoch_close_rss_after, rss_after, memory_order_relaxed);
  atomic_store_explicit(&a->last_epoch_close_madvise_bytes, madvise_bytes_in_close, memory_order_relaxed);
  atomic_store_explicit(&a->last_epoch_close_timestamp_ns, timestamp_ns, memory_order_relaxed);
}
```

#### Add to SlabGlobalStats:

```c
typedef struct SlabGlobalStats {
  /* ... existing fields ... */
  
  /* Phase 2.4: Reclaim attempt context (heuristic only, not truth) */
  uint64_t last_epoch_close_rss_before_bytes;   /* Process RSS before last epoch_close */
  uint64_t last_epoch_close_rss_after_bytes;    /* Process RSS after last epoch_close */
  uint64_t last_epoch_close_madvise_bytes;      /* Bytes passed to madvise in last close */
  uint64_t last_epoch_close_timestamp_ns;       /* When did last epoch_close happen */
  int64_t last_epoch_close_rss_delta_bytes;     /* signed: rss_after - rss_before */
} SlabGlobalStats;
```

#### Update slab_stats_global():

```c
void slab_stats_global(SlabAllocator* alloc, SlabGlobalStats* out) {
  /* ... existing code ... */
  
  /* Phase 2.4: Copy reclaim attempt context (no computed scores) */
  uint64_t rss_before = atomic_load_explicit(&alloc->last_epoch_close_rss_before, memory_order_relaxed);
  uint64_t rss_after = atomic_load_explicit(&alloc->last_epoch_close_rss_after, memory_order_relaxed);
  
  out->last_epoch_close_rss_before_bytes = rss_before;
  out->last_epoch_close_rss_after_bytes = rss_after;
  out->last_epoch_close_madvise_bytes = atomic_load_explicit(&alloc->last_epoch_close_madvise_bytes, memory_order_relaxed);
  out->last_epoch_close_timestamp_ns = atomic_load_explicit(&alloc->last_epoch_close_timestamp_ns, memory_order_relaxed);
  
  /* Delta is signed: RSS can grow during epoch_close due to other allocations */
  out->last_epoch_close_rss_delta_bytes = (int64_t)rss_after - (int64_t)rss_before;
}
```

### New JSON Output

```json
{
  "version": 1,
  "last_epoch_close_context": {
    "timestamp_ns": 1704727384512000000,
    "rss_before_bytes": 52428800,
    "rss_after_bytes": 41943040,
    "rss_delta_bytes": -10485760,
    "madvise_bytes": 10485760
  }
}
```

### Diagnostic Patterns Enabled

#### Pattern 7: RSS context after epoch_close

**Query:**
```bash
./stats_dump | jq '.last_epoch_close_context | {madvise_mb: (.madvise_bytes / 1048576), rss_delta_mb: (.rss_delta_bytes / 1048576)}'
```

**Diagnosis:**
- `madvise_mb = 10, rss_delta_mb = -10` → RSS dropped by expected amount (good)
- `madvise_mb = 10, rss_delta_mb = -2` → RSS barely dropped (THP? Other allocs? Pages in use?)
- `madvise_mb = 10, rss_delta_mb = +5` → RSS *grew* during epoch_close (concurrent allocations dominating)

**Important:** This is **context**, not truth. Don't compute percentages—just expose the raw numbers for human interpretation.

#### Pattern 8: madvise effectiveness over time

**Query:**
```bash
# Sample stats every 10s, compute ratio manually
while true; do
  ./stats_dump | jq '.last_epoch_close_context | "\(.madvise_bytes) \(.rss_delta_bytes)"'
  sleep 10
done
```

**Diagnosis:**
- Consistent negative RSS delta → Reclamation working
- Erratic or positive delta → System memory pressure or concurrent allocation
- madvise_bytes = 0 → No epoch_close calls recently

---

## Phase 2.3 + 2.4 Combined Power

### Root Cause Attribution Example

```bash
./stats_dump | jq '
{
  long_lived_epochs: [
    .epochs[] | select(.age_sec > 300 and .rss_kb > 1024)
  ],
  kernel_health: .kernel_cooperation.cooperation_score_pct
}
'
```

**Output:**
```json
{
  "long_lived_epochs": [
    {
      "epoch_id": 12,
      "age_sec": 612,
      "refcount": 2,
      "label": "background-worker-3",
      "rss_kb": 4096
    }
  ],
  "kernel_health": 88.3
}
```

**Diagnosis:**
- Epoch 12 open for 10 minutes, holding 4 MB
- Refcount = 2 → Domain still has references (application bug)
- Kernel cooperation = 88% → Kernel is healthy (not the problem)
- **Root cause:** background-worker-3 never called epoch_domain_exit()

### This Is Unprecedented

**No other allocator can say:**
1. "Epoch 12 (background-worker-3) has been open for 10 minutes" (Phase 2.3)
2. "Refcount = 2, so domain never exited" (Phase 2.3)
3. "Kernel returned 88% of requested memory" (Phase 2.4)
4. "Therefore: application bug, not allocator or kernel issue"

This is **allocator-level root cause analysis**.

---

## Semantic Tightening for Production

### Critical Fixes for Stable Observability Contract

#### 1. Epoch Era Atomicity (Phase 2.2)

**Problem:** `epoch_era[]` written in `epoch_advance()`, read concurrently in `slab_stats_epoch()` and `new_slab()` → data race.

**Fix:**
```c
/* Before (WRONG) */
uint64_t epoch_era[EPOCH_COUNT];  /* Data race! */

/* After (CORRECT) */
_Atomic uint64_t epoch_era[EPOCH_COUNT];

/* Usage */
uint64_t era = atomic_fetch_add_explicit(&a->epoch_era_counter, 1, memory_order_relaxed) + 1;
atomic_store_explicit(&a->epoch_era[next_epoch], era, memory_order_relaxed);

/* Read in stats */
uint64_t era = atomic_load_explicit(&alloc->epoch_era[epoch], memory_order_relaxed);
```

#### 2. Memory Ordering Discipline

**Principle:** Relaxed everywhere except where ordering is truly required.

**For stats counters:**
```c
/* All stats counters use relaxed (no ordering needed) */
atomic_fetch_add_explicit(&sc->slow_path_cache_miss, 1, memory_order_relaxed);
atomic_load_explicit(&sc->madvise_calls, memory_order_relaxed);
```

**For epoch_state:**
```c
/* Best-effort visibility: use relaxed on both sides */
atomic_store_explicit(&a->epoch_state[epoch], EPOCH_CLOSING, memory_order_relaxed);
uint32_t state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed);

/* Document: "Allocation into closing epoch may race and succeed briefly" */
```

**Why relaxed works:** Stats are eventually consistent snapshots, not strict synchronization points.

#### 3. Reclaimable Scan Cost (Phase 2.0 Fix)

**Problem:** `slab_stats_epoch()` scans entire partial list to count empty slabs → O(n) per epoch per class → explodes in leaky scenarios.

**Options:**

**Option A (Recommended): Maintain counter**
```c
/* Add to EpochState */
typedef struct EpochState {
  SlabList partial;
  SlabList full;
  _Atomic(Slab*) current_partial;
  _Atomic uint32_t empty_partial_count;  /* NEW: updated on transitions */
} EpochState;

/* Increment when slab becomes fully free */
if (new_fc == s->object_count && old_fc != s->object_count) {
  atomic_fetch_add_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
}

/* Decrement when slab allocated from or removed */
if (old_fc == s->object_count && new_fc != s->object_count) {
  atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
}

/* Stats is now O(1) */
out->reclaimable_slab_count = atomic_load_explicit(&es->empty_partial_count, memory_order_relaxed);
```

**Option B (Bounded scan):**
```c
#define RECLAIMABLE_SCAN_LIMIT 64

uint32_t scanned = 0;
Slab* s = es->partial.head;
while (s && scanned < RECLAIMABLE_SCAN_LIMIT) {
  if (atomic_load_explicit(&s->free_count, memory_order_relaxed) == s->object_count) {
    out->reclaimable_slab_count++;
  }
  s = s->next;
  scanned++;
}
out->reclaimable_scan_limit_hit = (scanned == RECLAIMABLE_SCAN_LIMIT && s != NULL);
```

**Option C (Move to epoch_close only):**
```c
/* slab_stats_epoch() reports 0 for reclaimable */
out->reclaimable_slab_count = 0;  /* Unknown without scan */

/* Phase 2.1 telemetry tracks what epoch_close actually reclaimed */
uint64_t epoch_close_recycled_slabs;  /* What was actually done */
```

**Decision:** Use Option A (counter) for stable O(1) cost.

#### 4. Lock Ordering Discipline

**Problem:** `slab_stats_class()` takes `cache_lock` then `sc->lock`. Other code may take them in opposite order → potential deadlock.

**Global lock order (enforce everywhere):**
```
sc->lock → cache_lock
```

**In stats, avoid holding both:**
```c
/* Snapshot cache state */
pthread_mutex_lock(&sc->cache_lock);
uint32_t cache_size = sc->cache_size;
uint32_t cache_capacity = sc->cache_capacity;
uint32_t cache_overflow_len = sc->cache_overflow_len;
pthread_mutex_unlock(&sc->cache_lock);  /* Release immediately */

/* Then snapshot slab distribution */
pthread_mutex_lock(&sc->lock);
for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
  out->total_partial_slabs += sc->epochs[e].partial.len;
}
pthread_mutex_unlock(&sc->lock);
```

#### 5. Counter Name Consistency

**Problem:** `empty_slab_overflowed` vs `empty_slab_cache_overflowed` used inconsistently.

**Fix: Pick one name globally:**
```c
/* In struct SizeClassAlloc */
_Atomic uint64_t empty_slab_overflowed;  /* Use short name */

/* Everywhere */
atomic_fetch_add_explicit(&sc->empty_slab_overflowed, 1, memory_order_relaxed);

/* In PerfCounters / SlabClassStats */
uint64_t empty_slab_overflowed;  /* Match struct field name */
```

#### 6. Slow-Path Attribution Precision

**Requirement:** `slow_path_cache_miss` means "had to mmap", not just "cache empty".

**Current (CORRECT):**
```c
/* In new_slab() after successful mmap */
void* page = map_one_page();
if (page != MAP_FAILED) {
  atomic_fetch_add_explicit(&sc->slow_path_cache_miss, 1, memory_order_relaxed);
  /* ... initialize slab ... */
}
```

**Don't increment when:**
- Popping from array cache (that's a cache hit, not miss)
- Popping from overflow list (reuse, not new mapping)
- Recycling a slab (reuse, not new mapping)

**Only increment when:** Actual new slab created via mmap/mmap64.

#### 7. Output Format Determinism

**Problem:** Mixing stdout/stderr complicates golden tests and surprises shells.

**Recommended:**
```bash
# Default: text only to stdout
./stats_dump

# JSON only to stdout
./stats_dump --json --no-text

# Text only to stdout
./stats_dump --text --no-json

# Both (for interactive debugging)
./stats_dump --json --text  # Both to stdout, separated
```

**Avoid:** Text to stderr by default (makes piping confusing).

#### 8. Snapshot Metadata (Root-level JSON)

Add to every stats snapshot:
```json
{
  "schema_version": 1,
  "timestamp_ns": 1704727384512000000,
  "pid": 12345,
  "page_size": 4096,
  "slab_page_size": 4096,
  "epoch_count": 16,
  "classes": [...]
}
```

**Why:**
- `schema_version`: Forward compatibility (single root version, not nested)
- `timestamp_ns`: When was this snapshot taken
- `pid`: Which process (for dashboards with multiple processes)
- `page_size`/`slab_page_size`: Platform-specific constants
- `epoch_count`: Ring buffer size (may be configurable later)

---

## Phase Boundaries (Revised)

### Phase 2.0 (IMPLEMENTED)
- Global + per-class + minimal per-epoch stats (list lengths only)
- Slow-path attribution (cache miss, epoch closed)
- RSS reclamation tracking (madvise calls/bytes/failures)
- Dual-output tool (JSON + text)
- **No reclaimable scan** (moved to 2.1 or use O(1) counter)

### Phase 2.1 (DESIGNED)
- `epoch_close_calls`, `epoch_close_scanned_slabs`, `epoch_close_recycled_slabs`, `epoch_close_total_ns`
- Per-epoch reclaimable slab count (via O(1) counter from EpochState)

### Phase 2.2 (DESIGNED)
- `_Atomic uint64_t epoch_era_counter` (monotonic)
- `_Atomic uint64_t epoch_era[EPOCH_COUNT]` (stamped on epoch_advance)
- Slab-level `uint64_t era` field

### Phase 2.3 (DESIGNED)
- `EpochMetadata` struct: `open_since_ns`, `refcount`, `label[32]`
- Semantic attribution in stats output

### Phase 2.4 (DESIGNED)
- `last_epoch_close_rss_before/after/madvise_bytes/timestamp_ns`
- Context only, no computed "cooperation score"
- RSS delta exposed as **heuristic**, not truth

---

## Testing Strategy

### Golden File Testing
```bash
# Run workload → snapshot → compare JSON
./workload
./stats_dump --json --no-text > actual.json
diff expected.json actual.json
```

### Counter Consistency Tests
```c
/* After allocating N objects and freeing M objects */
assert(new_slab_count > 0);  /* At least one slab allocated */
assert(slow_path_cache_miss <= new_slab_count);  /* Can't mmap more than allocated */
assert(madvise_bytes <= new_slab_count * SLAB_PAGE_SIZE);  /* Can't madvise more than allocated */
```

### Race Detection (ThreadSanitizer)
```bash
make CC=gcc CFLAGS="-O1 -g -fsanitize=thread" stats_dump
TSAN_OPTIONS="halt_on_error=1" ./stats_dump
```

**Expected:** Zero data races (all counters atomic, all reads/writes have proper memory ordering).
