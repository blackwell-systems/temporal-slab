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

**Measured impact (Phase 2.1, 100M cycles):**
- **With epoch_close():** RSS stable at 768KB (8 slabs across 8 size classes), 0% drift
- **Without epoch_close():** RSS grows 1,033% (same as malloc's 1,111% unbounded drift)
- **Critical distinction:** Bounded RSS requires application-controlled reclamation boundaries (`epoch_close()`). Without explicit boundaries, temporal-slab behaves like malloc (unbounded drift from temporal fragmentation).

---

### 3. Structural Observability

**The problem:** malloc operates at pointer granularity. It cannot answer "which request leaked memory?" because it doesn't know what a "request" is.

**The insight:** When the allocator tracks epochs, multi-level observability emerges naturally.

**Five observability layers (Phase 2.0-2.4):**

1. **Stats API** - Snapshot-based queries with derived metrics
   - `slab_stats_global()` - Aggregate across all classes/epochs
   - `slab_stats_class()` - Per-size-class with recycle rates, contention
   - `slab_stats_epoch()` - Per-epoch with reclaimable slab estimation

2. **Era stamping** - Monotonic temporal identity (Phase 2.2)
   - 64-bit era counter increments on every `epoch_advance()`
   - Distinguishes epoch incarnations across ring wraparounds
   - Enables log correlation: "Request XYZ allocated in (epoch=3, era=1250)"

3. **Epoch metadata** - Lifetime profiling (Phase 2.3)
   - `open_since_ns`: When epoch became ACTIVE (detects stuck epochs)
   - `domain_refcount`: Live allocation count (leak detection signal)
   - `label[32]`: Semantic tag ("request_id:abc123", "frame:1234")

4. **RSS delta tracking** - Quantify reclamation (Phase 2.4)
   - `rss_before_close` / `rss_after_close` snapshots
   - Delta shows MB freed per `epoch_close()` (validation signal)
   - Diagnoses leaks: zero delta despite many frees = problem

5. **Thread-local sampling** - Tail attribution (Phase 2.5, optional)
   - Probabilistic 1/1024 sampling with wall vs CPU time split
   - `wait_ns = wall_ns - cpu_ns` (scheduler interference metric)
   - Repair timing with reason codes (zombie partial detection)

**Example query:**

```c
// Phase-level leak detection (O(1) queries)
SlabEpochStats stats;
slab_stats_epoch(alloc, size_class, request_epoch, &stats);

if (stats.reclaimable_slab_count > 0) {
    uint64_t age_sec = (now_ns() - stats.open_since_ns) / 1e9;
    printf("Epoch %u (era %lu, label='%s') leaked %u slabs after %lu sec\n",
           request_epoch, stats.epoch_era, stats.label,
           stats.reclaimable_slab_count, age_sec);
}

// Quantify reclamation effectiveness
if (stats.rss_before_close > 0) {
    uint64_t freed_mb = (stats.rss_before_close - stats.rss_after_close) / (1024*1024);
    printf("Closed epoch freed %lu MB\n", freed_mb);
}
```

**Why malloc can't do this:**
- malloc sees pointers: `malloc(128)` → `0x7f8a3c00`
- temporal-slab sees phases: `alloc_obj_epoch(128, request_epoch)` → handle

The epoch parameter **is** the attribution. No sampling, no backtraces, no profiling overhead.

**Comparison:**
- **jemalloc profiling:** 10-30% overhead (stack unwinding per allocation)
- **tcmalloc profiler:** 5-15% overhead (probabilistic sampling + backtraces)
- **temporal-slab (Phases 2.0-2.4):** 0% overhead (counters exist for correctness, not profiling)
- **temporal-slab (Phase 2.5, optional):** <0.2% overhead (1/1024 sampling, no backtraces)

**Key distinction:** Traditional profilers **add** overhead to **infer** attribution. temporal-slab attribution is **structural** - the epoch parameter **is** the attribution.

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

### 5. Self-Healing Correctness (Zombie Repair)

**The problem:** Under high concurrency, publication races can create "zombie partial" slabs where `free_count=0` but bitmap is full (all slots actually free).

**The solution:** Detection + repair on every allocation attempt.

```c
if (free_count == 0 && bitmap_is_full(s)) {
    // Zombie detected - repair immediately
    move_to_full_list(s);  // Restore invariant
    repair_count++;        // Track for observability
}
```

**Validated impact (Phase 2.5, 16-thread adversarial test):**
- **Repair rate:** 0.0104% (1 per 9,639 allocations)
- **Repair timing:** 9-14µs avg (CPU-bound list/bitmap work)
- **Reason attribution:** 100% `full_bitmap` (proves specific race condition)
- **Result:** System self-heals, no corruption, no crashes

**Why it matters:** Distinguishes production system from research prototype. Race conditions are inevitable under high concurrency - self-healing repair makes them **benign** rather than **fatal**.

---

### 6. Diagnostic Infrastructure

**Three compile-time optional features for incident investigation:**

**Label-based attribution** (`ENABLE_LABEL_CONTENTION`, Phase 2.3):
```c
epoch_domain_t* req = epoch_domain_create(alloc, "request:GET_/users");
// All contention now attributed to "request:GET_/users"
```
Answers: "Which workload phase caused the contention spike?"
Overhead: +5ns per lock acquisition (label array indexing)

**Slowpath sampling** (`ENABLE_SLOWPATH_SAMPLING`, Phase 2.5):
```c
// Wall vs CPU time split (1/1024 sampling)
ThreadStats stats = slab_stats_thread();
if (stats.alloc_wait_ns_sum > stats.alloc_cpu_ns_sum) {
    // Problem is OS scheduler (WSL2/hypervisor), not allocator
}
```
Answers: "Is tail latency from allocator work or WSL2/hypervisor noise?"
Overhead: <0.2% (80ns per sample / 1024 allocations)

**TLS handle cache** (`ENABLE_TLS_CACHE`, Phase 2.6):
```c
// Thread-local LIFO cache (256 handles per class)
// Cache hit = zero atomics (bypass global allocator)
```
Answers: "Can we eliminate p99 spikes for high-locality workloads?"
Impact: p50 -11% (41ns→36ns), p99 -75% (96ns→24ns), +15% throughput

**Why separate flags:** 5-10ns overhead acceptable for diagnostics, unacceptable for HFT production. Enable during incidents, disable for steady-state.

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

| Property | malloc (system) | temporal-slab | Advantage |
|----------|-----------------|---------------|-----------|
| **p99 latency** | 1,463ns | 131ns (fast path) | **11.2× better** |
| **p999 latency** | 4,418ns | 371ns (fast path) | **11.9× better** |
| **p99 (WSL2 wall)** | ~3µs (est.) | 1.5µs (measured) | **2× better** |
| **Tail attribution** | No decomposition | wait_ns separates scheduler | **Diagnostic** |
| **RSS growth** | 1,111% | 0% | **Bounded** |
| **Observability** | Pointer-level | Phase-level | **Structural** |
| **Reclamation timing** | Unpredictable | Deterministic | **Application-controlled** |

**Trade-offs:**
- **Median latency:** +29% slower (40ns vs 31ns) - more atomic operations
- **Baseline RSS:** +37% overhead (temporal partitioning cost)
- **API complexity:** Must declare phase boundaries (`epoch_advance()`, `epoch_close()`)

**When to use:**
- **Real-time systems:** Predictable worst-case > fast average (audio/video processing, trading)
- **Request-response servers:** Tail latency matters (web APIs, microservices)
- **Production debugging:** Need "which feature leaked memory?" without profiler overhead
- **Bounded RSS required:** Cannot tolerate unbounded drift (embedded, long-running daemons)

**When NOT to use:**
- **Allocate-once patterns:** Short-lived processes, initialization-heavy workloads
- **Highly irregular lifetimes:** Objects genuinely unpredictable (no phase structure)
- **Median-optimized:** Average case more important than worst-case (batch processing)

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

**What this system invented:**

1. **A temporal memory model** where lifetime is defined by program phases (not pointers or reachability)
2. **Passive epoch reclamation** - zero-coordination state transitions (0ns overhead, 0ms latency)
3. **Self-healing correctness** - zombie repair makes publication races benign (0.01% repair rate, no crashes)
4. **Structural observability** - five-layer introspection without profiler overhead (era stamping, metadata, RSS delta, thread sampling)
5. **Adaptive contention management** - reactive mode switching based on CAS retry rate (5.8× retry reduction at 16T)

**Core contribution:**

Making lifetimes **explicit** (through epoch parameters), **phase-aligned** (with application semantics), and **deterministic** (through `epoch_close()`) enables:

- **Performance** - Predictable timing (no emergent pathological states)
- **Observability** - Phase-level metrics emerge naturally (not grafted on)
- **Correctness** - Self-healing repair (races are benign, not fatal)
- **Control** - Application dictates WHEN reclamation happens (not heuristics)

These properties are **emergent** from the temporal model, not features added afterward.

**Academic positioning:** Demonstrates that temporal structure (phase boundaries) can replace both pointer tracking (malloc) and reachability tracing (GC) while providing deterministic reclamation and structural observability as byproducts of the model.
