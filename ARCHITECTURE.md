# temporal-slab Architecture

## Why This Exists

Traditional allocators optimize for spatial reuse and hole-finding. temporal-slab optimizes for **temporal coherence**: grouping objects by lifetime so memory is reclaimed in whole slabs instead of fragments.

This document describes the allocator's design and its role as a foundation for higher-level systems.

## Current Scope

temporal-slab is a **lifetime-aware memory allocator** for fixed-size objects. It provides:

- Lock-free allocation fast path
- Bounded RSS under sustained churn
- Provably safe recycling (FULL-only)
- Dual APIs (handle-based and malloc-style)
- Epoch-scoped RSS reclamation (madvise + epoch_close)
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
- Session stores
- Cache metadata
- Connection tracking
- Deduplication tables
- High-churn object registries

These workloads benefit from bounded RSS and predictable latency under sustained churn.

## Core Allocator Design

### Memory Model

```
SlabAllocator (per size class)
├── current_partial (atomic ptr) → Lock-free fast path
├── partial_list → Slabs with free slots
├── full_list → Slabs with no free slots (recyclable)
├── slab_cache[32] → Empty slabs ready for reuse
└── cache_overflow → Unbounded empty slab tracking
```

**Key invariant:** Slabs never unmapped during runtime. This enables:
- Safe stale handle validation
- Bounded RSS (cache + overflow = known maximum)
- No use-after-free from munmap races

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

**Critical safety property:** Only slabs in FULL state are recycled. PARTIAL slabs are never recycled, eliminating TOCTTOU races where threads hold stale `current_partial` pointers.

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

### Phase 3: Handle Indirection + munmap() (NOT IMPLEMENTED)

**Architecture:**
```
handle → slab_id → slab_table[generation, state, ptr] → slab
```

**What it enables:**
- Real `munmap()` with crash-proof stale frees
- Unmapped slabs detected safely without touching memory
- Strongest possible RSS guarantees

**Defer until:** Phases 1-2 proven in production

### Layer 3: Eviction Policy

```c
// Uses existing perf counters
void evict_coldest_slab(SlabAllocator* alloc, int size_class) {
    PerfCounters pc;
    get_perf_counters(alloc, size_class, &pc);
    
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

## Recommended Next Steps

For **this repo (temporal-slab)**:
1. ✅ Keep allocator-only scope
2. ✅ Archive planning docs to docs/archive/
3. Add ARCHITECTURE.md (this file) explaining extension path
4. Consider adding one **integration example** showing how to build a simple cache on top

For **zns-cache repo (new)**:
1. Start with minimal hash table using `slab_malloc()`
2. Add LRU eviction policy
3. Add TTL support
4. Reference temporal-slab as dependency

This keeps temporal-slab focused, stable, and maximally useful as a foundation.

Sound good?