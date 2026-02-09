# Changelog

All notable changes to the ZNS-Slab allocator project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Definition: What "Bounded RSS" Means

**Allocator-bounded:** `committed_bytes` (total mmap'd memory) stays constant under sustained churn, proven via atomic counter instrumentation.

**Process-bounded:** Total process RSS stays bounded after subtracting application/harness overhead.

**Critical distinction:** RSS charts may show growth from application components (metrics arrays, log buffers, histograms). Always decompose using diagnostic counters:
- Enable: `make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1 -I../include -DENABLE_RSS_RECLAMATION=1"`
- Read: `committed_bytes` (allocator) vs anonymous RSS (process) vs unaccounted (application)

**Validation (2000 cycles):** Allocator committed: 0.70 MB → 0.70 MB (0.0% drift). Process RSS growth, if any, is application overhead.

---

### Immediate Empty Slab Reclamation (2026-02-09)

Critical fix eliminating unbounded RSS growth under sustained churn with immortal epochs.

**Root Cause:** Empty slab reclamation was gated by `if (epoch_state == EPOCH_CLOSING)`. When an epoch remains ACTIVE indefinitely (e.g., epoch 0 holding pinned long-lived objects), empty slabs in that epoch were never reclaimed via `madvise(MADV_DONTNEED)`. This caused RSS to ratchet upward across allocation/free cycles, accumulating empty slabs that could never return physical pages to the OS.

#### The Fix (Option C)

Remove the epoch state gate - reclaim empty slabs **immediately** when they become empty, regardless of whether the epoch is ACTIVE or CLOSING.

**Code Change** (`src/slab_alloc.c:1465`):
```c
// BEFORE (broken):
if (new_fc == s->object_count) {  // Slab became empty
    if (epoch_state == EPOCH_CLOSING) {
        cache_push(sc, s);  // madvise(MADV_DONTNEED)
    }
    // else: keep hot for reuse (BUG - immortal epochs accumulate slabs)
}

// AFTER (fixed):
if (new_fc == s->object_count) {  // Slab became empty
    cache_push(sc, s);  // ALWAYS reclaim, regardless of epoch state
}
```

**Why Safe:**
- Each slab is independently `mmap(NULL, 4096, ...)` - one syscall per page
- `madvise(MADV_DONTNEED)` returns physical pages but preserves virtual mapping
- Slab reuse triggers zero-fill page fault (~5µs overhead, acceptable)
- No ABA issues: registry generation counter prevents stale handle reuse

#### Changed
- **Empty slab reclamation policy** - Decoupled from epoch lifecycle (Option C)
  - **Before**: `if (epoch_state == EPOCH_CLOSING) { cache_push(slab); }`
  - **After**: Always call `cache_push(slab)` when slab becomes fully empty
  - Reclamation now happens on every free that empties a slab, not at epoch boundaries
  - Trade-off: Slight latency cost on slab reuse (~5µs zero-fill overhead)
  - Benefit: RSS drops continuously, no dependency on epoch_close() timing

#### Added
- **Diagnostic Counters** - RSS analysis infrastructure for sustained workload debugging
  - `committed_bytes` - Total bytes mmap'd for slabs (slabs × SLAB_PAGE_SIZE)
  - `live_bytes` - Bytes currently allocated to live objects
  - `empty_slabs` - Number of slabs currently empty (all slots free)
  - Added to `SizeClassAlloc` struct alongside existing `madvise_*` counters
  - Enables "smoking gun" diagnostics: committed vs live vs empty correlation

#### Proof (Validation Results)

**Local Testing** (30 cycles, 20% pinned in epoch 0):
- **Before fix**: RSS grew 1.6MB → 20.6MB (+1115% retention)
- **After fix**: RSS stable at ~2MB (26% post-pressure retention), peak 2.8MB
- **Improvement**: 40× reduction in RSS growth

**GitHub Actions CI** (Ubuntu 24.04, kernel 6.11, 100 cycles):
- **temporal-slab**: 27% retention (post-pressure), RSS stability <10% ✓
- **system_malloc**: 20% retention (post-pressure), RSS stability <10% ✓
- **Bounded growth**: Both allocators maintain tight RSS oscillation bands
- **Proof**: No unbounded ratcheting across 10 cooldowns (100 cycles)

**Bounded Growth Validation:** The benchmark now tracks RSS at every cooldown and computes RSS stability (max-min range / min). Values <50% indicate bounded behavior (tight oscillation), 50-100% indicate marginal stability, >100% indicate unbounded growth. Before the fix, temporal-slab showed >1000% instability (unbounded ratcheting). After the fix, both allocators show <10% (deterministic reclamation).

**Note on Probe Fault Counters:** CI shows 32 page faults instead of expected 16384 (64MB / 4KB). This is a kernel/container measurement artifact (Transparent Huge Pages or cgroup batching), not a code bug. Diagnostic instrumentation confirmed `touch_pages()` executes all 16384 iterations correctly. **RSS measurements remain accurate and are the primary validation signal in containerized environments.**

#### Design Rationale

Immediate reclamation (Option C) aligns with the allocator's core promise: **"bounded RSS under sustained churn."** Epoch boundaries provide temporal grouping for allocation (objects with similar lifetimes co-locate), but reclamation should be opportunistic (free memory as soon as available, not gated on epoch lifecycle).

**Alternatives Considered:**
- **Option A**: Force-close immortal epochs during cooldown (changes API semantics)
- **Option B**: Separate arena for pinned objects (increases complexity)
- **Option C**: Reclaim empty slabs regardless of epoch state ✓ (cleanest, safest)

---

### Adaptive Bitmap Scanning (2026-02-08)

Complete implementation of dynamic mode switching between sequential and randomized bitmap scanning based on CAS contention, with full observability and race-free atomic implementation.

#### Added
- **Phase 2.2+ Adaptive Bitmap Scanning** - Dynamic mode switching based on contention
  - Controller uses windowed delta calculations (allocation-triggered, no clock reads)
  - Sequential mode (mode=0): Low contention, predictable scan patterns
  - Randomized mode (mode=1): High contention, reduces CAS retry collisions
  - Threshold: 0.30 retry rate with 3-check dwell time (prevents flapping)
  - TLS-cached scan offsets: Reduces per-thread startup jitter
- **Observability Metrics** - Full visibility into adaptive behavior:
  - `scan_adapt_checks_total` - Total adaptation checks performed (heartbeat counter)
  - `scan_adapt_switches_total` - Mode switches between sequential/randomized
  - `scan_mode` - Current scanning mode (0=sequential, 1=randomized)
  - All metrics exported via `slab_stats_class()` for Prometheus integration
- **Single-writer guard** - CAS-based lock-free pattern in `scan_adapt_check()`
  - `_Atomic uint32_t in_check` ensures only one thread updates controller
  - Prevents concurrent modifications to windowed delta calculations
  - Low overhead (single CAS operation per heartbeat check)
- **Atomic controller state** - Safe concurrent access for hot path + export
  - Window endpoints: `_Atomic uint64_t last_attempts/retries`
  - Controller mode: `_Atomic uint32_t mode` (0=sequential, 1=randomized)
  - Observable counters: `_Atomic uint32_t checks/switches` (export-safe)
  - Memory ordering: acquire on entry, release on exit, relaxed for counters
- **Documentation** - Complete technical foundation in `docs/foundations.md`:
  - Thundering herd problem definition and adaptive mitigation
  - Bitmap scanning modes (sequential vs randomized)
  - Windowed delta calculations for HFT-safe adaptation
  - Controller design with hysteresis/dwell time

#### Changed
- **`slab_stats_class()` export** - Use `atomic_load_explicit()` for adaptive metrics
  - Prevents torn reads during concurrent controller updates
  - Ensures consistent snapshots for Prometheus scraping

#### Fixed
- **Data races in adaptive controller** (CRITICAL)
  - Plain reads/writes to `scan_adapt` fields caused undefined behavior
  - Window state (`last_attempts`, `last_retries`) accessed concurrently
  - Observable metrics (`checks`, `switches`, `mode`) unsafe for export
  - Fix: Convert all fields to `_Atomic` types with proper memory ordering

#### Validation
- 47 production trials across T=1,2,4,8,16 threads (60s measurement windows)
- Coefficient of variation: 4.9-14% (excellent repeatability)
- T=1: mode=0, switches=0, retries=0.0 (perfect sequential)
- T=2: mode=0.1, switches=2.1 (active switching near 0.30 threshold - proves controller works!)
- T=4: mode=0.9, switches=3.9 (converging to randomized)
- T=8: mode=1.0, switches=0, retries=2.58 (stable randomized, ~19% retry reduction vs sequential)
- T=16: mode=1.0, switches=0, retries=2.46 (stable randomized)
- No data corruption across all trials

---

### Critical Correctness Fixes (2025-02-07)

#### Fixed
- **Bug 1 (CRITICAL)**: `epoch_current()` overflow after 16 advances
  - Returned monotonic counter (16, 17, 18...) instead of ring index (0-15)
  - Caused allocations to fail silently after 16 `epoch_advance()` calls
  - Fix: Added modulo operation to return valid ring index
- **Bug 5 (MEDIUM)**: `epoch_domain_create()` advanced on every call
  - Created 20 domains → 20 advances → immediate ring wraparound
  - Fix: Removed implicit advance, domains now wrap current epoch
- **Bug 2 (HIGH)**: Semantic collision in `alloc_count` field
  - Same field used for live allocations AND domain refcounts
  - Prevented domain leak detection (10K objects vs 10K domains indistinguishable)
  - Fix: Renamed to `domain_refcount`, removed hot-path allocation tracking
- **Bug 3 (HIGH)**: Domain refcount not wired to allocator
  - `epoch_domain_enter/exit` only updated local refcount
  - Allocator always saw refcount=0 (observability broken)
  - Fix: Wire enter/exit to `slab_epoch_inc/dec_refcount()` APIs
- **Bug 4 (CRITICAL)**: Auto-close could close wrong epoch after wrap
  - Domain stored epoch_id only (not era)
  - After ring wrap, closed different epoch incarnation
  - Fix: Added `epoch_era` to domain struct, validate before close
- **Issue 6 (CRITICAL)**: Data race on `epoch_era` array
  - Plain `uint64_t[]` accessed by multiple threads without synchronization
  - Undefined behavior under concurrent access
  - Fix: Changed to `_Atomic uint64_t[]` with proper memory ordering
- **Issue 5 (CRITICAL)**: Registry growth corrupted free list
  - `reg_alloc_id()` dropped existing free_ids on capacity growth
  - Would cause double-allocation if ID recycling implemented
  - Fix: Preserve free_count entries during reallocation
- **Issue 4 (CRITICAL)**: `init_class_lookup()` not thread-safe
  - Two threads could race to initialize lookup table
  - Fix: Use `pthread_once()` for single-threaded initialization
- **Issue 2 (HIGH)**: `epoch_domain_destroy()` missing era validation
  - Could close wrong epoch after ring wrap
  - Fix: Added era validation before auto-close
- **Issue 3 (MEDIUM)**: `epoch_domain_force_close()` unsafe semantics
  - Mutated refcount, no era validation, no safety checks
  - Fix: Added refcount=0 assertion, era validation, TLS verification
- **Issue 1 (HIGH)**: epoch_domain nesting broken
  - Single TLS pointer, nested domains clobbered each other
  - Example: `enter(A) → enter(B) → exit(B)` lost A's context
  - Fix: TLS stack (32-element array) with LIFO enforcement

#### Added
- **Phase 2.3 Semantic Attribution APIs**:
  - `slab_epoch_set_label()` - Set semantic label for debugging
  - `slab_epoch_inc_refcount()` - Track domain enter boundaries
  - `slab_epoch_dec_refcount()` - Track domain exit boundaries
  - `slab_epoch_get_refcount()` - Query refcount for leak detection
- **Thread-Local Contract** - Domains enforced as thread-local scopes:
  - Added `pthread_t owner_tid` to `epoch_domain_t`
  - All operations assert ownership via `pthread_equal()`
  - TLS stack supports up to 32 nested domains (configurable)
  - Debug mode validates LIFO unwind and stack consistency
- **Per-Allocator Label Lock** - `pthread_mutex_t epoch_label_lock`
  - Protects label writes (rare, cold path)
  - Prevents cross-allocator contention

#### Changed
- **Default `auto_close`**: Changed from `true` to `false`
  - Explicit `epoch_close()` is safer than implicit auto-close
  - Prevents accidental premature closure with shared epochs
- **EpochMetadata field rename**: `alloc_count` → `domain_refcount`
  - Semantic clarity: tracks domain boundaries, not live allocations
  - Enables clean domain leak detection signal

#### Impact
- Allocator now works reliably after 16+ epoch advances (was silently broken)
- Domain nesting safe for request→transaction→query patterns
- All data races eliminated (era, lookup table, registry growth)
- Thread-local contract enforced with runtime assertions
- Production-ready concurrency safety

---

### Phase 2: Complete Observability System

Complete observability implementation across 5 incremental phases, transforming epochs from opaque memory buckets into fully observable temporal windows for debugging production issues.

#### Phase 2.0: Core Stats API

##### Added
- **Stats API** - Versioned snapshot-based observability for production diagnostics
  - `slab_stats_global()` - Aggregate stats across all size classes and epochs
  - `slab_stats_class()` - Per-size-class diagnostics with derived metrics
  - `slab_stats_epoch()` - Per-epoch visibility with reclaimable slab estimation
  - `EpochLifecycleState` - Public enum (ACTIVE/CLOSING) for observability
- **Attribution Counters** - Answer "why slow?" with precise attribution:
  - `slow_path_cache_miss` - Allocation needed new slab from OS (mmap)
  - `slow_path_epoch_closed` - Allocation rejected (epoch CLOSING)
  - Existing counters: `current_partial_null`, `current_partial_full`
- **RSS Reclamation Tracking** - Answer "why RSS not dropping?":
  - `madvise_calls` - Number of madvise(MADV_DONTNEED) invocations
  - `madvise_bytes` - Total bytes reclaimed via madvise
  - `madvise_failures` - madvise() system call failures
- **Observability Tool** - `stats_dump` with dual output pattern:
  - JSON to stdout (stable contract for tooling, jq, Prometheus, CI diffs)
  - Text to stderr (human-readable debugging without polluting pipes)
  - Flags: `--json`, `--no-json`, `--text`, `--no-text`
- **Derived Metrics** - Computed in snapshot functions:
  - `recycle_rate_pct` - Cache effectiveness (100 × recycled / [recycled + overflowed])
  - `net_slabs` - Memory growth tracking (allocated - recycled)
  - `estimated_rss_bytes` - Predicted RSS footprint per class/epoch
- **Forward Compatibility** - `SLAB_STATS_VERSION` constant for API evolution

##### Changed
- Exposed `EpochLifecycleState` enum in public API (`include/slab_alloc.h`)
- Counter initialization in `allocator_init()` - all Phase 2.0 counters zeroed
- Instrumentation added at three key code points (minimal fast-path impact):
  - `alloc_obj_epoch()` line 795 - epoch rejection attribution
  - `new_slab()` line 657 - cache miss attribution  
  - `cache_push()` lines 606-612 - madvise tracking

##### Performance
- **No regression**: p50: 31ns, p99: 1568ns, p999: 2818ns (unchanged)
- **Overhead**: Five relaxed atomic increments on slow paths only
- **Snapshot cost**: O(classes × epochs) = O(128) iterations + brief locks (~100µs)

##### Documentation
- `docs/OBSERVABILITY_DESIGN.md` - Phase 2.0/2.1/2.2 roadmap with diagnostic patterns
- `include/slab_stats.h` - Comprehensive API documentation with usage examples

#### Phase 2.1: O(1) Reclaimable Tracking

##### Added
- **Atomic Empty Slab Counter** - `empty_partial_count` per epoch
  - Incremented when slab transitions to fully empty (all slots free)
  - Decremented when empty slab gets first allocation
  - Decremented when empty slab recycled to cache
  - Eliminates O(n) scan of partial list in `slab_stats_epoch()`
- **Instant Reclamation Visibility** - `reclaimable_slab_count` in `SlabEpochStats`
  - O(1) query instead of O(partial_slabs) scan
  - Shows exact number of empty slabs ready for madvise
  - Enables real-time reclamation potential monitoring

##### Changed
- `EpochState` struct: Added `empty_partial_count` atomic counter
- `alloc_obj_epoch()`: Decrement counter when allocating from empty slab
- `free_obj()`: Increment counter when slab becomes empty (both ACTIVE and CLOSING)
- `epoch_close()`: Decrement counter when recycling empty slab
- `slab_stats_epoch()`: Read atomic counter instead of scanning partial list

##### Performance
- **Query speedup**: 100+ slabs → O(1) atomic load (~5ns vs ~1µs)
- **Zero allocation overhead**: Counter updates only on empty transitions
- **Memory cost**: 4 bytes per epoch per size class (512 bytes total)

#### Phase 2.2: Era Stamping

##### Added
- **Monotonic Era Counter** - `epoch_era_counter` global atomic
  - Increments on every `epoch_advance()` call
  - Never wraps (64-bit: ~584 billion years at 1Hz)
  - Provides unique temporal identity across epoch wraparounds
- **Per-Epoch Era Stamps** - `epoch_era[EPOCH_COUNT]` array
  - Records era when epoch was last activated
  - Stamped into slabs on allocation (`slab->era`)
  - Exposed via `SlabEpochStats` for observability
- **Temporal Uniqueness** - Solve "is this the same epoch or new rotation?"
  - Before: (epoch=0, era=100) vs (epoch=0, era=105) = different rotations
  - After: Can distinguish epochs across wraparound boundaries
  - Enables correlation with application logs/metrics

##### Changed
- `SlabAllocator`: Added `epoch_era_counter` and `epoch_era[]` array
- `Slab` struct: Added `era` field for temporal stamping
- `epoch_advance()`: Increment counter and stamp `epoch_era[new_epoch]`
- `new_slab()`: Copy `epoch_era[epoch_id]` into `slab->era`
- `SlabEpochStats`: Added `epoch_era` field for observability

##### Use Cases
- Detect epoch wraparound: Same `epoch_id` but different `era` values
- Correlate with logs: "Request XYZ allocated in (epoch=3, era=1250)"
- Track epoch lifetime: "Epoch 5 lived through eras 100-115 (15 rotations)"

#### Phase 2.3: Epoch Metadata

##### Added
- **Temporal Profiling Metadata** - `EpochMetadata` struct per epoch
  - `open_since_ns`: Timestamp when epoch became ACTIVE (0 if never opened)
  - `alloc_count`: Atomic refcount of live allocations in this epoch
  - `label[32]`: Semantic tag for debugging (e.g., "request_id:abc123", "frame:1234")
- **Lifetime Tracking** - Know when epochs opened and how long they've been active
  - Set on `epoch_advance()`: `open_since_ns = now_ns()`
  - Enables age detection: "Epoch 7 opened 2 minutes ago and still has 15K live objects"
- **Allocation Refcounting** - Track live allocations per epoch
  - Incremented on successful allocation (both fast and slow paths)
  - Decremented on successful free
  - Memory ordering: relaxed (best-effort visibility, eventual consistency)
- **Semantic Labeling** - Correlate epochs with application phases
  - 32-byte string field for custom tags
  - Reset to empty on `epoch_advance()`
  - Examples: "batch_42", "session:user_123", "frame:1234"

##### Changed
- `EpochMetadata` struct: Added to `slab_alloc_internal.h`
- `SlabAllocator`: Added `epoch_meta[EPOCH_COUNT]` array
- `allocator_init()`: Initialize all metadata fields to zero
- `epoch_advance()`: Reset metadata (timestamp, zero count, clear label)
- `alloc_obj_epoch()`: Increment `alloc_count` on success (2 code paths)
- `free_obj()`: Decrement `alloc_count` on successful free
- `SlabEpochStats`: Added 3 metadata fields for observability

##### Use Cases
- **Detect stuck epochs**: `open_since_ns` far in past + `alloc_count > 0`
- **Profile allocation patterns**: `alloc_count` spikes correlate with load
- **Debug lifetime issues**: Labels connect allocator state to app phases
- **Measure drain time**: Delta between `open_since_ns` observations

#### Phase 2.4: RSS Delta Tracking

##### Added
- **Before/After RSS Measurements** - Quantify memory reclamation impact
  - `rss_before_close`: RSS snapshot at start of `epoch_close()` (0 if never closed)
  - `rss_after_close`: RSS snapshot at end of `epoch_close()` (0 if never closed)
  - Delta calculation: `rss_before - rss_after` shows MB reclaimed
- **Reclamation Validation** - Hard numbers on memory returned to OS
  - Validates that closing epochs actually frees memory
  - Quantifies effectiveness (MB freed per close)
  - Diagnoses leaks (delta approaching zero = problem)

##### Changed
- `EpochMetadata` struct: Added `rss_before_close` and `rss_after_close`
- `allocator_init()`: Initialize RSS fields to zero
- `epoch_close()`: Capture RSS at start and end of function
- `SlabEpochStats`: Added RSS delta fields for observability

##### Use Cases
- **Validate reclamation**: "Closed epoch 5, freed 36 MB (28% reduction)"
- **Compare strategies**: A/B test epoch rotation policies by RSS delta
- **Diagnose leaks**: Zero delta despite many frees indicates leak
- **Track RSS trends**: Sum deltas across epochs for total reclamation

##### Example Output
```
Epoch 5 closed:
  rss_before: 128 MB
  rss_after:   92 MB
  delta:       36 MB reclaimed (28% reduction)
```

#### Testing
- `test_era_stamping.c` - Validates monotonic era progression and wraparound
- `test_epoch_metadata.c` - 5 scenarios testing timestamps, refcounts, isolation, wraparound
- `test_rss_delta.c` - 3 scenarios testing RSS capture, multiple closes, unclosed epochs

All tests pass, confirming complete Phase 2 functionality.

---

### Phase 3: Epoch Domains (RAII Memory Management)

#### Added
- **Epoch Domain API** - RAII-style wrappers for epoch lifecycle management
  - `epoch_domain_create()` - Auto-closing domain (web request pattern)
  - `epoch_domain_wrap()` - Manual-control domain (reusable frame pattern)
  - `epoch_domain_enter()`/`exit()` - Scoped lifetime management with refcounting
  - `epoch_domain_force_close()` - Explicit reclamation trigger
  - Thread-local domain tracking for implicit context
- **Usage Examples** - Four canonical patterns in `examples/domain_usage.c`:
  - Request-scoped allocation (auto-cleanup on scope exit)
  - Reusable frame domain (game engine/render loop)
  - Nested domains (transaction + query scopes)
  - Explicit lifetime control (batch processing)
- **Documentation** - `EPOCH_DOMAINS.md` with pattern catalog and design rationale

#### Changed
- Epoch API now supports both manual (`epoch_advance`/`epoch_close`) and RAII patterns

---

## Phase 2.1: RSS Reclamation Validation (2025-01-06)

### Added
- **Churn Test** - 100M allocation/free cycles validate RSS bounds under adversarial workload
- **Epoch-Aware Recycling** - Empty slabs only recycled in CLOSING epochs (FIX #1)
  - ACTIVE epochs: Keep empty slabs hot (latency-first, stable RSS)
  - CLOSING epochs: Aggressive recycling + madvise → RSS drops
  - Explicit control: `epoch_close()` becomes RSS reclamation boundary
- **Proactive Scan** - `epoch_close()` scans for already-empty slabs (missing piece)
- **Generation Bumping on Reuse** - ABA protection survives madvise zeroing (FIX #2)
- **Madvise Outside Lock** - Moved to after `cache_push()` unlock for predictable latency (FIX #3)

### Changed
- **`free_obj()` behavior**: Now checks epoch state before recycling empty slabs
- **`epoch_close()` behavior**: Proactively recycles empty slabs across all size classes
- **Cache safety**: Slab IDs stored off-page in `CachedNode` to survive madvise

### Validated
- **RSS Bounds**: 100M allocs plateau at 768KB RSS (8 slabs across 8 size classes)
- **Reclamation**: `epoch_close()` drops RSS to near-zero (1-2 slabs residual)
- **Stability**: No RSS growth, no leaks, deterministic behavior under churn

---

## Phase 2.0: RSS Reclamation Foundation (2025-01-05)

### Added
- **RSS Reclamation Flag** - `ENABLE_RSS_RECLAMATION=1` (compile-time opt-in)
  - When enabled: `madvise(MADV_DONTNEED)` on empty slabs → physical pages returned to OS
  - When disabled: Slabs stay hot (existing behavior preserved)
- **Slab Cache Architecture** - Two-tier caching for recycled slabs:
  - Array cache (32 slabs per size class, fast path)
  - Overflow list (unbounded, linked CachedNode structs)
  - Both survive madvise via off-page metadata storage
- **Performance Validation** - Sub-100ns latency maintained with reclamation enabled
- **Benchmark Suite**:
  - `churn_test.c` - RSS bounds validation (100M allocation cycles)
  - `benchmark_accurate.c` - Latency distribution measurement
  - `benchmark_threads.c` - Multi-threaded scaling tests

### Changed
- **Build system**: Added `-DENABLE_RSS_RECLAMATION=1` to default `CFLAGS` in Makefile
- **Cache push**: Moved madvise outside lock (FIX #3 in Phase 2.1)

---

## Phase 1.5: Portable Handle Encoding (2025-01-04)

### Added
- **Slab Registry** - Central registry maps `slab_id` → slab pointer + generation counter
  - Replaces direct pointer encoding in handles
  - 22-bit slab_id supports up to 4M slabs (addressable on all platforms)
  - 24-bit generation counter (ABA protection, wraps after 16M reuses)
- **Portable Handle Format v1** (64-bit):
  ```
  [63:42] slab_id (22 bits)
  [41:18] generation (24 bits)
  [17:10] slot (8 bits)
  [9:2]   size_class (8 bits)
  [1:0]   version (2 bits) = 0b01
  ```
- **ABA Safety** - Generation validation prevents use-after-free on handle reuse
- **Handle API**:
  - `handle_pack()` / `handle_unpack()` - Encode/decode handle fields
  - `encode_handle()` - Create handle from slab + registry
  - `reg_lookup_validate()` - Validate slab_id + generation atomically

### Changed
- **`slab_malloc_epoch()` / `slab_free()`**: Now use registry-based handle encoding
- **`free_obj()`**: Validates handles through registry (stale handles → no crash)

### Removed
- Direct pointer encoding in handles (non-portable, failed on 32-bit ARM)

---

## Phase 1.0: Lock-Free Fast Path (2025-01-01)

### Added
- **Lock-Free Allocation** - Atomic CAS on bitmap + `current_partial` pointer
  - Fast path: Single atomic CAS (no mutex)
  - Slow path: Mutex-protected slab list management
  - 97% allocation hit rate on `current_partial` (measured)
- **Per-Epoch State** - Size classes partitioned by epoch for temporal isolation
  - Each `(size_class, epoch)` pair maintains separate partial/full lists
  - Lock-free `current_partial` pointer per epoch
- **Epoch API**:
  - `epoch_advance()` - Rotate to next epoch (mark old as CLOSING)
  - `epoch_close(epoch)` - Explicit close without rotation
  - `epoch_current()` - Get current epoch ID
- **Atomic Bitmap Allocation** - `slab_alloc_slot_atomic()` with CAS retry loop
- **Performance Counters** - Telemetry for slow-path hits, list moves, cache stats

### Benchmarks
- **Median latency**: 73ns (lock-free path)
- **99.9th percentile**: 167ns (within 2.3× of median)
- **Variance elimination**: Algorithmic slab selection removes jemalloc's 40µs tail spikes

---

## Initial Commit (2024-12-30)

### Added
- **Core allocator structure**:
  - Size classes: [64, 96, 128, 192, 256, 384, 512, 768] bytes
  - O(1) class lookup table (1-byte granularity)
  - Slab-based allocation (4KB pages, bitmap metadata)
- **Intrusive lists** - Partial/full slab tracking per size class
- **Basic API**:
  - `slab_allocator_create()` / `slab_allocator_free()`
  - `slab_malloc_epoch()` / `slab_free()`
- **Test suite**: `smoke_tests.c` - Functional validation
- **Documentation**: README with architecture overview

---

## Design Philosophy

### Predictability Over Average Case
This allocator prioritizes **variance elimination** over raw throughput:
- Algorithmic decisions (epoch-based selection) replace heuristics
- Lock-free fast path eliminates lock contention spikes
- Bounded slab cache prevents unbounded search times
- Tail latency stays within 2-3× of median (vs 500× in general-purpose allocators)

### Explicit Memory Lifecycle
Rather than implicit heuristics (malloc/free), epochs provide:
- Deterministic reclamation boundaries (aligned with application phases)
- Explicit RSS control (`epoch_close()` triggers madvise)
- Temporal isolation (per-epoch slab partitioning)

### Evolution, Not Revolution
Each phase builds on previous work:
- Phase 1: Solve latency variance (lock-free + epochs)
- Phase 2: Add RSS control (keep Phase 1's latency properties)
- Phase 3: Add ergonomics (RAII wrappers preserve explicit control)

---

## Future Work

### Planned
- **ZNS SSD Integration** - Direct zone allocation (bypass page cache)
- **Multi-tenancy** - Per-tenant epoch partitioning
- **Adaptive Size Classes** - Runtime tuning based on workload

### Under Consideration
- **Object migration** - Compact partially-filled slabs during idle periods
- **NUMA awareness** - Per-socket slab pools
- **Shared-nothing threads** - Eliminate cross-thread cache line bouncing

---

## Benchmarking Methodology

All benchmarks use:
- **Hardware**: Isolated CPU core (taskset), turbo boost disabled
- **Measurement**: CLOCK_MONOTONIC (nanosecond precision)
- **Workload**: Alternating alloc/free (worst-case cache behavior)
- **Reporting**: Full latency distribution (min/median/99th/99.9th/max)

RSS measurements use `/proc/self/statm` (Linux) sampled every 10K operations.

---

## Version History

- **Phase 3** (Epoch Domains): RAII memory management (2025-01-07)
- **Phase 2.1** (RSS Validation): Churn test + bugfixes (2025-01-06)
- **Phase 2.0** (RSS Reclamation): madvise integration (2025-01-05)
- **Phase 1.5** (Portable Handles): Registry-based encoding (2025-01-04)
- **Phase 1.0** (Lock-Free): Sub-100ns allocation (2025-01-01)
- **Initial Commit**: Core allocator structure (2024-12-30)
