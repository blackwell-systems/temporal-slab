# Additions Needed for foundations.md

## Missing Comparative Content

Based on whitepaper analysis, foundations.md needs these additions to help readers understand alternatives, differences, and tradeoffs:

### 1. Memory Management Approaches: Complete Taxonomy

**Where to add:** After "Temporal Fragmentation" section, before "Slab Allocator"

**Content needed:**
- **Manual memory management (malloc/free)**: How it works, fragmentation sources, no lifetime tracking
- **Garbage collection (mark-sweep, generational, concurrent)**: Stop-the-world pauses, pause time vs throughput tradeoff
- **Reference counting (Swift ARC, Python)**: Cycle detection problem, contention on shared objects
- **Region-based (arena allocators, APR pools)**: Stack-like allocation, cannot handle interleaved lifetimes
- **Epoch-based (temporal-slab)**: Phase-aligned grouping, deterministic reclamation

**Decision matrix**: When to use each approach based on workload characteristics

### 2. malloc Variants Deep Dive

**Where to add:** After "Memory Allocator" section

**Content needed:**

**ptmalloc2 (glibc malloc)**:
- Arena-per-thread design
- Free list + binning strategy
- Fragmentation characteristics
- Why it suffers from temporal fragmentation (no lifetime awareness)

**jemalloc (Facebook, Rust default)**:
- Size-class segregation (similar to slab)
- Thread-local caching
- Low fragmentation but still temporal drift
- Profiling overhead: 10-30% when enabled

**tcmalloc (Google, used in Chrome)**:
- Per-thread cache + central freelist
- Aggressive caching strategies
- RSS characteristics vs temporal-slab

**Comparison table**:
| Allocator | Avg Alloc Latency | p99 Latency | RSS Drift | Observability Overhead |
|-----------|-------------------|-------------|-----------|------------------------|
| ptmalloc2 | 50ns | 1,443ns | Unbounded | N/A (none available) |
| jemalloc | 45ns | ~800ns | Moderate | 10-30% (if profiling enabled) |
| tcmalloc | 40ns | ~600ns | Moderate | 5-15% (heap profiler) |
| temporal-slab | 40ns | 131ns | 0% (with epochs) | 0% (structural) |

### 3. Garbage Collection Mechanics

**Where to add:** New section after "Fragmentation as Entropy"

**Content needed:**

**Mark-Sweep GC**:
- How tracing works (root set → reachability graph)
- Why it causes pauses (world must stop while marking)
- Memory overhead (mark bits, stack scanning)

**Generational GC (Java G1, .NET)**:
- Young generation assumption (most objects die young)
- Minor vs major collections
- Pause times: 10-100ms typical

**Concurrent GC (Go, Azul Zing)**:
- Attempts to reduce pause times
- Write barriers required (overhead on pointer updates)
- Still has brief stop-the-world phases

**Why temporal-slab doesn't need GC**:
- Explicit lifetime management via epochs
- No tracing (epochs track lifetimes structurally)
- No pauses (reclamation at deterministic boundaries)

**Tradeoff comparison**:
```
GC advantages:
+ Memory safety (no use-after-free)
+ No manual tracking (compiler handles)

GC disadvantages:
- Unpredictable pauses (10-100ms)
- Write barrier overhead (5-10% throughput cost)
- Heap size tuning complexity
- No control over reclamation timing

temporal-slab:
+ Deterministic reclamation (epoch_close)
+ No pauses (constant-time operations)
+ Zero GC overhead
- Requires explicit epoch management
- No automatic safety (C/C++ pointers)
```

### 4. RCU and Epoch-Based Reclamation (Linux Kernel)

**Where to add:** After "Hazard Pointers and Reference Counting"

**Content needed:**

**Read-Copy-Update (RCU)**:
- Grace period concept (wait for all CPUs to reach quiescent state)
- rcu_read_lock/rcu_read_unlock
- call_rcu deferred callbacks
- Why it's not suitable for general allocation (quiescence overhead)

**Comparison table**:
| Mechanism | Quiescence Required? | Per-Access Overhead | Reclamation Trigger |
|-----------|---------------------|---------------------|---------------------|
| RCU | Yes (grace period) | 10-20ns (barriers) | After all threads quiesce |
| Hazard Pointers | No | 20-80ns (publish/unpublish) | Scan on every free |
| Passive Epoch (temporal-slab) | No | 0ns | Explicit epoch_close() |

**Why temporal-slab is "passive"**:
- No grace period waiting (threads observe CLOSING state asynchronously)
- No per-access overhead (epochs tracked per allocation, not per access)
- Deterministic timing (application controls when reclamation happens)

### 5. Region-Based Systems Comparison

**Where to add:** New section after "Epoch Domains"

**Content needed:**

**Region inference (Cyclone, MLKit)**:
- Compiler analyzes lifetimes statically
- Generates region push/pop operations
- Limitations: Cannot express non-stack lifetimes (sessions outliving requests)

**Apache APR pools**:
- Manual region management (create pool, allocate from pool, destroy pool)
- Problem: Allocations from different phases intermingle if using same pool
- No temporal isolation (pool ≠ epoch)

**Arena allocators (common in C)**:
- Similar to APR pools but simpler
- No threading support
- No RSS reclamation (grows until destruction)

**temporal-slab vs regions**:
```
Region allocators:
+ Simple (linear allocation)
+ Fast (bump pointer)
- Stack-only lifetimes (no nesting)
- No partial reclamation (all-or-nothing)
- No threading support

temporal-slab epochs:
+ RAII with refcounts (nested epochs)
+ Partial reclamation (slabs returned as empty)
+ Thread-safe (lock-free fast path)
- Slower than bump pointer (bitmap allocation)
- Fixed size classes only
```

### 6. When NOT to Use temporal-slab (Decision Matrix)

**Where to add:** Expand existing "When to use temporal-slab" section at document start

**Content needed:**

**Decision flowchart:**
```
START: Choosing memory management approach

1. Is safety more important than performance?
   YES → Use GC (Go, Java, C#)
   NO → Continue

2. Do objects have correlated lifetimes?
   NO → Use malloc/jemalloc (general-purpose)
   YES → Continue

3. Are allocation sizes ≤768 bytes?
   NO → Use malloc (large objects)
   YES → Continue

4. Is allocation rate >10K/sec?
   NO → Use malloc (overhead not justified)
   YES → Continue

5. Can you identify phase boundaries?
   NO → Use jemalloc (better than malloc for churn)
   YES → Use temporal-slab

END
```

**Anti-patterns for temporal-slab:**
1. Desktop GUI applications (user-driven lifetimes, unpredictable)
2. Long-lived uniform objects (database with million-row cache loaded at startup)
3. Variable-size allocations >768 bytes (video frames, file buffers)
4. Low allocation rate (<1K/sec) (CLI tools, config parsers)
5. Circular data structures with complex ownership (compilers, graph databases)

**When each approach wins:**

| Workload | Best Choice | Why |
|----------|-------------|-----|
| Web server (request/response) | **temporal-slab** | High rate, correlated lifetimes, deterministic boundaries |
| Trading system (orders per microsecond) | **temporal-slab** | Latency-sensitive, correlated lifetimes |
| Game engine (per-frame objects) | **temporal-slab** | Frame boundary = epoch boundary |
| Desktop app (user interactions) | **malloc/GC** | Unpredictable lifetimes, low rate |
| Database (long-lived cache) | **malloc** | Stable working set, no churn |
| Document editor (variable buffers) | **malloc** | Large objects, no size-class fit |
| Compiler (AST with cycles) | **GC** | Complex ownership, safety matters |

### 7. Observability: Why malloc Can't Do This

**Where to add:** New section after "Bounded RSS Through Conservative Recycling"

**Content needed:**

**malloc's observability problem**:
- Operates at pointer level (no phase concept)
- To track "which route leaked memory" requires external tools:
  - Valgrind (20-50× slowdown)
  - jemalloc profiling (10-30% overhead)
  - tcmalloc heap profiler (5-15% overhead)

**Why profiling is expensive**:
1. Sample allocations (hash table per pointer)
2. Capture backtraces (stack unwinding: 200-500ns)
3. Reconstruct phase attribution post-hoc

**temporal-slab's structural observability**:
- Epochs are first-class (tracked for correctness, not profiling)
- Counters exist for lifecycle management (refcount, state, era)
- Exposing via slab_stats_*() APIs = zero added cost

**Example comparison**:
```
Question: "Which HTTP route consumed 40MB?"

With jemalloc:
1. Enable profiling (export MALLOC_CONF="prof:true")
2. Run with 10-30% overhead
3. Generate heap profile after shutdown
4. Parse jeprof output, correlate backtraces to routes manually

With temporal-slab:
1. Call slab_stats_epoch(allocator, size_class, route_epoch, &stats)
2. Read stats.estimated_rss_bytes
3. Zero overhead (counters already exist)
4. Real-time (check during request, not post-mortem)
```

### 8. Platform Differences (Linux vs BSD vs Windows)

**Where to add:** Expand "Platform-Specific Considerations: x86-64 Linux" section

**Content needed:**

**madvise semantics differ**:
- Linux MADV_DONTNEED: Immediate page reclamation (RSS drops)
- BSD MADV_FREE: Lazy reclamation (RSS drops only under pressure)
- Windows VirtualFree: Different API, requires careful alignment

**Memory ordering differences**:
- x86-64 TSO (Total Store Order): acquire/release are free
- ARM weak ordering: requires explicit fences (2× overhead)
- RISC-V weak ordering: similar to ARM

**Impact on temporal-slab**:
- RSS reclamation guarantees weaker on BSD/Windows
- ARM port would need fences in hot path (slower)
- This is why whitepaper specifies "x86-64 Linux optimized"

---

## Summary of Additions Needed

1. **Memory management taxonomy** (manual, GC, refcounting, region, epoch)
2. **malloc variants deep dive** (ptmalloc2, jemalloc, tcmalloc comparison)
3. **GC mechanics** (mark-sweep, generational, concurrent)
4. **RCU comparison** (grace periods vs passive reclamation)
5. **Region systems comparison** (Cyclone, APR pools, arenas)
6. **Decision matrix** (when to use what, anti-patterns)
7. **Observability comparison** (profiling overhead vs structural)
8. **Platform differences** (Linux/BSD/Windows, x86/ARM)

These additions would help readers:
- Understand the full design space
- Make informed decisions about which allocator to use
- Appreciate temporal-slab's unique position (fills gap between malloc and GC)
- Understand tradeoffs explicitly
