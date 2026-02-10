# The Core Idea: Memory Reclamation as a Structural Property

**The fundamental innovation in one sentence:**  
**Objects don't have individual lifetimes. Phases do.**

When a phase ends, its entire memory region becomes reclaimable as a unit. This lets the allocator avoid all unpredictable behaviors that come from treating each object as an independent lifetime.

---

## Why This Matters

**The problem with per-object lifetime:**

When you treat every object as having an independent lifetime, three things go wrong:

1. **Fragmentation accumulates unpredictably**  
   Free slots scatter across memory. Allocator searches grow. Coalescing triggers at arbitrary moments. RSS drifts unbounded under steady-state workloads.

2. **Reclamation happens at the wrong moments**  
   malloc coalesces during allocation hot paths. GC pauses when heap pressure crosses heuristic thresholds. Neither aligns with application semantics—you get 100ms pauses during request handling, not between requests.

3. **You can't answer "which phase leaked memory?"**  
   malloc sees pointers. It cannot attribute RSS growth to "the /api/users route" or "frame 1847" because those concepts don't exist at the pointer level.

**The real-world cost:**

- **Tail latency violations:** p99 allocation spikes to 1-4µs (malloc measured) from allocator search times and coalescing operations triggered mid-request
- **RSS unbounded drift:** 1,111% growth under steady-state churn (malloc measured) from temporal fragmentation—old objects linger in young slabs
- **Unpredictable pauses:** 10-100ms GC stop-the-world during request handling because heap heuristic triggered
- **Blind spot in production:** RSS grows 40MB but you can't tell if it's the payment API, background task, or cache warming because malloc operates at pointer granularity

**What changes with per-phase lifetime:**

When you make phase boundaries explicit, the allocator knows:
- **When** reclamation should happen (end of request, not mid-request)
- **What** to reclaim (entire epoch's slabs, not scattered free slots)
- **Why** RSS grew (epoch 3 = /api/users route leaked 15MB)

Fragmentation becomes impossible (phases reclaim as units). Reclamation becomes deterministic (application controls timing). Observability becomes structural (metrics align with application semantics).

---

## What This Means in Code

**Traditional view (per-object lifetime):**
```c
void* obj1 = malloc(128);  // Lifetime: now → free(obj1)
void* obj2 = malloc(128);  // Lifetime: now → free(obj2)
// Allocator must handle arbitrary interleaving of allocations/frees
// Result: fragmentation, search times, unpredictable coalescing
```

**temporal-slab view (per-phase lifetime):**
```c
epoch_domain_t* request = epoch_domain_enter(alloc, "request");
void* obj1 = slab_malloc(alloc, 128);  // Lifetime: now → epoch_close(request)
void* obj2 = slab_malloc(alloc, 128);  // Lifetime: now → epoch_close(request)
epoch_domain_exit(request);
// epoch_close() reclaims ALL objects from this phase as a unit
// Result: deterministic timing, no fragmentation, bounded RSS
```

General-purpose allocators try to **infer** lifetimes from frees, heuristics, or tracing. temporal-slab makes lifetimes **explicit**, **phase-aligned**, and **deterministic**.

This gives you:
- Predictable tail latency (no emergent pathological states)
- Bounded RSS under churn (phases reclaim in bulk)
- Deterministic reclamation (application controls WHEN)
- Pure hot path (no reclamation cost during alloc/free)
- Structural observability (phase-level metrics emerge naturally)

temporal-slab is a temporal memory model with slab-level structure and epoch-level semantics.

---

## The Three Core Mechanisms

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

**temporal-slab's unique contribution:**

**Memory reclamation is a structural property of program phases, not a per-object decision.**

Everything else flows from this single shift in perspective.

**Three models for memory management:**

1. **Manual (malloc):** Programmer tracks individual pointers  
   → Fragile (use-after-free, leaks), unpredictable (fragmentation, coalescing)

2. **Automatic (GC):** Runtime traces object reachability  
   → Unpredictable (10-100ms pauses), heuristic triggers

3. **Temporal (epochs):** Application declares phase boundaries  
   → **Deterministic** (reclamation at explicit moments), **predictable** (no emergent states)

**The fundamental insight:** Many systems exhibit **structural determinism**—logical units of work have observable boundaries where all intermediate allocations become semantically dead. 

Web servers know when requests complete.  
Game engines know when frames render.  
Database transactions know when they commit.

These moments **already exist** in application logic. temporal-slab makes them explicit through `epoch_close()`, shifting the unit of memory management from individual pointers to collective phases.

**What you invented:** A temporal memory model where lifetime is defined by program phases, enabling deterministic reclamation and eliminating allocator-induced tail latency.

**Core contribution:** Making lifetimes explicit, phase-aligned, and deterministic enables both performance (predictable timing) and observability (phase-level metrics) as emergent properties of the temporal model, not features grafted onto a traditional allocator.
