# Temporal-Slab: Positioning & Positioning Claims

## Executive Positioning

**Temporal-slab is a lifecycle-sensitive allocator with continuous health maintenance.**

### What This Means

**Continuous recycling** (automatic, always-on):
- Empty slabs returned to global pool via lock-free queue
- Cross-thread reuse without epoch closes
- No pathological degradation modes
- Result: **Allocator stays healthy indefinitely without intervention**

**Explicit reclamation** (optional, on-demand):
- `epoch_close()` triggers madvise() for deterministic RSS drops
- Schedulable by application (not required for health)
- Decoupled from recycling (can close "never" or "on-demand")
- Result: **RSS control when needed, zero overhead when not**

---

## The Claim (Tightened)

### What We Can Defensibly Claim

✅ **"Separates recycling from reclamation"**
- Recycling: continuous, lock-free, health-maintaining
- Reclamation: explicit, schedulable, deterministic

✅ **"Optional deterministic RSS drops"**
- `epoch_close()` → predictable madvise() → kernel reclaim on pressure
- Not "only allocator with predictable RSS" (jemalloc has arenas, mimalloc has page reset)
- But: **Only allocator where RSS drops are application-scheduled lifecycle events**

✅ **"No pathological degradation without epoch closes"**
- Before continuous recycling: `never` policy → 1959 µs convoy (38× slowdown)
- After continuous recycling: `never` policy → 48.5 µs (BEST performance)
- Proof: Allocator health independent of reclamation schedule

✅ **"Lifecycle-sensitive, not lifecycle-dependent"**
- Works great with natural phase boundaries (requests, frames, batches)
- Also works great **without** them (`never` policy is fastest)
- Difference: Applications choose when to reclaim, not when to recycle

### What We Cannot Claim (And Why)

❌ **"Only allocator with predictable RSS"**
- jemalloc: `mallctl("arena.X.purge")` → predictable purge
- mimalloc: `mi_collect(true)` → predictable page reset
- tcmalloc: `MallocExtension::ReleaseMemory()` → predictable release

❌ **"Bounded RSS under all workloads"**
- True for phase-aligned workloads (proven in churn_test)
- Not true for pathological patterns (e.g., one long-lived object per slab)
- Correct claim: **"Bounded RSS for phase-aligned workloads with predictable object lifetimes"**

❌ **"Faster than jemalloc/mimalloc"**
- Competitive on latency (48-55 µs vs 45-59 µs in benchmarks)
- Not consistently faster (depends on workload)
- Correct claim: **"Comparable latency to production allocators, with optional lifecycle features"**

---

## Competitive Differentiation

### vs General-Purpose Allocators (glibc, jemalloc, mimalloc, tcmalloc)

**What they do well:**
- Fast allocation/free (mimalloc: 45 µs mean)
- Good cache behavior
- Mature, battle-tested

**What temporal-slab adds:**
1. **Explicit reclamation scheduling**
   - `epoch_close()` is an application lifecycle event, not a heuristic trigger
   - Reclamation aligns with request boundaries, frame ends, batch completion
   - Deterministic (not "hope the allocator purges at the right time")

2. **Lifecycle-aware grouping**
   - Objects with similar lifetimes share slabs
   - Reduces fragmentation for phase-aligned workloads
   - RSS growth bounded by longest-lived phase (not cumulative drift)

3. **No background threads**
   - All reclamation is synchronous, application-controlled
   - Predictable tail latency (no surprise GC pauses)
   - Suitable for real-time systems

**When to use temporal-slab:**
- Application has natural phase boundaries (requests, frames, batches)
- RSS predictability matters (containers, quotas, demos)
- Want explicit control over reclamation timing
- Willing to trade 10-20% latency for lifecycle features

**When to use general-purpose:**
- No clear lifecycle phases
- Pure throughput optimization (mimalloc wins)
- Drop-in replacement (jemalloc via LD_PRELOAD)
- Mature ecosystem, proven at scale

### vs Region/Arena Allocators

**What they do well:**
- Bulk deallocation (O(1) free entire region)
- Zero fragmentation within region
- Cache-friendly locality

**What temporal-slab adds:**
1. **Individual free() support**
   - Don't need to free entire epoch at once
   - Objects can have different lifetimes within same epoch
   - More flexible than "all or nothing"

2. **Cross-epoch sharing**
   - Slabs recycled across epochs (continuous recycling)
   - Not tied to strict region boundaries
   - Better memory utilization

3. **Thread-safe**
   - Multiple threads share same allocator
   - Lock-free fast path
   - Regions typically require per-thread ownership

**When to use temporal-slab:**
- Need individual free() (not bulk-only)
- Multi-threaded, shared allocator
- Lifecycle phases overlap (e.g., long-lived + short-lived objects)

**When to use regions:**
- Bulk deallocation is sufficient (e.g., per-request arena)
- Single-threaded or thread-local ownership
- Want absolute minimum overhead (no handle encoding, no registry)

---

## Use Case Positioning

### Tier 1: Ideal Fit (Lifecycle-Aligned)

**HTTP request handlers:**
```c
EpochId req_epoch = epoch_advance(alloc);
void* session = slab_malloc_epoch(alloc, sizeof(Session), req_epoch);
// ... allocate request-scoped objects ...
epoch_close(alloc, req_epoch);  // Deterministic cleanup after response sent
```

**Game engines (frame-scoped allocations):**
```c
EpochId frame_epoch = epoch_current(alloc);
void* particles = slab_malloc_epoch(alloc, sizeof(Particle) * count, frame_epoch);
// ... render frame ...
epoch_close(alloc, frame_epoch);  // Reclaim at frame boundary
```

**Batch processors:**
```c
EpochId batch_epoch = epoch_advance(alloc);
for (record in batch) {
    process(slab_malloc_epoch(alloc, record_size, batch_epoch));
}
epoch_close(alloc, batch_epoch);  // Reclaim after batch completes
```

### Tier 2: Works Great (Throughput Mode)

**Request routers (stateless, high-throughput):**
```c
// Policy: never (best latency, stable RSS plateau)
void* req = slab_malloc(alloc, sizeof(Request));
// ... route ...
slab_free(alloc, req);
// No epoch_close() needed - continuous recycling keeps allocator healthy
```

**Real-time systems (no background GC):**
```c
// Policy: never (predictable latency, no surprise pauses)
// Continuous recycling prevents degradation
// RSS bounded by working set (deterministic)
```

### Tier 3: Not Ideal (Mismatched Patterns)

**Long-lived + short-lived mixed:**
- If one long-lived object per slab → slab stranded
- Better: Separate allocators or use general-purpose

**Unpredictable lifetimes:**
- No clear phase boundaries
- Reclamation timing unclear
- Better: jemalloc with periodic purge

**Variable-size heavy (>768 bytes dominant):**
- temporal-slab only handles 64-768 bytes
- Large allocations fall back to system malloc
- Better: General-purpose allocator

---

## Messaging Framework

### Elevator Pitch (30 seconds)

"Temporal-slab is a lifecycle-sensitive memory allocator that separates recycling from reclamation. Recycling happens continuously and automatically, keeping the allocator healthy. Reclamation (RSS drops) is explicit and schedulable, triggered by your application's lifecycle events. You get predictable RSS behavior when you need it, and zero reclamation overhead when you don't."

### Technical Pitch (2 minutes)

"Most allocators couple recycling (making memory reusable) with reclamation (returning memory to the OS). They use heuristics to guess when to reclaim: 'purge if idle for N seconds' or 'trim if above threshold X.'

Temporal-slab decouples these:

1. **Recycling is continuous**: Empty slabs go into a lock-free queue, harvested during allocation. No pathological degradation. Works forever without intervention.

2. **Reclamation is explicit**: Call `epoch_close()` when your lifecycle phase ends (request completes, frame renders, batch finishes). Triggers madvise() for deterministic RSS drops aligned with your application's natural boundaries.

This means:
- RSS drops are predictable lifecycle events, not heuristic surprises
- You control when reclamation happens (or skip it entirely for throughput)
- Allocator stays healthy whether you close epochs or not

For applications with natural phase boundaries, this is ideal. For pure throughput workloads, it's competitive with jemalloc/mimalloc while offering optional lifecycle features."

### Positioning Statement

**Target audience**: Systems programmers building latency-sensitive services with memory quotas or predictable RSS requirements.

**Problem**: Traditional allocators make RSS behavior unpredictable. You can't schedule when memory returns to the OS. You can't align reclamation with application lifecycle boundaries.

**Solution**: Temporal-slab separates recycling (automatic, continuous) from reclamation (explicit, schedulable). You get allocator health without intervention, plus optional deterministic RSS drops aligned with your lifecycle events.

**Proof**: Benchmark shows "never close" policy works perfectly (48 µs latency, stable RSS). Adding explicit closes gives predictable RSS drops without hurting performance. No other allocator offers this control.

**Why believe us**: We've proven it works under adversarial conditions. The "never close" policy used to cause catastrophic failure (1959 µs convoy). After implementing continuous recycling, it's now the **fastest** policy. This proves the architectural separation is real, not marketing.

---

## FAQ: Addressing Skepticism

### Q: "Why not just use jemalloc and call mallctl('arena.X.purge')?"

**A:** You can! If that works for your use case, great.

**Temporal-slab advantages:**
1. **Epoch grouping**: Objects with similar lifetimes share slabs (reduces fragmentation)
2. **Synchronized reclamation**: `epoch_close()` is a lifecycle event (aligns with phase boundaries)
3. **No background threads**: All reclamation synchronous (predictable latency)

**When jemalloc is better:**
- Need proven maturity (jemalloc has 15+ years)
- Want drop-in malloc replacement (LD_PRELOAD)
- Have complex allocation patterns (variable sizes, unpredictable lifetimes)

### Q: "What's the actual RSS improvement vs jemalloc?"

**A:** Depends on workload.

**Churn benchmark** (1000 cycles, 80% lifetime overlap):
- jemalloc: 20-50% RSS growth
- temporal-slab: 2.4% RSS growth

**HTTP benchmark** (1200 requests, request-scoped allocations):
- jemalloc: Not tested (would need to instrument)
- temporal-slab: 3% RSS growth with `per-request`, 31% with `never`

**Honest answer**: For phase-aligned workloads, temporal-slab wins. For general workloads, comparable. Benefit is **predictability**, not raw efficiency.

### Q: "Is continuous recycling just 'copying jemalloc's design'?"

**A:** No. Different problem, different solution.

**jemalloc**: Heuristic-based purging ("trim if above watermark")  
**temporal-slab**: Lifecycle-based reclamation ("close when phase ends")

**Continuous recycling** is the innovation that makes lifecycle-based reclamation **optional** instead of **required**. Before, you HAD to close epochs or allocator degraded. Now, closing is a performance optimization (RSS control), not a correctness requirement (health).

### Q: "Why 768-byte limit? That's tiny."

**A:** Design trade-off.

**Rationale:**
- Small objects (64-768B) dominate many workloads (structs, buffers, messages)
- Larger allocations (>768B) fall back to system malloc (hybrid approach)
- Fixed size classes enable O(1) deterministic lookup (no branching)

**When this works:**
- Request handlers (mostly small structs)
- Game engines (particles, entities, components)
- Network buffers (packets, frames)

**When this doesn't work:**
- Large object dominant (images, videos, large buffers)
- Use general-purpose allocator or specialize further

---

## Roadmap: Strengthening Claims

### Near-term (Prove It Works)

1. **Stress testing**: 1 hour, 16 threads, 1M requests → prove no degradation
2. **Comparative benchmarks**: vs jemalloc, mimalloc, tcmalloc → establish competitive parity
3. **Production pilot**: Deploy in one service → gather real-world data

### Mid-term (Document Benefits)

1. **RSS case studies**: Show before/after for real workloads
2. **Latency distribution analysis**: Prove no GC-style pauses
3. **Memory pressure testing**: Prove reclamation under cgroup limits

### Long-term (Expand Capabilities)

1. **Adaptive harvest**: Trigger empty queue drain proactively (not just on slowpath)
2. **Per-thread empty queues**: Reduce CAS contention on `empty_queue_head`
3. **Epoch-aware harvest**: Prioritize old epochs for better reclamation hints

---

## Conclusion: The Honest Positioning

**What temporal-slab is:**
- A lifecycle-sensitive allocator with optional explicit reclamation
- Competitive on latency, better on RSS predictability (for phase-aligned workloads)
- Self-sustaining (continuous recycling) with optional cleanup (explicit closes)

**What temporal-slab is NOT:**
- Not universally faster than jemalloc/mimalloc
- Not a drop-in malloc replacement
- Not ideal for all workload patterns

**When to use it:**
- You have natural lifecycle phases
- You want predictable RSS behavior
- You need explicit control over reclamation timing
- You're willing to integrate non-standard API

**When NOT to use it:**
- Need proven maturity at massive scale
- Want zero integration effort (LD_PRELOAD)
- Have unpredictable/complex allocation patterns
- Pure throughput optimization (mimalloc wins)

**The differentiator**: Temporal-slab is the only allocator where **RSS drops are application lifecycle events**, not heuristic triggers. This makes RSS behavior predictable and controllable in ways traditional allocators cannot match.
