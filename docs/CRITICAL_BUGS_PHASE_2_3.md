# CRITICAL BUGS: Must Fix Before Phase 2.3

**Status:** 🚨 RED ALERT - Allocator will break after 16 epoch advances

**Discovered:** 2025-02-07  
**Severity:** CRITICAL (allocator stops working)  

---

## Bug 1: `epoch_current()` Returns Invalid IDs After 16 Advances 🚨

### Current Implementation

```c
// include/slab_alloc.h
EpochId epoch_current(SlabAllocator* alloc);

// src/slab_alloc.c:1212
EpochId epoch_current(SlabAllocator* a) {
  return atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
}

// src/slab_alloc.c:1217
void epoch_advance(SlabAllocator* a) {
  uint32_t old_epoch_raw = atomic_fetch_add_explicit(&a->current_epoch, 1, memory_order_relaxed);
  uint32_t old_epoch = old_epoch_raw % a->epoch_count;  // Ring index
  uint32_t new_epoch = (old_epoch_raw + 1) % a->epoch_count;
  // ...
}
```

### The Bug

**`a->current_epoch` is a monotonic counter** (0, 1, 2, ..., 15, 16, 17, ...)  
**But `epoch_current()` returns it directly** (no modulo)  

**After 16 advances:**
- `epoch_current()` returns 16
- `slab_malloc_epoch(alloc, size, 16)` checks `if (epoch >= a->epoch_count)` → TRUE
- **Allocation returns NULL**

**Result:** Allocator silently stops working after 16 advances.

---

### Fix Option A (Recommended): Return Ring Index

```c
EpochId epoch_current(SlabAllocator* a) {
  uint32_t raw = atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
  return raw % a->epoch_count;  // Return ring index, not monotonic
}
```

**Pros:**
- Callers get valid ring index (0-15)
- Existing code works without changes
- Simple one-line fix

**Cons:**
- `epoch_current()` loses monotonic property (wraps)
- Era tracking must be used for uniqueness

---

### Fix Option B: Store Ring Index, Keep Monotonic Separately

**Change `epoch_advance()`:**
```c
void epoch_advance(SlabAllocator* a) {
  uint32_t old_ring = atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
  uint32_t new_ring = (old_ring + 1) % a->epoch_count;
  
  atomic_store_explicit(&a->current_epoch, new_ring, memory_order_relaxed);
  
  // Mark states
  atomic_store_explicit(&a->epoch_state[old_ring], EPOCH_CLOSING, memory_order_relaxed);
  atomic_store_explicit(&a->epoch_state[new_ring], EPOCH_ACTIVE, memory_order_relaxed);
  
  // Increment era (monotonic)
  uint64_t era = atomic_fetch_add_explicit(&a->epoch_era_counter, 1, memory_order_relaxed);
  a->epoch_era[new_ring] = era + 1;
  
  // ... rest stays same ...
}
```

**`epoch_current()` stays unchanged** (returns ring index directly).

**Pros:**
- `epoch_current()` always returns valid ring index (0-15)
- Monotonic identity in `epoch_era_counter` (separate)
- Clearer semantics

**Cons:**
- More invasive change (modifies epoch_advance logic)

---

### Recommendation

**Use Option A** (one-line fix to `epoch_current()`).

**Rationale:**
- Minimal code change
- Era tracking already provides monotonic identity
- Existing tests likely already exercise wrap-around

---

## Bug 2: `alloc_count` Used for Two Different Things 🚨

### Current State

**In `EpochMetadata`:**
```c
_Atomic uint64_t alloc_count;  /* Number of live allocations */
```

**In malloc/free paths (inferred from Phase 2.3 design):**
```c
atomic_fetch_add(&epoch_meta[epoch].alloc_count, 1);   // On alloc
atomic_fetch_sub(&epoch_meta[epoch].alloc_count, 1);   // On free
```

**In domain enter/exit (Phase 2.3 plan):**
```c
slab_epoch_inc_refcount(alloc, epoch);  // Uses same alloc_count
slab_epoch_dec_refcount(alloc, epoch);  // Uses same alloc_count
```

### The Problem

**These are semantically different:**
- **Live allocation count:** Increments per `malloc()`, decrements per `free()`
  - Value: 0-10,000+ (one per object)
  - Frequency: Hot path (99% of operations)
  
- **Domain refcount:** Increments per `domain_enter()`, decrements per `domain_exit()`
  - Value: 0-10 (one per domain scope)
  - Frequency: Cold path (per-request/per-frame boundary)

**If merged:**
- `refcount=10,000` doesn't mean "10,000 domains" (it means 10,000 live objects)
- Domain refcount leak detection is impossible (signal drowned by noise)
- Grafana alert `refcount > 0 for >5 min` fires constantly (always have live objects)

---

### Fix: Split into Two Fields

```c
typedef struct EpochMetadata {
  uint64_t open_since_ns;
  
  _Atomic uint64_t live_alloc_count;  /* Live objects (incremented on alloc, decremented on free) */
  _Atomic uint64_t domain_refcount;   /* Domain scopes (incremented on enter, decremented on exit) */
  
  char label[32];
  
  uint64_t rss_before_close;
  uint64_t rss_after_close;
} EpochMetadata;
```

**APIs update:**
```c
void slab_epoch_inc_refcount(SlabAllocator* a, EpochId epoch) {
  atomic_fetch_add_explicit(&a->epoch_meta[epoch].domain_refcount, 1, memory_order_relaxed);
}

void slab_epoch_dec_refcount(SlabAllocator* a, EpochId epoch) {
  uint64_t prev = atomic_load_explicit(&a->epoch_meta[epoch].domain_refcount, memory_order_relaxed);
  if (prev > 0) {
    atomic_fetch_sub_explicit(&a->epoch_meta[epoch].domain_refcount, 1, memory_order_relaxed);
  }
}
```

**Allocator paths update:**
```c
// In slab_malloc_epoch() (if Phase 2.3 wants live tracking)
atomic_fetch_add_explicit(&a->epoch_meta[epoch].live_alloc_count, 1, memory_order_relaxed);

// In slab_free() (if Phase 2.3 wants live tracking)
atomic_fetch_sub_explicit(&a->epoch_meta[epoch].live_alloc_count, 1, memory_order_relaxed);
```

**JSON output:**
```json
{
  "epoch_id": 5,
  "live_alloc_count": 10000,  // Live objects
  "domain_refcount": 2,        // Domain scopes
  ...
}
```

---

### Decision Point

**Do you need `live_alloc_count` tracking?**

**Option A (recommended): Don't track live_alloc_count yet**
- Adds hot-path overhead (atomic increment per allocation)
- Phase 2.3's goal is domain refcount leak detection
- Live allocation count can be added later if needed

**Option B: Track live_alloc_count**
- Useful for "which epoch has most live objects?"
- But adds 1-2 cycles per allocation (hot path)

**Recommendation:** Start with `domain_refcount` only (Phase 2.3), add `live_alloc_count` in Phase 2.6 if needed.

---

## Bug 3: Domain Refcount Not Wired to Allocator 🚨

### Current Implementation

**domain enter/exit only touches local refcount:**
```c
void epoch_domain_enter(epoch_domain_t* domain) {
  domain->refcount++;  // Local to domain struct, not visible to allocator
}

void epoch_domain_exit(epoch_domain_t* domain) {
  domain->refcount--;
  if (domain->refcount == 0 && domain->auto_close) {
    epoch_close(domain->alloc, domain->epoch_id);
  }
}
```

**Allocator has no visibility:**
- `slab_epoch_get_refcount(alloc, epoch)` would return 0 (never incremented)
- Observability tools can't see domain nesting
- Grafana can't detect refcount leaks

---

### Fix: Wire Domain Enter/Exit to Allocator APIs

```c
void epoch_domain_enter(epoch_domain_t* domain) {
  if (!domain) return;
  
  if (domain->refcount == 0) {
    // First enter: notify allocator
    slab_epoch_inc_refcount(domain->alloc, domain->epoch_id);
  }
  
  domain->refcount++;  // Local nesting tracking
  
  if (domain->refcount == 1) {
    tls_current_domain = domain;
  }
}

void epoch_domain_exit(epoch_domain_t* domain) {
  if (!domain) return;
  assert(domain->refcount > 0);
  
  domain->refcount--;
  
  if (domain->refcount == 0) {
    // Last exit: notify allocator
    slab_epoch_dec_refcount(domain->alloc, domain->epoch_id);
    
    if (domain->auto_close) {
      epoch_close(domain->alloc, domain->epoch_id);
    }
    
    if (tls_current_domain == domain) {
      tls_current_domain = NULL;
    }
  }
}
```

**Result:** Allocator's `domain_refcount` now tracks global state (not just local nesting).

---

## Bug 4: `auto_close` Can Close Wrong Epoch After Wrap 🚨

### The Problem

**Scenario:**
1. Domain created with `epoch_id = 5, era = 100`
2. Domain stays open for 20 epoch advances
3. Ring wraps: `epoch_id = 5` now has `era = 120` (different epoch!)
4. Domain exits, calls `epoch_close(alloc, 5)`
5. **Closes wrong epoch** (era 120, not era 100)

**Result:** Premature reclamation of unrelated memory.

---

### Root Cause

**Domain struct only stores `epoch_id`:**
```c
typedef struct epoch_domain {
  SlabAllocator* alloc;
  EpochId epoch_id;  // Ring index (0-15), not unique!
  // Missing: epoch_era for disambiguation
} epoch_domain_t;
```

**`epoch_close()` only takes `epoch_id`:**
```c
void epoch_close(SlabAllocator* alloc, EpochId epoch);  // Ambiguous after wrap
```

---

### Fix: Add Era to Domain and Validate on Close

**Update domain struct:**
```c
typedef struct epoch_domain {
  SlabAllocator* alloc;
  EpochId epoch_id;
  uint64_t epoch_era;  // NEW: Capture era at creation
  uint32_t refcount;
  bool auto_close;
} epoch_domain_t;
```

**Capture era on creation:**
```c
epoch_domain_t* epoch_domain_create(SlabAllocator* alloc) {
  // ...
  epoch_advance(alloc);
  uint32_t raw = atomic_load_explicit(&alloc->current_epoch, memory_order_relaxed);
  domain->epoch_id = raw % alloc->epoch_count;
  domain->epoch_era = alloc->epoch_era[domain->epoch_id];  // Capture era
  // ...
}
```

**Validate era on close:**
```c
void epoch_domain_exit(epoch_domain_t* domain) {
  // ...
  if (domain->refcount == 0 && domain->auto_close) {
    // Validate era before closing
    uint64_t current_era = domain->alloc->epoch_era[domain->epoch_id];
    if (current_era == domain->epoch_era) {
      epoch_close(domain->alloc, domain->epoch_id);
    } else {
      // Era mismatch: epoch already wrapped and reused
      // Silently skip close (or log warning)
    }
  }
}
```

**Alternative: Add era parameter to epoch_close:**
```c
void epoch_close_checked(SlabAllocator* alloc, EpochId epoch, uint64_t expected_era);
```

---

## Bug 5: `epoch_domain_create()` Advances on Every Creation 🚨

### Current Code

```c
epoch_domain_t* epoch_domain_create(SlabAllocator* alloc) {
  // ...
  epoch_advance(alloc);  // Advances ring immediately
  domain->epoch_id = epoch_current(alloc);
  // ...
}
```

### The Problem

**Every domain creation advances the ring.**

**Scenario:**
```c
// Create 20 domains for parallel requests
for (int i = 0; i < 20; i++) {
  epoch_domain_t* d = epoch_domain_create(alloc);  // 20 advances!
  // ...
}
```

**Result:** Ring wraps after 16 creations, epochs collide.

---

### Fix: Separate Creation from Advance

**Option A: Don't advance in create (wrap current epoch)**
```c
epoch_domain_t* epoch_domain_create(SlabAllocator* alloc) {
  epoch_domain_t* domain = malloc(sizeof(*domain));
  domain->alloc = alloc;
  
  // Wrap current epoch (no advance)
  uint32_t raw = atomic_load_explicit(&alloc->current_epoch, memory_order_relaxed);
  domain->epoch_id = raw % alloc->epoch_count;
  domain->epoch_era = alloc->epoch_era[domain->epoch_id];
  
  domain->refcount = 0;
  domain->auto_close = false;  // Default: no auto-close (explicit epoch_close)
  
  return domain;
}
```

**Option B: Add explicit `epoch_domain_create_with_advance()`**
```c
epoch_domain_t* epoch_domain_create(SlabAllocator* alloc);  // Wraps current
epoch_domain_t* epoch_domain_create_with_advance(SlabAllocator* alloc);  // Advances first
```

**Recommendation:** Option A + make `auto_close = false` by default.

**Why:** Advancing should be **explicit** (application controls phase boundaries), not implicit (hidden in create).

---

## Design Issue: `auto_close` Semantics with Refcount

### Current Behavior

```c
if (domain->refcount == 0 && domain->auto_close) {
  epoch_close(domain->alloc, domain->epoch_id);
}
```

### The Confusion

**Per-domain refcount ≠ Global epoch refcount**

**Scenario:**
1. Domain A: `epoch_id = 5, refcount = 0` (about to exit)
2. Domain B: `epoch_id = 5, refcount = 2` (still active)
3. Domain A exits → `auto_close` triggers → `epoch_close(5)`
4. **Domain B's epoch just got closed prematurely!**

**Result:** Domain B's allocations now in CLOSING epoch (new allocations fail).

---

### Fix: Check Global Refcount Before Auto-Close

```c
void epoch_domain_exit(epoch_domain_t* domain) {
  // ...
  if (domain->refcount == 0) {
    slab_epoch_dec_refcount(domain->alloc, domain->epoch_id);
    
    if (domain->auto_close) {
      // Check global refcount before closing
      uint64_t global_refcount = slab_epoch_get_refcount(domain->alloc, domain->epoch_id);
      if (global_refcount == 0) {
        // Validate era
        uint64_t current_era = domain->alloc->epoch_era[domain->epoch_id];
        if (current_era == domain->epoch_era) {
          epoch_close(domain->alloc, domain->epoch_id);
        }
      }
    }
  }
}
```

**Result:** Only close when **global** refcount reaches zero (no other domains using epoch).

---

## Summary: 5 Critical Bugs to Fix Before Phase 2.3

| Bug | Impact | Severity | Fix Complexity |
|-----|--------|----------|----------------|
| **1. epoch_current() overflow** | Allocator stops after 16 advances | 🚨 CRITICAL | Simple (1 line) |
| **2. alloc_count semantic collision** | Refcount leak detection impossible | ⚠️ HIGH | Medium (split fields) |
| **3. Domain refcount not wired** | Observability sees refcount=0 always | ⚠️ HIGH | Medium (wire enter/exit) |
| **4. auto_close era mismatch** | Closes wrong epoch after wrap | 🚨 CRITICAL | Medium (validate era) |
| **5. advance-on-create** | Ring wraps with 16+ domains | ⚠️ MEDIUM | Simple (remove advance) |

---

## Recommended Fix Order

### Immediate (Blocking)

1. **Fix `epoch_current()`** to return ring index (1 line)
2. **Remove `epoch_advance()` from `epoch_domain_create()`** (1 line)
3. **Change default `auto_close = false`** (explicit close safer)

**Result:** Allocator works reliably, no silent failures.

---

### Before Phase 2.3 Implementation

4. **Split `alloc_count` into `live_alloc_count` + `domain_refcount`**
5. **Wire domain enter/exit to allocator refcount APIs**
6. **Add `epoch_era` to domain struct + validate on close**

**Result:** Phase 2.3 observability semantics are correct.

---

## Testing After Fixes

```c
// Test 1: epoch_current() returns valid ring index after 20 advances
for (int i = 0; i < 20; i++) {
  epoch_advance(alloc);
}
EpochId e = epoch_current(alloc);
assert(e < 16);  // Must be ring index, not monotonic

// Test 2: Multiple domains can share same epoch
epoch_domain_t* d1 = epoch_domain_create(alloc);  // No advance
epoch_domain_t* d2 = epoch_domain_create(alloc);  // Same epoch
assert(d1->epoch_id == d2->epoch_id);  // Both wrap current

// Test 3: Global refcount tracks all domains
epoch_domain_enter(d1);
epoch_domain_enter(d2);
assert(slab_epoch_get_refcount(alloc, d1->epoch_id) == 2);

epoch_domain_exit(d1);
assert(slab_epoch_get_refcount(alloc, d1->epoch_id) == 1);  // d2 still active

epoch_domain_exit(d2);
assert(slab_epoch_get_refcount(alloc, d1->epoch_id) == 0);  // All exited
```

---

## Impact on Phase 2.3 Plan

**Changes required:**

1. ✅ Fix `epoch_current()` first (blocking)
2. ✅ Split `alloc_count` → `live_alloc_count` + `domain_refcount`
3. ✅ Add `epoch_era` to `epoch_domain_t` struct
4. ✅ Validate era in `auto_close` path
5. ✅ Wire domain enter/exit to allocator APIs

**Once fixed:**
- Phase 2.3 implementation proceeds as planned
- JSON output uses `domain_refcount` (not `live_alloc_count`)
- Grafana alerts work correctly (`refcount > 0` means domain leak, not live objects)

---

**Priority:** Fix bugs 1, 2, 4, 5 **immediately** (before Phase 2.3).  
**Status:** Implementation ready (exact patches above).  
**Testing:** Extended test cases provided.

---

**Maintainer:** blackwd  
**Supersedes:** PHASE_2_3_CORRECTED_PLAN.md (must fix bugs first)
