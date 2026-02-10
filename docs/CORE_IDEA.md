# The Core Idea: Structure Over Pointers

**temporal-slab in one sentence:**  
Memory management should follow application phase boundaries, not individual pointer lifetimes.

---

## The Three Innovations

### 1. Passive Epoch Reclamation

**The problem:** malloc operates at the pointer level (allocate/free pairs). Garbage collectors operate at the reachability level (trace object graphs). Both miss the **structural level** where applications already know "this request is done" or "this frame completed."

**The insight:** Epoch state transitions are **announcements**, not negotiations.

```c
epoch_advance(alloc);  // Atomic store, no coordination
// Old epoch now CLOSING, new epoch now ACTIVE
// Threads discover state on next allocation (acquire load)
```

**Why it matters:** Zero quiescence periods. No RCU-style grace period waiting (10-50ms). No thread coordination overhead.

**Comparison:**
- **RCU:** Requires quiescent states, synchronize_rcu() blocks
- **Hazard pointers:** 20-40 cycles per-access overhead
- **Passive epochs:** 0 cycles per-access, 0ms reclamation latency

---

### 2. Conservative Deferred Recycling

**The problem:** Traditional allocators trigger reclamation during allocation/free hot paths, causing unpredictable latency spikes.

**The insight:** Defer ALL recycling to explicit application-controlled moments.

```c
// Allocation: never recycles slabs (fast path stays fast)
void* obj = alloc_obj_epoch(alloc, 128, epoch, &handle);

// Free: never recycles slabs (just marks slot free)
free_obj(alloc, handle);

// Reclamation: happens ONLY when application says so
epoch_close(alloc, epoch);  // Scan for empty slabs, recycle ALL at once
```

**Why it matters:** Moves unpredictability from hot path (every allocation) to cold path (explicit boundary). Application controls WHEN reclamation happens, not IF.

**Measured impact:** 0% RSS growth with epoch boundaries. Without epoch_close(), RSS grows 1,033% (same as malloc's 1,111% unbounded drift).

---

### 3. Structural Observability

**The problem:** malloc operates at pointer granularity. It cannot answer "which request leaked memory?" because it doesn't know what a "request" is.

**The insight:** When the allocator tracks epochs, observability emerges naturally.

```c
// These questions are trivial for temporal-slab:
SlabEpochStats stats;
slab_stats_epoch(alloc, size_class, request_epoch, &stats);

if (stats.reclaimable_slab_count > 0) {
    printf("Request leaked %u slabs\n", stats.reclaimable_slab_count);
}
```

**Why malloc can't do this:**
- malloc sees pointers: `malloc(128)` → `0x7f8a3c00`
- temporal-slab sees phases: `alloc_obj_epoch(128, request_epoch)` → handle

The epoch parameter **is** the attribution. No sampling, no backtraces, no profiling overhead.

**Comparison:**
- **jemalloc profiling:** 10-30% overhead (stack unwinding)
- **tcmalloc profiler:** 5-15% overhead (probabilistic sampling)
- **temporal-slab:** 0% overhead (counters exist for correctness)

---

## The Production-Grade Additions

### 4. Adaptive Contention Management

**The problem:** Under high thread contention, sequential bitmap scanning creates thundering herd (all threads compete for bit 0).

**The solution:** Reactive controller switches between sequential (cache-friendly) and randomized (contention-spreading) based on observed CAS retry rate.

```c
if (retry_rate > 0.30) mode = RANDOMIZED;  // Spread threads
if (retry_rate < 0.10) mode = SEQUENTIAL;  // Optimize locality
```

**Validated impact:** 5.8× reduction in CAS retries at 16 threads (0.043 → 0.0074 retries/op).

**Why it matters:** Automatic optimization without recompilation or manual tuning. Distinguishes production system from academic prototype.

---

### 5. Diagnostic Infrastructure

**Two compile-time optional features for incident investigation:**

**Label-based attribution** (`ENABLE_LABEL_CONTENTION`):
```c
epoch_domain_t* req = epoch_domain_create(alloc, "request:GET_/users");
// All contention now attributed to "request:GET_/users"
```
Answers: "Which workload phase caused the contention spike?"

**Slowpath sampling** (`ENABLE_SLOWPATH_SAMPLING`):
```c
// Wall vs CPU time split (1/1024 sampling)
if (wall_ns >> cpu_ns * 2) {
    // Problem is OS scheduler, not allocator
}
```
Answers: "Is tail latency from allocator work or WSL2/hypervisor noise?"

**Why separate flags:** 5ns overhead acceptable for diagnostics, unacceptable for HFT production. Enable during incidents, disable for steady-state.

---

## The False Dichotomy Resolved

**Traditional view:**
```
malloc (manual)     GC (automatic)
    ↓                    ↓
Fragile            Unpredictable
(use-after-free)   (10-100ms pauses)
```

**temporal-slab:**
```
Epochs (structural)
        ↓
Deterministic reclamation without pointer tracking or GC infrastructure
```

**The key realization:** Many programs already have phase boundaries. Web servers know when requests complete. Game engines know when frames render. Database transactions know when they commit. These moments **already exist** in application logic. temporal-slab just makes them explicit.

---

## Measured Results

| Property | malloc | temporal-slab | Advantage |
|----------|--------|---------------|-----------|
| **p99 latency** | 1,463ns | 131ns | **11.2× better** |
| **p999 latency** | 4,418ns | 371ns | **11.9× better** |
| **RSS growth** | 1,111% | 0% | **Bounded** |
| **Observability** | Pointer-level | Phase-level | **Structural** |
| **Reclamation timing** | Unpredictable | Deterministic | **Application-controlled** |

**Trade-off:** +29% slower median (40ns vs 31ns), +37% baseline RSS overhead.

**When to use:** Systems where worst-case behavior matters more than average speed, and allocator-induced latency spikes are unacceptable.

---

## The Bottom Line

**temporal-slab introduces a third computational model for memory management:**

1. **Manual (malloc):** Programmer tracks individual pointers
2. **Automatic (GC):** Runtime traces object reachability
3. **Structural (epochs):** Application declares phase boundaries

The insight is that many systems exhibit **structural determinism**: logical units of work have observable boundaries where all intermediate allocations become semantically dead. temporal-slab makes this implicit structure explicit, achieving deterministic reclamation without the fragility of manual memory management or the unpredictability of garbage collection.

**Core contribution:** Shifting the unit of memory management from pointers to phases enables both performance (deterministic timing) and observability (phase-level metrics) as emergent properties, not grafted-on features.
