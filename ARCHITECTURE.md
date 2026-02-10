# temporal-slab Architecture

## Structural Determinism: A Third Way

Most systems force a trade-off:
- **Manual memory (malloc/free):** fast but fragile
- **Automatic memory (GC):** safe but unpredictable

temporal-slab introduces a third model: **Structural Determinism**.

### You Don't Manage Objects—You Manage Lifetimes

Memory is reclaimed when a **structure ends** (request, frame, batch, transaction), not when a pointer happens to be freed.

Epoch Domains make these lifetimes explicit, scoped, and deterministic.

**Key insight:** malloc and GC operate at the **object level**. temporal-slab operates at the **phase/structure level**.

This isn't just optimization—it's a different computational model where memory follows program structure rather than pointer chasing.

---

## Why This Exists

Traditional allocators optimize for spatial reuse and hole-finding. temporal-slab optimizes for **temporal coherence**: grouping objects by lifetime so memory is reclaimed in whole slabs instead of fragments.

This document describes the allocator's design and its role as a foundation for higher-level systems.

## Current Scope

temporal-slab is a **lifetime-aware memory allocator** for fixed-size objects. It provides:

- Lock-free allocation fast path (120ns p99, 340ns p999 - GitHub Actions validated)
- Bounded RSS under sustained churn (0-2.4% growth vs unbounded malloc drift)
- Passive epoch reclamation (no quiescence requirements, async state observation)
- Conservative deferred recycling (zero overhead in alloc/free paths)
- Structural observability (phase-level metrics emerge from design)
- Dual APIs (handle-based and malloc-style)
- Epoch-scoped RSS reclamation (madvise + epoch_close)
- Epoch domains (RAII-style scoped lifetimes)
- Comprehensive observability (stats APIs, telemetry)
- 16-epoch ring buffer for temporal grouping

This document describes the current implementation and the path to extended functionality.

## Non-Goals

temporal-slab intentionally does not attempt:
- Variable-sized allocation (use jemalloc/tcmalloc)
- Fine-grained object migration between tiers
- General-purpose malloc replacement
- Predicting object lifetimes heuristically

Lifetime alignment emerges naturally from allocation patterns, not policy.

## Current Use Cases

temporal-slab is well-suited for:
- **High-frequency trading (HFT)** - Sub-100ns deterministic allocation, no jitter
- **Control planes** - Session stores, connection metadata, bounded RSS
- **Request processors** - Web servers, RPC frameworks (request-scoped allocation via domains)
- **Frame-based systems** - Game engines, simulations (frame-scoped allocation)
- **Cache metadata** - Bounded RSS under continuous eviction
- **Real-time systems** - Packet buffers, message queues (predictable tail latency)

These workloads benefit from bounded RSS (0-2.4% growth) and predictable latency (12-13× better tail latency than malloc, GitHub Actions validated) under sustained churn.

## Core Allocator Design

### Memory Model

```
SlabAllocator (8 size classes: 64-768 bytes)
├── Per-size-class state:
│   ├── current_partial (atomic ptr) → Lock-free fast path
│   ├── epochs[16] → Per-epoch partial/full lists
│   │   ├── partial_list → Slabs with free slots
│   │   ├── full_list → Slabs with no free slots (recyclable)
│   │   └── empty_partial_count (atomic) → O(1) reclaimable tracking
│   ├── slab_cache[32] → Empty slabs ready for reuse (CachedSlab entries)
│   └── cache_overflow → Linked list of empty slabs (CachedNode entries)
├── Slab registry (SlabRegistry):
│   ├── metas[4M] → (slab*, generation) pairs (off-page, survives madvise)
│   └── ABA protection via 24-bit generation counter
└── Epoch state:
    ├── epoch_state[16] → ACTIVE or CLOSING
    ├── epoch_era[16] → Monotonic era for observability
    └── epoch_meta[16] → open_since_ns, alloc_count, label, RSS deltas
```

**Key invariants:**

1. **Slabs never unmapped during runtime** - Enables safe stale handle validation, no use-after-free from munmap races
2. **Conservative deferred recycling** - Empty slabs recycled only during epoch_close() (scans both partial and full lists)
3. **Off-page metadata** - (slab*, slab_id) stored in CachedSlab/CachedNode (survives madvise)
4. **Generation-checked handles** - 24-bit generation counter prevents ABA (16M reuse budget)
5. **Bounded RSS** - Cache (32 slabs/class) + overflow (linked list) = known maximum per class
6. **Passive epoch transitions** - State changes use only atomic stores (no coordination, no quiescence)

### State Machine

```
Slab lifecycle:

  new_slab()
      ↓
  [ACTIVE, PARTIAL] ← Lock-free allocation fast path
      ↓ (last slot allocated)
  [ACTIVE, FULL] ← Never published, safe to recycle
      ↓ (first slot freed)
  [ACTIVE, PARTIAL]
      ↓ (all slots freed on FULL list)
  [CACHED/OVERFLOWED, NONE] ← Pushed to cache or overflow
      ↓ (madvise MADV_DONTNEED if epoch CLOSING)
  [CACHED, MADVISED] ← Physical pages returned to kernel
      ↓ (cache_pop)
  [ACTIVE, PARTIAL] ← Reused (kernel zero-fills on access)
```

**Epoch-based reclamation:**

```
Epoch states:

  [ACTIVE] ← Current epoch receiving allocations
      ↓ (epoch_advance)
  [ACTIVE] ← Previous epoch drains naturally
      ↓ (epoch_close)
  [CLOSING] ← Aggressive reclamation enabled
      ↓ (empty slabs madvised immediately)
  [ACTIVE] ← Epoch ID reused (ring wraps at 16)
```

**Critical safety property:** Recycling is deferred to epoch_close() rather than happening in free_obj(). During epoch_close(), empty slabs are collected from both partial and full lists, then recycled in bulk. This eliminates recycling overhead from the allocation/free hot path while maintaining deterministic reclamation timing.

### Handle Encoding

**Current format (v1):**

```
64-bit opaque handle:
  [63:42] slab_id (22 bits, 4M slabs)
  [41:18] generation (24 bits, 16M reuses before wrap)
  [17:10] slot (8 bits, max 256 objects/slab)
  [9:2]   size_class (8 bits)
  [1:0]   version (2 bits, v1 = 0b01)
```

**Why generation + version fields:**
- **Generation (24-bit):** ABA protection for slab reuse (16M budget vs old 16-bit 65K)
- **Version (2-bit):** Future ABI evolution (v2 can change encoding)
- **slab_id instead of pointer:** Enables safe madvise (pointer invalidation-proof)

**CachedNode architecture:**

```c
typedef struct cached_node {
    slab_t *slab;        // Virtual address (survives madvise)
    slab_id_t slab_id;   // Stored off-page, safe from madvise zeroing
} cached_node_t;
```

**Critical invariant:** When slab is madvised, its `(slab*, slab_id)` pair is stored in CachedNode before `madvise()` zeros the slab header. On reuse, `slab_id` is restored from the cached entry.

This solved the **Phase 2 overflow bug** where madvise corrupted slab_id fields, causing 1% → 98.9% reclamation improvement.

## Path to Extended Functionality

The allocator is the **kernel** for higher-level systems. Here's how it extends:

### Layer 1: Hash Table Cache (separate repo: zns-cache)

```c
typedef struct CacheEntry {
    uint64_t key_hash;
    SlabHandle value_handle;  // Allocated via alloc_obj()
    uint64_t access_count;
    uint64_t last_access_ns;
} CacheEntry;

typedef struct HashTable {
    CacheEntry** buckets;      // Allocated via slab_malloc()
    SlabAllocator* allocator;  // Uses this allocator
    size_t bucket_count;
} HashTable;
```

**Why this works:**
- CacheEntry metadata allocated via `slab_malloc()` (drop-in)
- Value data allocated via `alloc_obj()` (explicit handles)
- Eviction policy operates on slabs, not individual objects
- Hash table gets bounded RSS for free

### Layer 2: Tiered Storage (separate repo: zns-tiered)

```c
typedef struct TieredAllocator {
    SlabAllocator* l0_dram;     // Hot tier (anonymous mmap)
    SlabAllocator* l1_pmem;     // Warm tier (file-backed mmap)
    SlabAllocator* l2_nvme;     // Cold tier (file-backed mmap)
    
    int pmem_fd;
    int nvme_fd;
} TieredAllocator;

// Promote entire slab between tiers
void promote_slab(TieredAllocator* ta, Slab* slab, int from_tier, int to_tier) {
    // 1. Copy 4KB slab from source tier to dest tier
    // 2. Update handles (or use indirection table)
    // 3. Recycle source slab via cache_push()
}
```

**Why slabs are the right unit:**
- 4KB is efficient for PMEM/NVMe I/O
- Entire slab moves atomically (no partial state)
- Aligned lifetimes mean hot/cold slabs are naturally separable
- FULL slabs can move without coordination (not published)

**Critical constraint:** No code in this repo (temporal-slab) assumes tiers exist. Tiering is an optional higher-level orchestration layer.

## RSS Reclamation Architecture

### Phase 1: madvise on Empty Slabs (IMPLEMENTED)

**Mechanism:**
- Empty slabs in CLOSING epochs call `madvise(MADV_DONTNEED)` before cache push
- Virtual memory stays mapped (safe for stale handles)
- Kernel zero-fills pages on next access

**Effectiveness:**
- Limited alone - slabs immediately recycled in high-churn workloads
- Requires deterministic empty periods (Phase 2 provides this)

### Phase 2: epoch_close() + Aggressive Reclamation (IMPLEMENTED)

**API:**
```c
void epoch_close(SlabAllocator* alloc, EpochId epoch);
```

**Semantics:**
- Marks epoch as CLOSING (aggressive reclamation enabled)
- Scans for already-empty slabs and madvises immediately
- Future empty slabs from this epoch madvised on cache push

**Critical bug fix:** CachedNode architecture
- Problem: madvise zeroed slab headers, corrupting slab_id field
- Solution: Store (slab*, slab_id) off-page in cached_node_t
- Result: 1% \u2192 98.9% reclamation improvement in isolated tests

**Benchmark results (epoch_rss_bench):**
- 5 epochs × 50,000 objects (128 bytes)
- 19.15 MiB marked reclaimable via madvise
- 3.3% RSS drop under memory pressure (kernel-dependent)
- 100% cache hit rate (perfect slab reuse)

**Pattern:** Small epochs approach 100% reclaim, large epochs ~2%

**Use cases:**
```c
// Web server: per-request epoch
epoch_id_t req = epoch_open();
// ... handle request ...
epoch_close(alloc, req);  // All request memory reclaimable

// Game engine: per-frame epoch
epoch_id_t frame = epoch_open();
// ... render frame ...
epoch_close(alloc, frame);  // All frame memory reclaimable
```

### Phase 3: Slab Registry (IMPLEMENTED)

**Architecture:**
```
handle → slab_id → registry.metas[slab_id] → (slab*, generation) → slab
```

**Implementation:**
```c
struct SlabMeta {
    _Atomic(Slab*) ptr;        // NULL if unmapped/unused
    _Atomic uint32_t gen;      // Generation counter (survives madvise)
};

struct SlabRegistry {
    SlabMeta* metas;           // Array of metadata entries
    uint32_t cap;              // Current capacity (grows dynamically)
    uint32_t* free_ids;        // Free ID pool
    pthread_mutex_t lock;      // Protects registry growth
};
```

**What it enables:**
- ABA protection via generation counter (24-bit, 16M reuse budget)
- Portable handles (no raw pointers, works across all platforms)
- Safe madvise (generation stored off-page, survives header zeroing)
- Handle validation without touching slab memory (check registry first)

**Status:** Production-ready. Phase 4 (actual munmap) deferred until Phase 3 proven.

### Phase 4: Handle Indirection + munmap() (NOT IMPLEMENTED)

**What it would enable:**
- Real `munmap()` with crash-proof stale frees
- Unmapped slabs detected via NULL ptr in registry (no memory access)
- Strongest possible RSS guarantees (physical unmapping, not just madvise)

**Defer until:** Phase 3 registry proven in production

## Observability Architecture (IMPLEMENTED)

temporal-slab provides comprehensive telemetry for production diagnostics:

**Three-tier stats hierarchy:**

```c
// Global stats (aggregate across all classes and epochs)
SlabGlobalStats gs;
slab_stats_global(alloc, &gs);
// RSS actual vs estimated, slow-path attribution, epoch state summary

// Per-size-class stats
SlabClassStats cs;
slab_stats_class(alloc, 2, &cs);  // 128-byte class
// Cache effectiveness, madvise tracking, slab distribution

// Per-epoch stats (within a size class)
SlabEpochStats es;
slab_stats_epoch(alloc, 2, 5, &es);  // Class 2, epoch 5
// Reclaimable memory, age, refcount, RSS footprint
```

**Diagnostic queries enabled:**
- "Why did we go slow?" → slow_path_cache_miss vs slow_path_epoch_closed
- "Why isn't RSS dropping?" → madvise_calls, madvise_bytes, madvise_failures
- "Which epoch owns most memory?" → estimated_rss_bytes per epoch
- "Is there a memory leak?" → Long-lived epochs with high refcount
- "Is the kernel cooperating?" → madvise_bytes vs RSS delta (context only)

**Key design principles:**
- **Structural attribution** - Every counter has precise causality (no emergent patterns)
- **Zero fast-path cost** - Stats use relaxed atomics, no hot-path overhead
- **Versioned JSON output** - External tools can parse without library linkage
- **No background threads** - Deterministic telemetry (every increment is traceable)

**See also:** `docs/stats_dump_reference.md` (879 lines, stable contract for external tooling)

### Semantic Attribution (Phase 2.3)

**Label registry for per-phase contention tracking:**

```c
// Assign human-readable label to epoch
slab_epoch_set_label(alloc, epoch, "/api/users");

// Per-label contention automatically tracked (when ENABLE_LABEL_CONTENTION=1)
// Hot path: O(1) label_id lookup, cache-line-friendly attribution arrays
```

**Architecture:**
- 16 label slots (bounded cardinality)
- Compact label_id (0-15) for hot-path indexing
- Per-label counters: lock_contended, CAS retries (bitmap alloc/free)
- 128-byte attribution array per size class (single cache line)

**Why this matters:**
- Identifies which application phases cause allocator contention
- Zero overhead when disabled (compile-time flag)
- ~5ns overhead when enabled (TLS label_id lookup + atomic increment)
- Answers: "Does /api/users cause more bitmap contention than /api/orders?"

### Zombie Partial Repair (Robustness)

**Defense-in-depth against free_count/bitmap divergence:**

```c
// In alloc_obj_epoch slow path
while (s = es->partial.head) {
    if (s->free_count >= 2) break;  // Definitely usable
    
    // For free_count == 0 or 1, verify against bitmap
    if (bitmap_is_full(s)) {
        atomic_thread_fence(memory_order_acquire);  // Sync with concurrent updates
        if (bitmap_is_full(s)) {  // Double-check after fence
            // Zombie detected: move PARTIAL → FULL
            repair_zombie_partial(s);
            s = es->partial.head;  // Try next slab
        }
    }
}
```

**Root cause:** Race condition in concurrent alloc/free could cause free_count to diverge from bitmap state (fixed in lines 1470-1526). Repair mechanism remains as defense-in-depth.

**Impact:** Prevents infinite CAS retry loops if divergence occurs.

### Slowpath Sampling (p9999 Diagnosis)

**Tail latency outlier diagnosis for virtualized environments:**

```c
// Enable: make CFLAGS="-DENABLE_SLOWPATH_SAMPLING=1 -DSLOWPATH_THRESHOLD_NS=5000"

typedef struct {
    uint64_t wall_ns;       // Wall-clock time
    uint64_t cpu_ns;        // Thread CPU time
    uint32_t reason_flags;  // LOCK_WAIT | NEW_SLAB | ZOMBIE_REPAIR | ...
} SlowpathSample;
```

**Distinguishes allocator work from VM scheduling noise:**
- Sample 1: wall=45µs, cpu=42µs → Real contention (CPU-bound)
- Sample 2: wall=15ms, cpu=80µs → VM preemption (wall >> cpu)

**Storage:** Lock-free ring buffer (10,000 samples, overwriting oldest)  
**Overhead:** ~50-100ns per sample, only when allocation exceeds threshold

**Use case:** Debugging CI/CD p99999 spikes from WSL2/virtualization scheduling delays.

### Layer 3: Eviction Policy (Future Repos)

```c
// Uses existing stats APIs
void evict_coldest_slab(SlabAllocator* alloc, int size_class) {
    SlabClassStats cs;
    slab_stats_class(alloc, size_class, &cs);
    
    // Find FULL slab with lowest aggregate access count
    // Demote to next tier or recycle
}
```

## Why Separation Matters

**temporal-slab (this repo):**
- Pure allocator with epoch-scoped RSS reclamation
- Zero external dependencies
- Clear safety contract (no munmap during runtime)
- Wide adoption potential

**ZNS-Cache (future repo):**
- Hash table + eviction policy
- Depends on temporal-slab
- Can experiment with different eviction strategies
- Users choose if they want it

**ZNS-Tiered (future repo):**
- DRAM/PMEM/NVMe coordination
- Depends on temporal-slab
- Can target different hardware configs
- Optional for most users

## Epoch Domains: RAII-Style Lifetime Management (IMPLEMENTED)

Epoch domains provide structured, scoped memory management on top of raw epochs:

```c
// Request-scoped allocation with automatic cleanup
epoch_domain_t* request = epoch_domain_create(alloc);
epoch_domain_enter(request);
{
    void* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
    void* buffer = slab_malloc_epoch(alloc, 256, request->epoch_id);
    
    handle_request(conn);
    
    // No manual frees needed
}
epoch_domain_exit(request);  // Automatic epoch_close() if auto_close=true
epoch_domain_destroy(request);
```

**Key features:**
- **Refcount-based nesting** - Domains can be entered/exited multiple times
- **Thread-local context** - epoch_domain_current() for implicit allocation binding
- **Auto-close vs manual control** - Reusable domains for frame-based systems
- **Zero overhead** - Domain abstraction compiles down to epoch_advance/epoch_close

**Patterns enabled:**
1. Request-scoped allocation (web servers, RPC handlers)
2. Reusable frame domains (game engines, simulations)
3. Nested domains (transaction + query scopes)
4. Explicit lifetime control (batch processing with deferred cleanup)

**See also:** `docs/EPOCH_DOMAINS.md` (comprehensive pattern catalog, safety contracts, anti-patterns)

## Current Implementation Status

**Phase 1: Core allocator** - Production-ready
- Lock-free fast path (120ns p99, 340ns p999 - GitHub Actions validated)
- 8 size classes (64-768 bytes)
- Handle-based and malloc-style APIs
- Passive epoch reclamation (no quiescence requirements)
- Conservative deferred recycling (zero hot-path overhead)

**Phase 2: RSS reclamation** - Production-ready
- madvise(MADV_DONTNEED) on empty slabs
- epoch_close() API for deterministic reclamation
- Slab registry with 24-bit generation counters (ABA protection)
- CachedNode architecture (madvise-safe reuse)
- 19.15 MiB reclaimable per epoch test, 3.3% RSS drop, 100% cache hit rate

**Phase 3: Observability** - Production-ready
- Global, per-class, per-epoch stats APIs
- Slow-path attribution (cache miss vs epoch closed)
- RSS tracking (madvise calls/bytes/failures)
- stats_dump utility with versioned JSON output (879-line reference doc)
- Structural attribution (no emergent patterns)

**Phase 4: Epoch domains** - Production-ready
- RAII-style scoped lifetimes
- Automatic epoch_close() on domain exit
- Refcount tracking for nested scopes
- Thread-local context for implicit binding
- Comprehensive pattern guide (EPOCH_DOMAINS.md)

**Documentation** - Complete
- foundations.md (3,222 lines, first-principles theory)
- EPOCH_DOMAINS.md (pattern catalog, safety contracts, anti-patterns)
- VALUE_PROP.md (measured benchmark results, risk exchange)
- ADOPTION.md (market analysis, adoption barriers, 6-12 month roadmap)
- OBSERVABILITY_DESIGN.md (telemetry architecture, diagnostic patterns)
- stats_dump_reference.md (879 lines, stable JSON contract)

## Recommended Next Steps

For **this repo (temporal-slab)**:
1. Core allocator, RSS reclamation, observability, domains - all complete
2. Comprehensive documentation (5,000+ lines across 6 docs)
3. Production validation (Month 1-2 roadmap phase)
4. Multi-platform testing (ARM64, macOS, additional Linux distributions)
5. Integration examples (Redis module, Envoy filter, game server)

For **zns-cache repo (new)**:
1. Start with minimal hash table using `slab_malloc()`
2. Add LRU eviction policy
3. Add TTL support
4. Reference temporal-slab as dependency

For **zns-tiered repo (new)**:
1. DRAM/PMEM/NVMe tier coordination
2. Slab migration between tiers
3. Hardware-specific optimizations

This keeps temporal-slab focused, stable, and maximally useful as a foundation.