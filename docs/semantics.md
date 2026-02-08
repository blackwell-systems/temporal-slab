# Epoch and Domain Semantics: The Contract

**Purpose:** This document defines the canonical invariants for epochs and domains in temporal-slab. All other documentation derives from these definitions.

**Last updated:** 2026-02-07  
**Status:** Authoritative reference for epoch/domain behavior

---

## Executive Summary

**Epochs** group allocations by temporal phase (0-15 ring buffer, reusable after wrap, era-disambiguated).  
**Domains** provide RAII-style scoped lifetimes on top of epochs (thread-local, nestable via TLS stack).  
**State gating:** `EPOCH_CLOSING` state rejects new allocations (fast atomic check in allocation path).  
**Reclamation:** `epoch_close()` drains and reclaims empty slabs (assumes epoch already CLOSING).

---

## Epoch Semantics

### Definition

An **epoch** is a ring slot (0-15) used for allocation grouping. Epochs are reusable after ring wraparound, but each incarnation is distinguished by a monotonic **era** counter.

### Ring Buffer Model

```
Epochs: [0] [1] [2] ... [14] [15] → wraps to [0]
Eras:    100  101  102      114  115     116 (monotonic, never wraps)
```

**Key insight:** `epoch_id` alone is ambiguous after wraparound. `(epoch_id, era)` pair is unique.

### Epoch Lifecycle States

```c
typedef enum {
    EPOCH_ACTIVE,   /* Accepts new allocations */
    EPOCH_CLOSING   /* Rejects new allocations, draining in progress */
} EpochLifecycleState;
```

**State transitions:**
- `epoch_advance()` → Previous epoch becomes CLOSING, next epoch becomes ACTIVE
- `epoch_close()` → Scans CLOSING epoch for reclaimable slabs, calls madvise(MADV_DONTNEED)

**Invariants:**
1. At most 1 ACTIVE epoch per allocator at any time **(in the current design - may change if per-thread epoch lanes added)**
2. Multiple epochs may be CLOSING simultaneously (drainage is asynchronous)
3. **Allocations are rejected when epoch state is CLOSING** (fast atomic check in allocation path, not by epoch_close itself)
4. Epochs never "expire" by time - only by explicit `epoch_advance()` calls

**Allocation rejection mechanism:**
```c
// In alloc_obj_epoch() before attempting allocation:
uint32_t state = atomic_load_explicit(&alloc->epoch_state[epoch], memory_order_relaxed);
if (state == EPOCH_CLOSING) {
    return NULL;  // Reject immediately
}
```

### epoch_advance() Contract

```c
void epoch_advance(SlabAllocator* alloc);
```

**Semantics:**
1. Mark current epoch as CLOSING (atomic store with release ordering)
2. Advance current_epoch to `(prev + 1) % EPOCH_COUNT` (ring wraparound)
3. Increment epoch_era_counter (monotonic generation)
4. Stamp epoch_era[next_epoch] with new era value
5. Return (does NOT call epoch_close - caller controls reclamation timing)

**Postconditions:**
- Previous epoch is CLOSING (rejects new allocations)
- Next epoch is ACTIVE (accepts allocations)
- Era counter incremented (wraparound detection possible)

**Cost:** ~10-20ns (atomic stores, no syscalls)

### epoch_close() Contract

```c
void epoch_close(SlabAllocator* alloc, EpochId epoch);
```

**Preconditions:**
- Epoch state MUST be CLOSING (allocation rejection already active)
- Caller controls timing (not automatic unless domain auto_close=true)

**Semantics:**
1. **"Drain and reclaim empties"** - Scan PARTIAL lists for empty slabs (free_count == object_count)
2. **"Return physical pages to kernel"** - Call madvise(MADV_DONTNEED) on empty slabs
3. **"Recycle slabs for reuse"** - Push empty slabs to per-class cache (LIFO)

**Postconditions:**
- Empty slabs in this epoch are recycled (virtual memory retained, physical memory released)
- Cache may overflow (bounded by cache_capacity + overflow list)
- RSS may drop (kernel cooperation not guaranteed)

**Important:** epoch_close() does NOT reject allocations itself - that's the job of EPOCH_CLOSING state check in the allocation path. epoch_close() assumes rejection is already happening and focuses on reclamation.

**Cost:** O(slabs in epoch) scan, O(empty slabs) madvise syscalls (~1-10µs per empty slab)

**Thread safety:** Can be called concurrently with allocations/frees. Allocations into CLOSING epochs are rejected by state check (atomic load). Frees into CLOSING epochs are safe and may race with reclamation (slab list operations use per-class mutex).

**Concurrency guarantee:** A slab is reclaimed only while holding the owning size-class lock (`sc->lock`), and the "empty" predicate (`free_count == object_count`) is validated under that lock. This prevents races where a concurrent free could invalidate the emptiness decision.

### Era Stamping (Wraparound Safety)

**Problem:** Epoch IDs wrap at 16. Without era, can't distinguish:
- "Epoch 5 from 10 minutes ago" vs "Epoch 5 from this second"

**Solution:** Monotonic era counter

```c
struct SlabAllocator {
    _Atomic uint64_t epoch_era_counter;      /* Increments on every epoch_advance */
    _Atomic uint64_t epoch_era[EPOCH_COUNT]; /* Era when each epoch was last activated */
};

struct Slab {
    uint64_t era;  /* Era when slab was created/reused */
};
```

**Invariants:**
1. `epoch_era_counter` is monotonic (never decrements, never wraps in practice)
2. `epoch_era[e]` updated on every activation of epoch `e`
3. Slab `era` field captures `epoch_era[epoch_id]` at slab creation time
4. Era validation prevents closing wrong epoch after wraparound

**Example:**
```c
epoch_advance(alloc);  // epoch 0 → era 100
// ... 16 advances later ...
epoch_advance(alloc);  // epoch 0 → era 116 (reused, but different era)
```

Domain with `epoch_id=0, era=100` knows it's stale (current era is 116).

---

## Domain Semantics

### Definition

An **epoch domain** is an RAII-style scoped lifetime wrapper around an epoch. It provides automatic cleanup and composable nested scopes.

```c
typedef struct epoch_domain {
    SlabAllocator* alloc;      /* Allocator instance */
    EpochId epoch_id;          /* Underlying epoch (ring index 0-15) */
    uint64_t epoch_era;        /* Era captured at create/wrap time */
    uint32_t refcount;         /* Nesting depth (thread-local) */
    bool auto_close;           /* Close epoch on last exit? */
    pthread_t owner_tid;       /* Thread ownership (contract enforcement) */
} epoch_domain_t;
```

**Critical distinction:**
- `epoch_domain_create(alloc)` - **Binds to current ACTIVE epoch** (does NOT advance)
- `epoch_domain_wrap(alloc, epoch_id, auto_close)` - Binds to specific epoch ID (explicit control)

**Why create() doesn't advance:**
- Caller controls phase boundaries (explicit epoch_advance before create if needed)
- Allows multiple domains to share same epoch (different scopes, same temporal phase)
- Avoids implicit advancing that could surprise caller

### Thread-Local Contract (Strict)

**Invariants:**
1. Domain creation, enter, exit, and destroy MUST occur on the same thread
2. `owner_tid` captured on create, validated on all operations
3. Crossing thread boundaries is undefined behavior (asserts in debug, UB in release)

**Rationale:**
- Simplifies locking (no cross-thread refcount atomics)
- Eliminates contention (per-thread TLS stack, no global state)
- Fastest possible implementation (no memory barriers on refcount)

**Multi-threaded usage:**
- Each thread creates its own domain
- Threads may share underlying epoch (different domains, same epoch_id)
- Cross-thread free is safe (free_obj validates slab ownership, not domain ownership)

### Refcount-Based Nesting

**Model:** LIFO stack of domain pointers (32-element thread-local array)

```c
static __thread epoch_domain_t* tls_domain_stack[32];
static __thread uint32_t tls_domain_depth = 0;
```

**Operations:**
- `epoch_domain_enter(d)` → Push `d` onto stack, increment `d->refcount`
- `epoch_domain_exit(d)` → Assert LIFO order, pop stack, decrement `d->refcount`
- `epoch_domain_current()` → Return top of stack (innermost domain)

**Invariants:**
1. Stack depth never exceeds 32 (asserts on overflow)
2. Exits must be LIFO (asserts on out-of-order unwind)
3. Domain not present on stack when destroyed (asserts on stale reference)

**Example (Correct):**
```c
epoch_domain_enter(outer);      // depth: 0→1, refcount: 0→1
    epoch_domain_enter(inner);  // depth: 1→2, refcount: 0→1
    epoch_domain_exit(inner);   // depth: 2→1, refcount: 1→0
epoch_domain_exit(outer);       // depth: 1→0, refcount: 1→0
```

**Example (Incorrect - will assert):**
```c
epoch_domain_enter(outer);      // depth: 0→1
    epoch_domain_enter(inner);  // depth: 1→2
    epoch_domain_exit(outer);   // ASSERT FAILURE: out-of-order unwind
```

### Auto-Close Semantics

**Two lifecycle modes:**

#### 1. Explicit control (auto_close=false, DEFAULT)

```c
epoch_domain_t* frame = epoch_domain_wrap(alloc, epoch, false);

for (int i = 0; i < 100; i++) {
    epoch_domain_enter(frame);
        // frame allocations...
    epoch_domain_exit(frame);  // Just decrements refcount, no epoch_close
}

epoch_domain_force_close(frame);  // Explicit cleanup
epoch_domain_destroy(frame);
```

**When to use:**
- Reusable scopes across iterations (game frames, batches)
- Delayed cleanup (accumulate across cycles, reclaim later)
- Performance-critical loops (avoid epoch_close overhead per iteration)

#### 2. Automatic cleanup (auto_close=true)

```c
epoch_domain_t* request = epoch_domain_wrap(alloc, epoch, true);

epoch_domain_enter(request);
    // request allocations...
epoch_domain_exit(request);  // Calls epoch_close if refcount→0 and era matches

epoch_domain_destroy(request);
```

**When to use:**
- Request-scoped allocation (web servers, RPC handlers)
- One-shot scopes (no reuse expected)
- **Single-thread or isolated epoch usage** (auto_close is less useful in multi-thread scenarios)
- "Fire and forget" cleanup (don't want to track epoch_close manually)

**Safety checks (auto_close=true):**
1. Era validation: `current_era == domain->epoch_era` (prevent closing wrong epoch after wrap)
2. Global refcount check: `slab_epoch_get_refcount(epoch_id) == 0` (no other active domains)
3. If either fails, skip auto-close (safe, won't close wrong epoch or drain active allocations)

**Multi-thread caveat:** If multiple threads share the same epoch (different domain objects, same epoch_id), the global refcount will rarely reach 0, making auto_close ineffective. In multi-thread scenarios, prefer explicit `epoch_close()` policies coordinated across threads.

**epoch_domain_destroy() behavior:** Frees only the domain object itself. It never closes epochs automatically except when `auto_close==true` AND `refcount==0` AND `era` still matches (same checks as exit). For explicit cleanup, use `epoch_domain_force_close()` before destroy.

### Domain Refcount vs Allocator Refcount

**Two separate refcount mechanisms:**

#### 1. Domain-local refcount (domain->refcount)
- **Purpose:** Track nesting depth for this specific domain object
- **Scope:** Thread-local, no atomics needed
- **Updated by:** epoch_domain_enter/exit (increments/decrements per call)
- **Used for:** Determining when to call auto-close (refcount→0 means last exit)

#### 2. Allocator epoch refcount (alloc->epoch_metadata[e].domain_refcount)
- **Purpose:** Track total active domains referencing this epoch across all threads
- **Scope:** Per-epoch global state (atomic updates)
- **Updated by:** 0→1 transition on enter, 1→0 transition on exit (only boundary transitions)
- **Used for:** Leak detection (age_sec>300 && refcount>0 = domain leak)

**Relationship:**
```c
// On first enter (domain local refcount 0→1)
if (domain->refcount == 0) {
    slab_epoch_inc_refcount(alloc, epoch_id);  // Global refcount ++
}
domain->refcount++;  // Local refcount ++

// On last exit (domain local refcount 1→0)
domain->refcount--;  // Local refcount --
if (domain->refcount == 0) {
    slab_epoch_dec_refcount(alloc, epoch_id);  // Global refcount --
}
```

**Why two refcounts?**
- Domain refcount: Fast thread-local nesting tracking (no contention)
- Epoch refcount: Global observability signal (how many domains still active?)

---

## Nested Domains (Issue #1 - RESOLVED)

### Problem

**Original issue:** Single TLS pointer clobbered nested domains.

```c
// BROKEN (before fix):
epoch_domain_enter(A);      // TLS = A
    epoch_domain_enter(B);  // TLS = B (A lost!)
    epoch_domain_exit(B);   // TLS = NULL
epoch_domain_exit(A);       // CRASH: A not in TLS
```

### Solution: TLS Stack (32-element LIFO)

**Implementation:** Thread-local stack of domain pointers with LIFO enforcement.

```c
static __thread epoch_domain_t* tls_domain_stack[32];
static __thread uint32_t tls_domain_depth = 0;
```

**Operations (src/epoch_domain.c:23-32):**
- `tls_push(d)` → Assert depth < 32, push onto stack
- `tls_pop_expected(d)` → Assert LIFO order, pop from stack
- `tls_top()` → Return top of stack (innermost domain)

**Correctness guarantees:**
1. Nesting depth limit: 32 levels (asserts on overflow)
2. LIFO unwinding enforced (asserts on out-of-order exit)
3. Debug mode validates stack consistency on destroy

### Nested Domain Patterns

#### Pattern 1: Transaction + Query

```c
void run_transaction(SlabAllocator* alloc) {
    epoch_domain_t* txn = epoch_domain_create(alloc);
    epoch_domain_enter(txn);
    {
        void* txn_log = slab_malloc_epoch(alloc, 512, txn->epoch_id);
        
        // Nested query scope
        epoch_domain_t* query = epoch_domain_create(alloc);
        epoch_domain_enter(query);
        {
            void* result_set = slab_malloc_epoch(alloc, 256, query->epoch_id);
            run_sql_query(result_set);
        }
        epoch_domain_exit(query);
        epoch_domain_destroy(query);  // Query domain scope ends (allocations reclaimed when epoch closes)
        
        commit_transaction(txn_log);
    }
    epoch_domain_exit(txn);
    epoch_domain_destroy(txn);  // Transaction domain scope ends (allocations reclaimed when epoch closes)
}
```

**Key insight:** Query results don't outlive query execution (early reclamation), transaction log survives across queries (different lifetimes).

#### Pattern 2: Request + Background Task

```c
void handle_request(Request* req) {
    epoch_domain_t* request = epoch_domain_create(alloc);
    epoch_domain_enter(request);
    {
        void* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
        
        // Nested background task
        epoch_domain_t* task = epoch_domain_create(alloc);
        epoch_domain_enter(task);
        {
            void* work_buffer = slab_malloc_epoch(alloc, 384, task->epoch_id);
            process_async(work_buffer);
        }
        epoch_domain_exit(task);
        epoch_domain_destroy(task);  // Task domain scope ends (allocations reclaimed when epoch closes)
        
        send_response(session);
    }
    epoch_domain_exit(request);
    epoch_domain_destroy(request);  // Request domain scope ends (allocations reclaimed when epoch closes)
}
```

#### Pattern 3: Reentrant Domains

```c
epoch_domain_t* frame = epoch_domain_create(alloc);

for (int i = 0; i < 100; i++) {
    epoch_domain_enter(frame);      // refcount: 0→1
        epoch_domain_enter(frame);  // refcount: 1→2 (reentrant!)
            // nested work...
        epoch_domain_exit(frame);   // refcount: 2→1
    epoch_domain_exit(frame);       // refcount: 1→0
}
```

**Valid usage:** Same domain can be entered multiple times (refcount tracks depth).

---

## Shareable vs Thread-Local Epochs

### Question: Can epochs be shared across threads?

**Answer:** Yes (with caveats).

**Epochs are multi-thread safe:**
- Allocation fast path is lock-free (atomic current_partial, CAS bitmap)
- epoch_advance() uses atomic stores (any thread can call)
- epoch_close() uses per-class locks (safe from multiple threads)

**Domains are strictly thread-local:**
- enter/exit must occur on owner thread (pthread_t validated)
- TLS stack is per-thread (no cross-thread visibility)

### Cross-Thread Allocation Pattern

```c
// Thread A
epoch_domain_t* domain_A = epoch_domain_create(alloc);
epoch_domain_enter(domain_A);
void* p1 = slab_malloc_epoch(alloc, 128, domain_A->epoch_id);
epoch_domain_exit(domain_A);

// Thread B (same allocator, different domain)
epoch_domain_t* domain_B = epoch_domain_create(alloc);
epoch_domain_enter(domain_B);
void* p2 = slab_malloc_epoch(alloc, 128, domain_B->epoch_id);
epoch_domain_exit(domain_B);

// Both domains may reference same epoch_id (shared epoch)
// But domain objects themselves are thread-local
```

**Cross-thread free (EXPLICITLY SAFE):**
```c
// Thread A allocates
void* p = slab_malloc_epoch(alloc, 128, epoch);

// Thread B frees (SAFE - no thread ownership check)
slab_free(alloc, p);  // Validates slab ID, generation, magic (not pthread_t)
```

**What makes cross-thread free safe:**
1. **Handle validation** - free_obj() validates slab_id + 24-bit generation via registry (ABA-proof against 16M reuses, implemented today)
2. **Lock-free bitmap** - Slot freeing uses atomic CAS (safe from any thread)
3. **Per-class mutex** - List operations (full→partial transitions) use size-class lock (not per-thread)
4. **Slab metadata lifetime** - Slabs remain mapped for allocator lifetime (no munmap during runtime; Phase 3 will add handle indirection to enable safe munmap)

**What about cross-thread free during epoch_close?**
- **Safe:** epoch_close() scans for empty slabs while holding per-class lock
- **Race possible:** Free may race with reclamation scan (both hold same lock, serialized)
- **Outcome:** Either slab freed before scan (reclaimed), or freed after scan (stays until next close)

**Rationale:**
- Epochs are allocator-level constructs (multi-thread shared state)
- Domains are thread-level abstractions (thread-local RAII)
- Allocations have no thread affinity (only slab ownership)

---

## Forbidden Patterns (Anti-Patterns)

### 1. Cross-Thread Domain Usage

```c
// FORBIDDEN: Domain used from non-owner thread
epoch_domain_t* domain = epoch_domain_create(alloc);  // Created on Thread A

pthread_create(&thread_B, NULL, [](void* arg) {
    epoch_domain_t* d = (epoch_domain_t*)arg;
    epoch_domain_enter(d);  // ASSERT FAILURE: owner_tid mismatch
}, domain);
```

**Fix:** Create separate domain per thread.

### 2. Nested Domains Across Different Allocators (Footgun Warning)

```c
// DISCOURAGED: TLS tracks top domain only (any allocator)
SlabAllocator* alloc_a = slab_allocator_create();
SlabAllocator* alloc_b = slab_allocator_create();

epoch_domain_t* domain_a = epoch_domain_create(alloc_a);
epoch_domain_t* domain_b = epoch_domain_create(alloc_b);

epoch_domain_enter(domain_a);  // TLS = domain_a (alloc_a)
    epoch_domain_enter(domain_b);  // TLS = domain_b (alloc_b)
        SlabAllocator* alloc = epoch_domain_allocator();  // Returns alloc_b
        // Confusion: which allocator to use?
```

**Not a correctness violation** (domains are independent), but a **footgun** if you rely on epoch_domain_allocator() as implicit context.

**Solutions:**
1. **Recommended:** Nest domains within same allocator (avoids confusion)
2. **Advanced:** Pass allocator explicitly, never use epoch_domain_allocator() with mixed allocators
3. **Strict enforcement (future):** Add `tls_current_allocator` and assert all nested domains share same allocator

### 3. Out-of-Order Unwinding

```c
// FORBIDDEN: Non-LIFO exit order
epoch_domain_enter(outer);
    epoch_domain_enter(inner);
    epoch_domain_exit(outer);  // ASSERT FAILURE: out-of-order unwind
```

**Fix:** Exit in reverse order (LIFO).

### 4. Forgetting to Exit Domain

```c
// FORBIDDEN: Early return leaks refcount
void handle_request() {
    epoch_domain_t* request = epoch_domain_create(alloc);
    epoch_domain_enter(request);
    
    if (error) {
        return;  // BUG: forgot epoch_domain_exit
    }
    
    epoch_domain_exit(request);
}
```

**Fix:** Use goto cleanup or ensure all paths exit.

### 5. Reusing Auto-Close Domain Across Iterations

```c
// FORBIDDEN: auto_close=true calls epoch_close on every exit
epoch_domain_t* frame = epoch_domain_create(alloc);  // auto_close=true (default)

for (int i = 0; i < 100; i++) {
    epoch_domain_enter(frame);
        // allocations...
    epoch_domain_exit(frame);  // Calls epoch_close every iteration!
}
```

**Fix:** Use `epoch_domain_wrap(alloc, epoch, false)` for reusable domains.

### 6. Mixing Domain and Raw Epoch APIs (Context-Dependent)

**Discouraged pattern:**
```c
// PROBLEMATIC: Advancing allocator while domain expects epoch to stay active
epoch_domain_t* domain = epoch_domain_create(alloc);
epoch_domain_enter(domain);
{
    void* p = slab_malloc_epoch(alloc, 128, domain->epoch_id);
    epoch_advance(alloc);  // Domain's epoch is now CLOSING
    // Allocations using domain->epoch_id will now fail
}
```

**Why problematic:** Domain represents "the current phase" but caller advanced to next phase.

**Allowed patterns:**
```c
// OK: Explicit epoch close on different epoch (not domain's epoch)
epoch_domain_t* long_lived = epoch_domain_wrap(alloc, 0, false);
epoch_advance(alloc);  // Move to epoch 1
epoch_close(alloc, 5);  // Close old epoch 5 (not epoch 0)
```

**Rule of thumb:** 
- Domains represent intended lifetime boundaries - don't violate them by manually advancing
- Low-level code MAY call epoch_close on other epochs (part of cleanup policy)
- If using domains for automatic management, don't mix with manual epoch_advance in same scope

---

## Summary: The Epoch/Domain Contract

### Epoch Invariants

1. **Ring buffer:** 16 epochs (0-15), wraps at 16
2. **Era-based identity:** `(epoch_id, era)` pair uniquely identifies incarnation
3. **Two states:** ACTIVE (accepts allocations), CLOSING (rejects allocations)
4. **epoch_advance():** Marks previous CLOSING, advances to next ACTIVE
5. **epoch_close():** Reclaims empties in CLOSING epochs (rejection happens in allocation fast path)
6. **Multi-thread safe:** Allocation fast path is lock-free

### Domain Invariants

1. **Thread-local scope:** enter/exit/destroy on owner thread only
2. **Refcount-based nesting:** LIFO stack (32-element TLS array)
3. **Two lifecycle modes:** auto_close=false (explicit), auto_close=true (automatic)
4. **Era validation:** Prevents closing wrong epoch after wraparound
5. **Dual refcount:** Domain-local (nesting depth), allocator-global (leak detection)

### Design Rationale

**Why epochs exist:**
- Group allocations by temporal phase
- Deterministic reclamation at lifetime boundaries
- No heuristic-based cleanup (explicit control)

**Why domains exist:**
- RAII-style automatic cleanup
- Composable nested scopes
- Thread-local context (no explicit passing)

**Why thread-local domains:**
- Eliminates cross-thread refcount contention
- Simplifies locking (no memory barriers on enter/exit)
- Fastest possible implementation

**Why era stamping:**
- Prevents closing wrong epoch after ring wraparound
- Enables monotonic observability (graphs don't confuse wrap)
- Safe auto-close even with long-lived domains

---

## Future Work

### Not Yet Implemented

1. **Handle indirection + munmap()** (Phase 3)
   - Current: Virtual memory stays mapped, madvise releases physical pages
   - Future: Handle → slab_id → slab_table[generation, state, ptr] → slab
   - Enables: Real munmap() with crash-proof stale frees

2. **NUMA-aware domain placement** (Phase 4)
   - Current: Single allocator for all threads
   - Future: Per-NUMA-node allocators with cross-node free

3. **Domain labels** (Phase 2.3 semantic attribution)
   - Current: Implemented (slab_epoch_set_label API)
   - Status: Wired to stats_dump JSON output

4. **Epoch-close telemetry** (Phase 2.1)
   - Current: Not implemented
   - Future: epoch_close_calls, scanned_slabs, recycled_slabs counters

---

## Related Documentation

- `EPOCH_DOMAINS.md` - Pattern catalog with concrete examples
- `OBSERVABILITY_DESIGN.md` - Phase 2 observability roadmap
- `docs/foundations.md` - Theoretical foundation (temporal fragmentation)
- `include/epoch_domain.h` - Public API reference
- `CHANGELOG.md` - Issue #1 resolution details

---

**Maintainer:** blackwd  
**Version:** 1.0 (canonical reference)  
**Last reviewed:** 2026-02-07
