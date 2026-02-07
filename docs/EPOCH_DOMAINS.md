# Epoch Domains: Structured Temporal Memory Management

Epoch domains provide **RAII-style scoped lifetimes** on top of temporal-slab's raw epoch API. They formalize the relationship between allocation scope and epoch lifecycle, enabling automatic cleanup and nested temporal scopes.

## When to Use Domains vs Raw Epochs

**Use raw epochs when:**
- You need explicit control over epoch advancement (e.g., rotating every N requests)
- Multiple subsystems share the same epoch (coordinated lifecycle)
- Performance-critical paths where function call overhead matters
- You're building custom lifetime abstractions

**Use epoch domains when:**
- You want automatic cleanup at scope boundaries (request handlers, frame loops)
- Lifetimes naturally nest (transaction → query, frame → render pass)
- You need reusable scopes with deterministic reclamation (frame-based systems)
- You want RAII-style memory management in C

**Core trade-off:** Domains add a thin abstraction layer (refcount tracking, thread-local context) in exchange for automatic epoch_close() and composable scopes.

## Core Concepts

### 1. RAII-Style Scoping

Domains bind epoch lifecycle to scope entry/exit:

```c
epoch_domain_t* request = epoch_domain_create(alloc);

epoch_domain_enter(request);
{
    // Allocations tied to request lifetime
    void* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
    void* buffer = slab_malloc_epoch(alloc, 256, request->epoch_id);
    
    // No explicit frees needed
}
epoch_domain_exit(request);  // Automatic epoch_close() if auto_close=true

epoch_domain_destroy(request);
```

**Key insight:** Memory reclamation happens at **structural boundaries** (domain exit), not individual object lifetimes. This aligns memory lifecycle with program phase transitions.

### 2. Refcount Semantics

Domains track nesting depth via refcount:

```c
epoch_domain_t* domain = epoch_domain_create(alloc);

epoch_domain_enter(domain);  // refcount: 0 → 1
    epoch_domain_enter(domain);  // refcount: 1 → 2 (nested)
    epoch_domain_exit(domain);   // refcount: 2 → 1
epoch_domain_exit(domain);   // refcount: 1 → 0 → triggers epoch_close()
```

**Safety contract:** Domain can only be destroyed when refcount = 0. Destroying an active domain is undefined behavior (use-after-free risk).

### 3. Thread-Local Context

`epoch_domain_enter()` sets thread-local state for implicit binding:

```c
epoch_domain_t* domain = epoch_domain_create(alloc);
epoch_domain_enter(domain);

// Helper can access domain without explicit passing
epoch_domain_t* current = epoch_domain_current();
SlabAllocator* alloc = epoch_domain_allocator();
```

**Caveat:** Thread-local context is per-thread. If multiple threads share an allocator, each thread needs separate domain enter/exit calls.

### 4. Auto-Close vs Manual Control

Domains support two lifecycle modes:

**Auto-close (default):**
```c
epoch_domain_t* request = epoch_domain_create(alloc);  // auto_close=true
epoch_domain_enter(request);
    // allocations...
epoch_domain_exit(request);  // Automatic epoch_close() when refcount → 0
```

**Manual control:**
```c
EpochId epoch = epoch_current(alloc);
epoch_domain_t* frame = epoch_domain_wrap(alloc, epoch, false);  // auto_close=false

for (int i = 0; i < frames; i++) {
    epoch_domain_enter(frame);
        // frame allocations...
    epoch_domain_exit(frame);  // No epoch_close(), refcount just decrements
}

epoch_domain_force_close(frame);  // Explicit cleanup when done
```

**When to use manual control:**
- Reusable scopes across multiple iterations (game frames, batch processing)
- Delayed cleanup (accumulate across cycles, then bulk reclaim)
- Performance-critical loops where epoch_close() overhead matters

## Pattern Catalog

### Pattern 1: Request-Scoped Allocation

**Use case:** Web servers, RPC handlers, request processors

**Goal:** Allocate request-scoped memory, reclaim deterministically on completion

```c
void handle_request(Connection* conn) {
    epoch_domain_t* request = epoch_domain_create(alloc);
    epoch_domain_enter(request);
    {
        char* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
        char* headers = slab_malloc_epoch(alloc, 256, request->epoch_id);
        char* body = slab_malloc_epoch(alloc, 504, request->epoch_id);
        
        parse_request(conn, headers, body);
        authenticate(session);
        process_request(session, body);
        send_response(conn);
        
        // No individual frees needed
    }
    epoch_domain_exit(request);  // All request memory reclaimed
    epoch_domain_destroy(request);
}
```

**Why this works:**
- Request lifetime is well-defined (enter handler → exit handler)
- All allocations share the same lifetime (no mixed-lifetime objects)
- Automatic cleanup prevents leaks on early returns or exceptions

**Measured benefit:** Eliminates per-request free() calls, 0-2.4% RSS growth vs unbounded malloc drift.

### Pattern 2: Reusable Frame Domain

**Use case:** Game engines, simulation loops, rendering pipelines

**Goal:** Reuse same domain across frames for deterministic per-frame cleanup

```c
void run_game_loop(SlabAllocator* alloc) {
    epoch_advance(alloc);
    EpochId frame_epoch = epoch_current(alloc);
    epoch_domain_t* frame = epoch_domain_wrap(alloc, frame_epoch, false);  // Manual control
    
    for (int i = 0; i < frame_count; i++) {
        epoch_domain_enter(frame);
        {
            void* render_data = slab_malloc_epoch(alloc, 384, frame->epoch_id);
            void* particle_state = slab_malloc_epoch(alloc, 256, frame->epoch_id);
            void* debug_overlay = slab_malloc_epoch(alloc, 128, frame->epoch_id);
            
            update_game_state();
            render_frame(render_data);
            simulate_particles(particle_state);
            draw_debug_info(debug_overlay);
        }
        epoch_domain_exit(frame);  // Deterministic frame cleanup
    }
    
    epoch_domain_force_close(frame);
    epoch_domain_destroy(frame);
}
```

**Why manual control:**
- Frame domain reused across iterations (no per-frame create/destroy overhead)
- Explicit force_close() controls when memory is actually reclaimed
- Predictable latency (no surprise epoch_close() during frame)

**Performance characteristic:** Zero allocation overhead per frame (domain reused), cleanup amortized across frames.

### Pattern 3: Nested Domains (Transaction + Query)

**Use case:** Database transactions, nested API calls, hierarchical scopes

**Goal:** Short-lived inner scope (query) within longer outer scope (transaction)

```c
void execute_query(epoch_domain_t* query_domain) {
    epoch_domain_enter(query_domain);
    {
        void* result_set = slab_malloc_epoch(query_domain->alloc, 256, query_domain->epoch_id);
        void* index_buffer = slab_malloc_epoch(query_domain->alloc, 256, query_domain->epoch_id);
        
        run_sql_query(result_set);
        build_index(index_buffer);
    }
    epoch_domain_exit(query_domain);
    // Query memory reclaimed here
}

void run_transaction(SlabAllocator* alloc) {
    epoch_domain_t* transaction = epoch_domain_create(alloc);
    epoch_domain_enter(transaction);
    {
        void* txn_log = slab_malloc_epoch(alloc, 512, transaction->epoch_id);
        
        begin_transaction(txn_log);
        
        // Nested query scope
        epoch_domain_t* query = epoch_domain_create(alloc);
        execute_query(query);
        epoch_domain_destroy(query);  // Query memory freed before commit
        
        void* commit_data = slab_malloc_epoch(alloc, 256, transaction->epoch_id);
        commit_transaction(commit_data);
    }
    epoch_domain_exit(transaction);
    // Transaction memory reclaimed here
    
    epoch_domain_destroy(transaction);
}
```

**Why nesting matters:**
- Query results don't need to outlive query execution (early reclamation)
- Transaction log survives across nested queries (different lifetimes)
- Memory pressure reduced (query slabs recycled before commit phase)

**Critical insight:** Nested domains enable **hierarchical lifetime management** - inner scopes reclaim memory before outer scopes complete.

### Pattern 4: Explicit Lifetime Control

**Use case:** Batch processing, accumulation patterns, delayed cleanup

**Goal:** Accumulate allocations across multiple cycles, bulk reclaim at end

```c
void process_batches(SlabAllocator* alloc) {
    epoch_advance(alloc);
    EpochId batch_epoch = epoch_current(alloc);
    epoch_domain_t* domain = epoch_domain_wrap(alloc, batch_epoch, false);  // Manual control
    
    // Multiple enter/exit cycles, memory accumulates
    for (int batch = 0; batch < 10; batch++) {
        epoch_domain_enter(domain);
        {
            for (int item = 0; item < 1000; item++) {
                void* buffer = slab_malloc_epoch(alloc, 128, batch_epoch);
                process_item(buffer);
                // No free - memory accumulates
            }
        }
        epoch_domain_exit(domain);  // Exit domain, but memory still allocated
        
        printf("Batch %d complete, memory still allocated\n", batch);
    }
    
    // Explicit bulk cleanup
    epoch_domain_force_close(domain);
    printf("All batches reclaimed\n");
    
    epoch_domain_destroy(domain);
}
```

**Why defer cleanup:**
- Batch processing may need results from previous batches
- Amortized cleanup cost (one epoch_close() vs 10)
- Explicit control over when reclamation happens (not tied to scope exit)

**Trade-off:** Higher RSS during accumulation phase, but deterministic cleanup point.

## Performance Characteristics

### Domain Overhead

**Zero-cost abstraction when unused:**
- If you use raw epochs (epoch_advance/epoch_close), domains add no overhead
- Domain structs only allocated when explicitly created

**Per-operation costs:**
- `epoch_domain_create()`: malloc(sizeof(epoch_domain_t)) = ~32 bytes, one-time
- `epoch_domain_enter()`: refcount++, thread-local store = 2-5 cycles
- `epoch_domain_exit()`: refcount--, conditional epoch_close() = 5-10 cycles + epoch_close() if triggered
- `epoch_domain_destroy()`: free(domain) = ~50-100 cycles

**Measured overhead vs raw epochs:**
- Request-scoped pattern: +10-20ns per request (negligible vs 30-70ns allocation median)
- Frame-reuse pattern: ~0ns amortized (domain reused across frames)

**Key insight:** Domain abstraction is essentially free - the real cost is epoch_close() (madvise syscalls), which you'd call manually anyway.

### Thread-Local Context Cost

Thread-local storage (TLS) access is fast on modern systems:
- Read: 1-2 cycles (compiler may inline to register)
- Write: 2-5 cycles (may require store barrier)

**Impact:** Negligible compared to allocation fast path (~30-70ns).

## Safety Contracts

### 1. Refcount Must Reach Zero Before Destroy

```c
epoch_domain_t* domain = epoch_domain_create(alloc);
epoch_domain_enter(domain);
    // refcount = 1
epoch_domain_destroy(domain);  // BUG: refcount != 0, undefined behavior
```

**Correct pattern:**
```c
epoch_domain_enter(domain);
    // work...
epoch_domain_exit(domain);  // refcount → 0
epoch_domain_destroy(domain);  // Safe
```

### 2. Domain Must Not Outlive Allocator

```c
SlabAllocator* alloc = slab_allocator_create();
epoch_domain_t* domain = epoch_domain_create(alloc);

slab_allocator_free(alloc);  // Allocator destroyed
epoch_domain_exit(domain);   // BUG: use-after-free, allocator is gone
```

**Correct pattern:**
```c
epoch_domain_t* domain = epoch_domain_create(alloc);
// use domain...
epoch_domain_destroy(domain);  // Domain destroyed first
slab_allocator_free(alloc);     // Then allocator
```

### 3. Thread-Local Context is Per-Thread

```c
epoch_domain_t* domain = epoch_domain_create(alloc);
epoch_domain_enter(domain);  // Sets thread-local on Thread A

// On Thread B:
epoch_domain_t* current = epoch_domain_current();  // Returns NULL (different thread)
```

**Correct pattern for multi-threaded use:**
```c
// Thread A
epoch_domain_enter(domain);
// allocations on Thread A...
epoch_domain_exit(domain);

// Thread B
epoch_domain_enter(domain);  // Separate enter call on Thread B
// allocations on Thread B...
epoch_domain_exit(domain);   // Separate exit call on Thread B
```

### 4. Nested Domains Must Be Properly Unwound

```c
epoch_domain_t* outer = epoch_domain_create(alloc);
epoch_domain_t* inner = epoch_domain_create(alloc);

epoch_domain_enter(outer);
    epoch_domain_enter(inner);
        // work...
    // BUG: forgot to exit inner
epoch_domain_exit(outer);  // Leaks inner domain refcount
```

**Correct pattern:**
```c
epoch_domain_enter(outer);
    epoch_domain_enter(inner);
        // work...
    epoch_domain_exit(inner);  // Properly unwind
epoch_domain_exit(outer);
```

## Anti-Patterns

### Anti-Pattern 1: Nested Domains Across Different Allocators

```c
SlabAllocator* alloc_a = slab_allocator_create();
SlabAllocator* alloc_b = slab_allocator_create();

epoch_domain_t* domain_a = epoch_domain_create(alloc_a);
epoch_domain_t* domain_b = epoch_domain_create(alloc_b);

epoch_domain_enter(domain_a);  // Thread-local = domain_a
    epoch_domain_enter(domain_b);  // Thread-local = domain_b (overwrites)
        SlabAllocator* alloc = epoch_domain_allocator();  // Returns alloc_b
        void* p = slab_malloc_epoch(alloc, 128, ???);  // Which epoch???
    epoch_domain_exit(domain_b);
epoch_domain_exit(domain_a);
```

**Problem:** Thread-local context only tracks one domain at a time. Nesting domains from different allocators causes confusion.

**Solution:** Keep nested domains within same allocator, or avoid thread-local context (pass domain explicitly).

### Anti-Pattern 2: Forgetting to Exit Domain

```c
void handle_request(Connection* conn) {
    epoch_domain_t* request = epoch_domain_create(alloc);
    epoch_domain_enter(request);
    {
        // process request...
        if (error) {
            return;  // BUG: forgot epoch_domain_exit()
        }
    }
    epoch_domain_exit(request);
    epoch_domain_destroy(request);
}
```

**Problem:** Early return leaks domain refcount, prevents cleanup.

**Solution:** Use goto cleanup pattern or ensure all exit paths call domain_exit:

```c
void handle_request(Connection* conn) {
    epoch_domain_t* request = epoch_domain_create(alloc);
    epoch_domain_enter(request);
    
    bool success = process_request(conn);
    
    epoch_domain_exit(request);
    epoch_domain_destroy(request);
    
    return success;
}
```

### Anti-Pattern 3: Reusing Auto-Close Domain Across Iterations

```c
epoch_domain_t* frame = epoch_domain_create(alloc);  // auto_close=true (default)

for (int i = 0; i < 100; i++) {
    epoch_domain_enter(frame);
        // frame allocations...
    epoch_domain_exit(frame);  // Calls epoch_close() every frame!
}
```

**Problem:** auto_close=true triggers epoch_close() on every exit, defeating reuse optimization.

**Solution:** Use manual control for reusable domains:

```c
EpochId epoch = epoch_current(alloc);
epoch_domain_t* frame = epoch_domain_wrap(alloc, epoch, false);  // Manual control

for (int i = 0; i < 100; i++) {
    epoch_domain_enter(frame);
        // frame allocations...
    epoch_domain_exit(frame);  // Just decrements refcount
}

epoch_domain_force_close(frame);  // Explicit cleanup
```

### Anti-Pattern 4: Mixing Domain and Raw Epoch APIs

```c
epoch_domain_t* domain = epoch_domain_create(alloc);
epoch_domain_enter(domain);
{
    void* p = slab_malloc_epoch(alloc, 128, domain->epoch_id);
    epoch_advance(alloc);  // BUG: advances past domain's epoch!
    // p is now in wrong epoch context
}
epoch_domain_exit(domain);
```

**Problem:** Manually calling epoch_advance() invalidates domain's epoch_id.

**Solution:** Either use domains exclusively (let them manage epochs), or use raw epoch API exclusively.

## Design Rationale

### Why Domains Exist

Epoch domains solve three problems with raw epoch API:

**Problem 1: Manual cleanup is error-prone**
```c
// Raw epochs - easy to forget epoch_close()
EpochId epoch = epoch_current(alloc);
// allocations...
// BUG: forgot to call epoch_close(), memory never reclaimed
```

**Solution: Automatic cleanup**
```c
// Domains - automatic cleanup on scope exit
epoch_domain_t* domain = epoch_domain_create(alloc);
epoch_domain_enter(domain);
    // allocations...
epoch_domain_exit(domain);  // Automatic epoch_close()
```

**Problem 2: Nesting is complex with raw epochs**
```c
// Raw epochs - manual nesting management
EpochId outer = epoch_current(alloc);
epoch_advance(alloc);
EpochId inner = epoch_current(alloc);
// allocations in inner...
epoch_close(alloc, inner);
// allocations in outer...
epoch_close(alloc, outer);  // Must remember to close outer too
```

**Solution: Refcount-based nesting**
```c
// Domains - automatic nesting via refcount
epoch_domain_enter(outer);
    epoch_domain_enter(inner);
        // allocations...
    epoch_domain_exit(inner);  // Automatic cleanup
epoch_domain_exit(outer);      // Automatic cleanup
```

**Problem 3: No implicit context for helper functions**
```c
// Raw epochs - must pass epoch ID explicitly
void helper(SlabAllocator* alloc, EpochId epoch) {
    void* p = slab_malloc_epoch(alloc, 128, epoch);  // Explicit epoch
}
```

**Solution: Thread-local context**
```c
// Domains - implicit context via TLS
void helper() {
    SlabAllocator* alloc = epoch_domain_allocator();
    epoch_domain_t* domain = epoch_domain_current();
    void* p = slab_malloc_epoch(alloc, 128, domain->epoch_id);
}
```

### Why Not Always Use Domains?

Domains are optional for good reasons:

**1. Explicit control matters in performance-critical code**
- Raw epochs let you control exactly when epoch_close() happens
- Domains may trigger cleanup at inopportune times (if auto_close=true)

**2. Simple workloads don't need the abstraction**
- If you have one epoch per subsystem, raw API is simpler
- Domains add conceptual overhead for minimal benefit

**3. Thread-local context can be surprising**
- Multiple allocators + domains = potential confusion
- Explicit epoch passing is clearer in complex scenarios

**Rule of thumb:** Start with raw epochs, introduce domains when you need automatic cleanup or nested scopes.

## See Also

- `examples/domain_usage.c` - Concrete code examples for all patterns
- `include/epoch_domain.h` - Full API reference
- `docs/foundations.md` - Theoretical foundation (epochs, temporal fragmentation)
- `README.md` - Quick start with raw epoch API
