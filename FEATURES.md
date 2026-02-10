# ZNS-Slab Features

## Core Capabilities

### 1. Variance Elimination (Sub-100ns Predictable Allocation)

**Problem Solved**: General-purpose allocators (jemalloc, tcmalloc) exhibit 500× tail latency variance due to:
- Lock contention under multi-threaded load
- Heuristic-based decisions (best-fit search, coalescing, compaction)
- Unpredictable OS interaction (mmap/munmap, page faults)

**Our Solution**: Algorithmic allocation strategy eliminates non-determinism:

| Metric | ZNS-Slab | jemalloc |
|--------|----------|----------|
| Median | 73ns | 85ns |
| 99th percentile | 125ns | 1,200ns |
| 99.9th percentile | 167ns | 3,800ns |
| Max observed | 8,934ns | 40,127ns |
| **Variance ratio** | **2.3×** | **471×** |

**Key technique**: Lock-free fast path (97% hit rate) + epoch-based slab selection (no search).

---

### 2. Temporal Memory Management (Epoch-Based Allocation)

**Concept**: Group allocations by lifetime phase rather than size alone.

**API**:
```c
epoch_advance(alloc);                    // Rotate to next epoch
EpochId epoch = epoch_current(alloc);    // Get active epoch ID
void* ptr = slab_malloc_epoch(alloc, size, epoch);
epoch_close(alloc, epoch);               // Trigger RSS reclamation
```

**Benefits**:
- **Deterministic reclamation**: `epoch_close()` aligns with application boundaries (request end, frame end, batch commit)
- **Temporal locality**: Objects from same phase allocated contiguously (cache-friendly)
- **Bulk deallocation**: Close entire epoch → all slabs recycled atomically

**Use Cases**:
- **Web servers**: 1 epoch per request (close on response sent)
- **Game engines**: 1 epoch per frame (close after render)
- **Batch processing**: 1 epoch per transaction batch

---

### 3. RSS Reclamation (Deterministic Memory Return)

**Problem Solved**: Traditional allocators hold onto freed memory indefinitely (RSS bloat).

**Our Solution**: Explicit reclamation boundaries via `epoch_close()`:

1. Mark epoch as `EPOCH_CLOSING` → refuse new allocations
2. Scan for empty slabs across all size classes
3. Recycle empty slabs → slab cache → `madvise(MADV_DONTNEED)`
4. Physical pages returned to OS → RSS drops to baseline

**Measured Results** (100M allocation churn test):
- **Active RSS**: 768KB (8 slabs × 8 size classes × 4KB per slab)
- **After `epoch_close()`**: ~64KB (1-2 residual slabs)
- **Reclamation ratio**: 39-69× memory reduction

**Trade-offs**:
- Opt-in via `-DENABLE_RSS_RECLAMATION=1` (disabled by default)
- Slight latency cost on slab reuse (kernel zero-fill overhead ~200ns)
- Virtual memory stays mapped (safe for stale handle validation)

---

### 4. RAII Memory Management (Epoch Domains)

**Problem Solved**: Manual epoch lifecycle management is error-prone (forget to close → RSS leak).

**Our Solution**: RAII wrappers with automatic cleanup.

Epoch Domains demonstrate **three levels of lifetime control**, proving that Structural Determinism scales from simple to complex workloads:

#### Level 1: The Death of the free() Call (Request Scope)

```c
epoch_domain_t* request = epoch_domain_create(alloc);
epoch_domain_enter(request);
{
    char* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
    char* cache = slab_malloc_epoch(alloc, 256, request->epoch_id);
    char* buffer = slab_malloc_epoch(alloc, 504, request->epoch_id);
    // No individual free() calls needed
}
epoch_domain_exit(request);  // All memory reclaimed
epoch_domain_destroy(request);
```

**What this proves:** In high-throughput systems (RPC servers, API gateways), tracking what to free when is cognitive overhead. By binding allocations to request lifetime, you eliminate an entire class of resource leaks.

**Malloc reality:** Every allocation is a promise you might break (leak) or fulfill twice (crash).  
**temporal-slab reality:** Cleanup is structural, not manual.

**The 2026 play:** Serverless functions (AWS Lambda, Cloudflare Workers) that must return memory after each invocation without cold-start bloat.

---

#### Level 2: Deterministic Nesting (Transactions > Queries)

```c
epoch_domain_t* transaction = epoch_domain_create(alloc);
epoch_domain_enter(transaction);
{
    char* txn_log = slab_malloc_epoch(alloc, 512, transaction->epoch_id);
    
    // Nested query scope
    epoch_domain_t* query = epoch_domain_create(alloc);
    execute_query(query);  // Query metadata reclaimed immediately
    epoch_domain_destroy(query);
    
    // Transaction continues with txn_log still valid
    char* commit_data = slab_malloc_epoch(alloc, 256, transaction->epoch_id);
}
epoch_domain_exit(transaction);
```

**What this proves:** Short-lived lifetimes can nest inside longer-lived ones. A query domain ends while its transaction domain continues.

This allows:
- Immediate reclamation of query metadata
- Preservation of transaction state
- Zero fragmentation between concurrent lifetimes

**The AWS RDS pitch:** Database query planning generates temporary metadata (parse trees, optimizer stats). Query domains reclaim planning metadata immediately while keeping result sets alive until commit/rollback.

**The 2026 edge play:** Multiple concurrent AI agent "thoughts" that reclaim independently without poisoning each other's memory locality.

---

#### Level 3: Explicit Control (Batch Processing)

```c
epoch_advance(alloc);
EpochId epoch = epoch_current(alloc);
epoch_domain_t* domain = epoch_domain_wrap(alloc, epoch, false);  // Manual control

for (int i = 0; i < 3; i++) {
    epoch_domain_enter(domain);
    {
        char* buffer = slab_malloc_epoch(alloc, 128, epoch);
        // Accumulate data across iterations
    }
    epoch_domain_exit(domain);
    // Memory persists across enter/exit cycles
}

epoch_domain_force_close(domain);  // Explicit bulk reclamation
epoch_domain_destroy(domain);
```

**What this proves:** The system adapts to workload needs instead of forcing a single philosophy.

- `epoch_domain_create()` → automatic, RAII-style lifetimes
- `epoch_domain_wrap()` → manual lifecycle control  
- `epoch_domain_force_close()` → explicit reclamation when needed

**The Databricks/Snowflake pitch:** ETL pipelines and log aggregation systems that batch rows into columnar formats need to accumulate allocations across stages, then bulk-free when complete.

---

**Summary:** These three levels prove that Epoch Domains aren't a niche optimization—they're a **design pattern** that works from simple (web requests) to complex (nested transactions with explicit control).

---

### 6. Lock-Free Fast Path (97% Contention-Free Allocation)

**Architecture**:
- **Per-epoch `current_partial` pointer** (atomic, lock-free read)
- **Bitmap-based slot allocation** (CAS loop, no mutex)
- **Slow path fallback** (mutex-protected list management on cache miss)

**Performance Impact**:
- 97% of allocations complete without taking a lock
- 3% slow path (new slab creation, list management)
- Near-zero lock contention in multi-threaded workloads

**Measured Scalability** (8-thread benchmark):
- **1 thread**: 73ns median
- **8 threads**: 82ns median (only 12% degradation)
- **Linear speedup**: 8M allocs/sec per thread

---

### 5. Production Observability (Phase 2.0 - Attribution-Based Diagnostics)

**Problem Solved**: When tail latency spikes or RSS grows unbounded, traditional allocators offer no visibility into *why*.

**Our Solution**: Versioned snapshot-based observability that answers four critical questions:

#### A. Why did we go slow?

Slow-path attribution counters pinpoint exact cause:

```c
SlabClassStats cs;
slab_stats_class(alloc, 2, &cs);  // 128-byte class

printf("Slow path breakdown:\n");
printf("  cache miss:    %lu (needed mmap)\n", cs.slow_path_cache_miss);
printf("  epoch closed:  %lu (allocation rejected)\n", cs.slow_path_epoch_closed);
printf("  partial null:  %lu (no cached slab)\n", cs.current_partial_null);
printf("  partial full:  %lu (cached slab exhausted)\n", cs.current_partial_full);
```

**Commercial value**: "Why did latency spike at 3am?" → Precise attribution eliminates guesswork.

#### B. Why is RSS not dropping?

madvise tracking shows reclamation effectiveness:

```c
SlabGlobalStats gs;
slab_stats_global(alloc, &gs);

printf("RSS reclamation:\n");
printf("  madvise calls:    %lu\n", gs.total_madvise_calls);
printf("  bytes reclaimed:  %.2f MB\n", gs.total_madvise_bytes / 1024.0 / 1024);
printf("  failures:         %lu\n", gs.total_madvise_failures);
```

**Commercial value**: "RSS stuck at 10GB after epoch close" → madvise_failures reveals kernel/permission issue.

#### C. What's happening per epoch?

Epoch-level visibility shows memory distribution:

```c
SlabEpochStats es;
slab_stats_epoch(alloc, 2, 5, &es);  // Class 2, Epoch 5

printf("Epoch %u (%s):\n", es.epoch_id, 
       es.state == EPOCH_ACTIVE ? "ACTIVE" : "CLOSING");
printf("  Slabs: %u partial, %u full (%.2f KB RSS)\n",
       es.partial_slab_count, es.full_slab_count,
       es.estimated_rss_bytes / 1024.0);
printf("  Reclaimable: %u slabs (%.2f KB)\n",
       es.reclaimable_slab_count, es.reclaimable_bytes / 1024.0);
```

**Commercial value**: "Which epoch is leaking memory?" → Per-epoch RSS footprint shows culprit.

#### D. What's the allocator shape right now?

Snapshot visibility for capacity planning:

```c
slab_stats_class(alloc, 2, &cs);

printf("Cache state:\n");
printf("  Array:    %u/%u\n", cs.cache_size, cs.cache_capacity);
printf("  Overflow: %u\n", cs.cache_overflow_len);
printf("  Recycle rate: %.1f%%\n", cs.recycle_rate_pct);

printf("Slab distribution:\n");
printf("  Partial: %u | Full: %u\n", cs.total_partial_slabs, cs.total_full_slabs);
```

**Commercial value**: "Should we increase cache size?" → 95%+ recycle rate means cache is sized correctly.

#### Observability Tool: stats_dump

Dual-output pattern for production use:

```bash
# JSON for tooling (jq, Prometheus, CI diffs)
./stats_dump --no-text | jq '.classes[2].slow_path_cache_miss'

# Text for human debugging
./stats_dump --no-json 2>&1 | grep "madvise"

# Both simultaneously (default)
./stats_dump | jq . 2>&1 | less
```

**Key features**:
- **Stable contract**: JSON keys match struct field names exactly
- **Versioned API**: `SLAB_STATS_VERSION` for forward compatibility
- **Zero overhead**: Relaxed atomic reads on slow paths only
- **Snapshot semantics**: Point-in-time view (~100µs cost)

**Diagnostic patterns enabled**:
- **RSS not dropping**: Check `madvise_failures` → kernel issue
- **High slow-path rate**: Check attribution → tune cache or fix epoch usage
- **Memory leak**: Per-epoch RSS shows which epoch won't drain

#### Phase 2.1-2.4: Complete Observability Pipeline

Building on Phase 2.0's foundation, four additional phases transform epochs into **fully observable temporal windows**:

##### Phase 2.1: O(1) Reclaimable Tracking

**Problem**: Scanning partial lists to find empty slabs is O(n) expensive.

**Solution**: Atomic `empty_partial_count` per epoch:
```c
SlabEpochStats es;
slab_stats_epoch(alloc, 2, 5, &es);
printf("Reclaimable: %u slabs (%.2f KB) [O(1) query]\n",
       es.reclaimable_slab_count, es.reclaimable_bytes / 1024.0);
```

**Impact**: 100+ slabs → O(1) atomic load (~5ns vs ~1µs scan)

##### Phase 2.2: Era Stamping

**Problem**: "Is this epoch 0 from now, or from 5 rotations ago?"

**Solution**: Monotonic 64-bit era counter:
```c
slab_stats_epoch(alloc, 2, 0, &es);
printf("Epoch %u, Era %lu\n", es.epoch_id, es.epoch_era);
// Example: (epoch=0, era=105) vs (epoch=0, era=110) = different rotations
```

**Use case**: Correlate with logs: "Request XYZ allocated in (epoch=3, era=1250)"

##### Phase 2.3: Epoch Metadata (Temporal Profiling)

**Problem**: Epochs are opaque buckets. Can't debug "why stuck?"

**Solution**: Three metadata fields per epoch:
```c
slab_stats_epoch(alloc, 2, 5, &es);
printf("Epoch 5:\n");
printf("  open_since_ns: %lu (opened %.1f sec ago)\n", 
       es.open_since_ns, (now_ns() - es.open_since_ns) / 1e9);
printf("  alloc_count: %lu live allocations\n", es.alloc_count);
printf("  label: '%s'\n", es.label);  // e.g., "request_id:abc123"
```

**Use cases**:
- **Detect stuck epochs**: `open_since_ns` 2 minutes old + `alloc_count=15000`
- **Profile allocation patterns**: `alloc_count` spikes correlate with load
- **Correlate with app phases**: Labels like "batch_42", "frame:1234"

##### Phase 2.4: RSS Delta Tracking

**Problem**: "Did closing the epoch actually free memory?"

**Solution**: Before/after RSS measurements:
```c
epoch_close(alloc, 5);  // Triggers measurement
slab_stats_epoch(alloc, 2, 5, &es);
uint64_t delta = es.rss_before_close - es.rss_after_close;
printf("Epoch 5 closed:\n");
printf("  rss_before: %.2f MB\n", es.rss_before_close / 1024.0 / 1024);
printf("  rss_after:  %.2f MB\n", es.rss_after_close / 1024.0 / 1024);
printf("  delta:      %.2f MB reclaimed\n", delta / 1024.0 / 1024);
```

**Value**: Quantifies reclamation effectiveness → validates RSS reclamation is working

#### Complete Observability Workflow

The five phases answer a complete debugging workflow:

| Question | Phase | API |
|----------|-------|-----|
| Why did allocation slow down? | 2.0 | `slow_path_cache_miss`, `slow_path_epoch_closed` |
| Which epoch is using memory? | 2.1 | `reclaimable_slab_count` (O(1)) |
| Is this the same epoch or new rotation? | 2.2 | `epoch_era` (monotonic ID) |
| How old is this epoch? | 2.3 | `open_since_ns` (age in nanoseconds) |
| How many objects still live? | 2.3 | `alloc_count` (refcount) |
| What caused these allocations? | 2.3 | `label` (semantic tag) |
| Did closing it free memory? | 2.4 | RSS delta (MB reclaimed) |

**Production scenario**: "Epoch 7 using 40MB"
1. Check `open_since_ns`: Opened 2 minutes ago (stuck!)
2. Check `alloc_count`: 15,000 live allocations (leak!)
3. Check `label`: "session:batch_42" (identifies culprit)
4. Close epoch: `rss_before=40MB`, `rss_after=2MB` (38MB reclaimed)

---

### 7. ABA-Safe Handle Validation (Portable, No UAF)

**Problem Solved**: Stale handles must not cause crashes or use-after-free.

**Our Solution**: Three-layer safety:

1. **Slab Registry** - Indirection layer with generation counters
2. **Portable encoding** - No raw pointers (works on 32-bit, 64-bit, ARM, x86)
3. **Validation handshake** - Atomic acquire/release ensures consistency

**Handle Format** (64-bit):
```
[63:42] slab_id (22 bits)    - Registry index
[41:18] generation (24 bits) - ABA protection (16M reuses before wrap)
[17:10] slot (8 bits)         - Object index (max 255 per slab)
[9:2]   size_class (8 bits)   - Size class index
[1:0]   version (2 bits)      - Handle format version (v1 = 0b01)
```

**Safety Guarantees**:
- **Stale handles**: Return `NULL` on `slab_free()` (no crash)
- **Double-free**: Detected by bitmap check (no corruption)
- **Use-after-free**: Impossible (freed memory stays in slab until epoch close)
- **Cross-platform**: Works on all architectures (no pointer tagging)

---

### 8. Configurable Reclamation Policy

**Compile-time control**:
```bash
# Latency-first (no RSS reclamation, keep memory hot)
gcc -O3 slab_alloc.c  # ENABLE_RSS_RECLAMATION=0 (default)

# RSS-first (aggressive reclamation, slight latency cost)
gcc -O3 -DENABLE_RSS_RECLAMATION=1 slab_alloc.c
```

**Runtime control**:
- Allocate in ACTIVE epochs → memory stays hot
- Close epochs explicitly → RSS drops on demand
- Hybrid strategy: Close old epochs, keep recent hot

---

## Design Highlights

### What Makes This Fast

1. **Lock-free fast path** - 97% of allocations never take a mutex
2. **O(1) size class lookup** - 768-byte table (fits in L1 cache)
3. **Bitmap allocation** - Single CAS operation (no linked list traversal)
4. **Slab cache** - 97% reuse rate (no mmap overhead)
5. **Aligned slabs** - 4KB boundaries (cache line friendly)

### What Makes This Predictable

1. **Algorithmic decisions** - No heuristics, no search
2. **Bounded operations** - No O(n) scans in allocation path
3. **Epoch isolation** - Temporal phases don't interfere
4. **Explicit reclamation** - Application controls when RSS drops
5. **Lock-free reads** - Readers never wait on writers

### What Makes This Safe

1. **ABA protection** - 24-bit generation prevents handle reuse bugs
2. **Never unmap slabs** - Virtual memory stays mapped (safe validation)
3. **Atomic transitions** - CAS ensures consistency under concurrency
4. **Handle validation** - Stale handles return NULL (no crash)
5. **Double-free detection** - Bitmap check prevents corruption

---

## Limitations & Trade-offs

### Intentional Constraints

1. **Max allocation size**: 504 bytes (512 - 8-byte handle header)
   - Designed for small object allocation (HFT typical: 64-256 bytes)
   - Large allocations should use separate allocator

2. **Max objects per slab**: 255 (8-bit slot field)
   - Bounds minimum object size to ~16 bytes (4096 / 255)
   - Current size classes (64-768) well within limits

3. **Fixed epoch count**: 128 epochs (configurable via `EPOCH_COUNT`)
   - Wrap-around on `epoch_advance()` reuses old epochs
   - Application must close epochs before wrap (128 rotations)

4. **Slab cache capacity**: 32 slabs per size class (128KB per class)
   - Overflow list handles burst allocations
   - Trade-off: Memory footprint vs reuse rate

### Performance Trade-offs

| Feature | Benefit | Cost |
|---------|---------|------|
| RSS reclamation | RSS drops to baseline | +200ns slab reuse latency |
| Handle validation | No UAF, portable | +1 indirection (registry lookup) |
| Epoch isolation | Temporal locality | 128× memory overhead (worst case) |
| Lock-free fast path | Sub-100ns allocation | Complex CAS retry logic |

---

## Comparison to Alternatives

### vs jemalloc

| Aspect | ZNS-Slab | jemalloc |
|--------|----------|----------|
| **Tail latency** | 2.3× median | 471× median |
| **RSS control** | Explicit (epoch_close) | Implicit (arenas decay) |
| **Max size** | 504 bytes | 8MB+ |
| **Thread cache** | Lock-free shared | TLS per-thread |
| **Use case** | Predictable small allocs | General-purpose |

### vs tcmalloc

| Aspect | ZNS-Slab | tcmalloc |
|--------|----------|----------|
| **Central list** | Per-epoch (isolated) | Per-size-class (global) |
| **Reclamation** | Epoch-aligned | Background thread |
| **Variance** | Algorithmic (bounded) | Heuristic (unbounded) |

### vs Custom Arena Allocators

| Aspect | ZNS-Slab | Arena |
|--------|----------|-------|
| **Deallocation** | Supported (epoch granularity) | Not supported (bulk-only) |
| **Reuse** | Per-object (immediate) | Per-arena (deferred) |
| **Fragmentation** | None (fixed size classes) | High (variable sizes) |
| **Complexity** | Higher (bitmaps, epochs) | Lower (bump pointer) |

**When to use ZNS-Slab**:
- Need sub-100ns latency with bounded variance
- Application has clear lifetime phases (requests, frames, batches)
- Small object allocation dominates (64-512 bytes)
- RSS control matters (containerized environments)

**When NOT to use**:
- Large allocations (>504 bytes) dominate workload
- No clear temporal boundaries (long-lived objects)
- Average-case performance more important than worst-case
- Tight memory budget (epoch isolation has overhead)

---

## API Overview

### Allocator Lifecycle
```c
SlabAllocator* alloc = slab_allocator_create();
// ... use allocator ...
slab_allocator_free(alloc);
```

### Epoch Management (Manual)
```c
epoch_advance(alloc);                    // Rotate to next epoch
EpochId current = epoch_current(alloc);  // Get active epoch
epoch_close(alloc, old_epoch);           // Trigger RSS reclamation
```

### Epoch Management (RAII)
```c
// Auto-closing (single-use)
epoch_domain_t* req = epoch_domain_create(alloc);
epoch_domain_enter(req);
{
    void* ptr = slab_malloc_epoch(alloc, size, req->epoch_id);
    // Use ptr...
}
epoch_domain_exit(req);  // Calls epoch_close() automatically
epoch_domain_destroy(req);

// Manual control (reusable)
epoch_domain_t* frame = epoch_domain_wrap(alloc, epoch, false);
// Reuse across multiple enter/exit cycles...
epoch_domain_force_close(frame);  // Explicit cleanup
epoch_domain_destroy(frame);
```

### Allocation/Deallocation
```c
void* ptr = slab_malloc_epoch(alloc, size, epoch_id);  // Returns NULL on failure
slab_free(alloc, ptr);                                 // Validates handle, safe on NULL
```

### Observability
```c
PerfCounters counters;
get_perf_counters(alloc, size_class, &counters);

uint64_t rss_bytes = read_rss_bytes_linux();  // Current RSS (Linux only)
```

---

## Implementation Details

### Lock-Free Algorithm

**Fast path** (97% of allocations):
1. Atomic load of `epochs[epoch_id].current_partial` (acquire)
2. CAS on bitmap to claim free slot (acq_rel)
3. Check if slab became full → atomic swap `current_partial` to next slab

**Slow path** (3% of allocations):
1. Take mutex for size class
2. Pick head of `epochs[epoch_id].partial` list
3. Publish to `current_partial` (release)
4. Release mutex
5. Retry allocation (now fast path)

**Key insight**: Publish step makes slow path work available to other threads (amortizes lock cost).

### Memory Layout

**Slab structure** (4KB page):
```
[Offset 0]      Slab header (64 bytes, cache-aligned)
[Offset 64]     Bitmap (N/32 words, N = object count)
[Offset 64+4*W] Objects (packed, size-class sized)
```

**Handle encoding** (64-bit):
```
|-- slab_id (22) --|-- gen (24) --|-- slot (8) --|-- cls (8) --|v(2)|
```

**Registry entry**:
```c
struct SlabMeta {
    _Atomic(Slab*) ptr;      // Slab pointer (acquire/release)
    _Atomic(uint32_t) gen;   // Generation counter (relaxed)
};
```

### RSS Reclamation Flow

```
free_obj(handle)
    ↓
Is slab now empty? (free_count == object_count)
    ↓
Is epoch CLOSING? (check epoch_state[epoch_id])
    ↓ YES (CLOSING)
Unlink from partial/full list
    ↓
cache_push(slab)
    ↓
Store slab_id off-page (survives madvise)
    ↓
madvise(slab, MADV_DONTNEED)  ← RSS drops here
    ↓
Slab cached (reusable, but zero-filled on next use)
```

---

## Benchmarking

### Included Tests

1. **`smoke_tests`** - Functional validation (allocation, free, epochs)
2. **`benchmark_accurate`** - Latency distribution (1M alloc/free cycles)
3. **`benchmark_threads`** - Multi-threaded scaling (1-8 threads)
4. **`churn_test`** - RSS bounds validation (100M cycles)
5. **`soak_test`** - Long-running stability test (24+ hours)
6. **`domain_usage`** - RAII pattern demonstration (4 examples)

### Running Benchmarks

```bash
cd src
make

# Latency measurement
./benchmark_accurate

# RSS validation
./churn_test

# Domain patterns
./domain_usage

# Multi-threaded scaling
./benchmark_threads 8  # 8 threads
```

### Interpreting Results

**Latency metrics**:
- **Median** - Typical allocation latency (target: <100ns)
- **99th** - 1 in 100 outlier (target: <200ns)
- **99.9th** - 1 in 1000 outlier (target: <1µs)
- **Max** - Worst-case observed (target: <10µs)

**RSS metrics**:
- **Baseline** - RSS after 100M allocs (should plateau)
- **After close** - RSS after `epoch_close()` (should drop 90%+)
- **Reclamation ratio** - Baseline / After close (target: >20×)

---

## Configuration Options

### Compile-Time Flags

```bash
# Enable RSS reclamation (default: disabled)
-DENABLE_RSS_RECLAMATION=1

# Enable probabilistic slowpath sampling for tail latency diagnosis (default: disabled)
# Samples 1/1024 allocations, measures wall vs CPU time to detect WSL2/VM interference
# See workloads/README_SLOWPATH_SAMPLING.md for usage
-DENABLE_SLOWPATH_SAMPLING

# Change page size (default: 4096)
-DSLAB_PAGE_SIZE=8192

# Adjust epoch count (default: 128, must be power of 2)
-DEPOCH_COUNT=256
```

### Runtime Tunables

**Slab cache capacity** (hard-coded in `allocator_init()`):
```c
a->classes[i].cache_capacity = 32;  // 32 slabs per size class
```

Increase for workloads with high slab churn (more memory, better reuse).

**Size classes** (hard-coded array):
```c
const uint32_t k_size_classes[] = {64, 96, 128, 192, 256, 384, 512, 768};
```

Tune for workload-specific allocation patterns (more classes = less internal fragmentation).

---

## Advanced Usage

### Pattern: Web Server Request Handling

```c
void handle_request(SlabAllocator* alloc, Request* req) {
    epoch_domain_t* request = epoch_domain_create(alloc);
    epoch_domain_enter(request);
    {
        char* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
        char* response = slab_malloc_epoch(alloc, 4096, request->epoch_id);
        
        // Process request, build response...
        
        send_response(response);
        // No individual frees needed
    }
    epoch_domain_exit(request);  // All request memory reclaimed
    epoch_domain_destroy(request);
}
```

**Result**: 1 allocation per request, deterministic reclamation on response sent.

### Pattern: Game Engine Frame Loop

```c
epoch_advance(alloc);
EpochId frame_epoch = epoch_current(alloc);
epoch_domain_t* frame = epoch_domain_wrap(alloc, frame_epoch, false);

while (running) {
    epoch_domain_enter(frame);
    {
        Particle* particles = slab_malloc_epoch(alloc, 256, frame_epoch);
        RenderBatch* batch = slab_malloc_epoch(alloc, 512, frame_epoch);
        
        render_frame(particles, batch);
    }
    epoch_domain_exit(frame);  // Deterministic frame cleanup
}

epoch_domain_destroy(frame);
```

**Result**: Frame-local allocations automatically reclaimed, no per-object free overhead.

### Pattern: Batch Transaction Processing

```c
epoch_domain_t* batch = epoch_domain_create(alloc);
epoch_domain_enter(batch);
{
    for (int i = 0; i < 10000; i++) {
        Transaction* txn = slab_malloc_epoch(alloc, 256, batch->epoch_id);
        process_transaction(txn);
    }
}
epoch_domain_exit(batch);  // Batch committed, all memory reclaimed
epoch_domain_destroy(batch);
```

**Result**: Bulk deallocation without per-transaction free() calls.

---

## Future Roadmap

### Planned Features

1. **ZNS SSD Integration** - Allocate zones as slabs (eliminate page cache)
2. **Multi-tenancy** - Per-tenant epoch pools (QoS isolation)
3. **Adaptive size classes** - Runtime tuning based on allocation histogram
4. **NUMA awareness** - Per-socket slab pools (reduce cross-socket traffic)

### Research Questions

1. **Object migration**: Can we compact partially-filled slabs during idle periods?
2. **Epoch GC**: Automatic epoch rotation based on memory pressure?
3. **Hybrid allocation**: ZNS for hot epochs, DRAM for cold epochs?

---

## References

### Academic Foundations

- **Slab allocation**: Bonwick (1994) - "The Slab Allocator: An Object-Caching Kernel Memory Allocator"
- **Epoch-based reclamation**: Fraser (2004) - "Practical Lock-Freedom"
- **Temporal allocation**: Appel (1989) - "Simple Generational Garbage Collection"

### Practical Inspirations

- **jemalloc**: Arena-based allocation, per-thread caching
- **tcmalloc**: Central cache, size class design
- **mimalloc**: Deferred coalescing, segment structure

### Novel Contributions

1. **Epoch-aligned RSS reclamation** - Explicit application boundaries control physical memory
2. **Lock-free epoch-partitioned slabs** - Temporal isolation without cross-thread interference
3. **Portable ABA-safe handles** - Registry-based validation works on all platforms
4. **RAII epoch domains** - Zero-cost abstraction over manual epoch management

---

### 9. Comprehensive Benchmark Validation (GitHub Actions CI)

**Problem Solved**: Performance claims need reproducible validation in controlled environments.

**Our Solution**: Multi-metric GitHub Actions workflow tracking 4 dimensions of allocator behavior.

#### Implemented Metrics (Phases 1-3, 5)

##### Phase 1: Variance Statistics
Measures latency predictability across trials:
- **IQR (Interquartile Range)**: p75 - p25 spread of p99/p999 latencies
- **CoV% (Coefficient of Variation)**: (stddev / mean) × 100 for relative variability

**Value**: Lower CoV% = more predictable latency. Validates temporal-slab's "variance elimination" claim.

```
=== Variance Analysis ===
temporal-slab p99:  CoV=2.35%, IQR=45 ns
system_malloc p99:  CoV=5.12%, IQR=187 ns
```

##### Phase 2: Peak RSS Tracking
Captures maximum memory pressure during benchmark runs:
- **Peak RSS (KB)**: Absolute maximum resident set size
- **Peak growth %**: Growth from baseline to peak

**Value**: Reveals retention behavior. Gap between peak and final RSS shows how much memory allocator holds after working set shrinks.

**JSON schema extension**:
```json
"rss": {
  "steady_state_kb": 1936,
  "peak_kb": 2464,
  "final_kb": 2464,
  "growth_pct": 27.27,
  "peak_growth_pct": 27.27
}
```

##### Phase 3: CPU Time Per Operation
Workflow-level wrapper captures total CPU consumption:
- **Formula**: `(user_time + sys_time) × 1e9 / total_operations`
- **Implementation**: `/usr/bin/time -v` wraps benchmark runs

**Value**: Throughput validation. Shows computational cost per alloc+free pair (includes all overhead: timing loops, percentile calculation, RSS measurement).

```
=== CPU Time Analysis ===
Metric              | temporal_slab          | system_malloc          |
--------------------|------------------------|------------------------|
CPU ns/op (median)  |    45.2 ns            |    38.7 ns            |
```

##### Phase 5: Regression Thresholds (Warn-Only)
Automated validation against expected ranges:
- temporal-slab p99 < 200ns
- temporal-slab p999 < 500ns
- temporal-slab p99 CoV < 10%
- p99 advantage > 5× vs malloc
- Peak RSS < 50MB for standard workload

**Value**: Prevents performance regressions. Warns (does not fail build) when metrics fall outside validated ranges.

#### Phase Shift Retention Benchmark

**Validates**: The "restart to reclaim" operational story.

**Pattern**: Burst (cycles 0-4) + Cooldown (cycle 5+)
- Build working set with 3 phases × objects + 40% pinned for fragmentation
- Free all pinned objects at cycle 5 (working set drops)
- Continue with reduced working set

**Validated Results** (GitHub Actions, ubuntu-latest, 20 cycles):
- **temporal-slab**: -71.9% retention (RSS: 9,796 KB → 2,756 KB)
- **system_malloc**: -18.6% retention (RSS: 11,392 KB → 9,268 KB)
- **Gap**: 53.3 percentage points

**What this proves**: General allocators retain memory across phase shifts due to fragmentation. temporal-slab provides explicit reclamation at phase boundaries via `epoch_close()`.

#### Recycle Efficiency Validation

**Validates**: Epoch ring buffer reuse without cache overhead.

**Key Finding**: **Zero cache traffic** when workload fits within 16-epoch ring window.

**Why this matters**: 
- PerfCounters track explicit cache operations (`empty_slab_recycled`, `empty_slab_overflowed`)
- Zero cache ops = slabs reused directly via epoch ring hot path
- No cache push/pop overhead when ring buffer suffices

**Technical insight**: Ring buffer design enables reuse at epoch N+16 without touching slab cache. This validates the design claim: "epoch ring handles steady-state reuse on the hot path."

#### Zero Hot Path Impact

All 4 implemented metrics share a critical property: **No code changes to allocation loops**.

- **Variance stats**: jq + bc calculations in workflow
- **Peak RSS**: Tracked outside allocation hot path (cycle-level measurement)
- **CPU time**: `/usr/bin/time -v` wrapper (process-level measurement)
- **Regression thresholds**: Post-execution validation

**Value**: Immediate credibility without risk of introducing performance regressions.

#### Future Work (Phase 4 - Deferred)

**Internal Counters Export**: Requires BenchAlloc interface extension to expose:
- CAS retry rate per allocation
- Scan mode switches (sequential vs randomized)
- Lock contention metrics

**Deferred rationale**: Requires code changes and interface redesign. Phases 1-3, 5 deliver 80% of value with zero risk.

---

## Getting Started

See [README.md](README.md) for build instructions and quick start guide.

See [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) for detailed performance measurements.

See [ARCHITECTURE.md](ARCHITECTURE.md) for in-depth design documentation.

See [EPOCH_DOMAINS.md](EPOCH_DOMAINS.md) for RAII pattern catalog and best practices.
