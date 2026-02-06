# ZNS-Slab Architecture

## Current Scope

ZNS-Slab is a **lifetime-aware memory allocator** for fixed-size objects. It provides:

- Lock-free allocation fast path
- Bounded RSS under sustained churn
- Provably safe recycling (FULL-only)
- Dual APIs (handle-based and malloc-style)

This document describes the current implementation and the path to extended functionality.

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
      ↓ (cache_pop)
  [ACTIVE, PARTIAL] ← Reused
```

**Critical safety property:** Only slabs in FULL state are recycled. PARTIAL slabs are never recycled, eliminating TOCTTOU races where threads hold stale `current_partial` pointers.

### Handle Encoding

```
64-bit opaque handle:
  [63:16] Slab pointer (48 bits)
  [15:8]  Slot index (8 bits, max 256 objects/slab)
  [7:0]   Size class (8 bits, currently 0-3)
```

This encoding is **extensible** for future tiered storage:
```
Future multi-tier encoding:
  [63:60] Tier ID (4 bits, 16 tiers)
  [59:16] Slab ID (44 bits, ~17T slabs)
  [15:8]  Slot index (8 bits)
  [7:0]   Size class (8 bits)
```

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

**ZNS-Slab (this repo):**
- Pure allocator with no policy
- Zero external dependencies
- Clear safety contract
- Wide adoption potential

**ZNS-Cache (future repo):**
- Hash table + eviction policy
- Depends on ZNS-Slab
- Can experiment with different eviction strategies
- Users choose if they want it

**ZNS-Tiered (future repo):**
- DRAM/PMEM/NVMe coordination
- Depends on ZNS-Slab
- Can target different hardware configs
- Optional for most users

## Recommended Next Steps

For **this repo (ZNS-Slab)**:
1. ✅ Keep allocator-only scope
2. ✅ Archive planning docs to docs/archive/
3. Add ARCHITECTURE.md (this file) explaining extension path
4. Consider adding one **integration example** showing how to build a simple cache on top

For **zns-cache repo (new)**:
1. Start with minimal hash table using `slab_malloc()`
2. Add LRU eviction policy
3. Add TTL support
4. Reference ZNS-Slab as dependency

This keeps ZNS-Slab focused, stable, and maximally useful as a foundation.

Sound good?