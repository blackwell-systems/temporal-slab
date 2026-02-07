# Changelog

All notable changes to the ZNS-Slab allocator project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Phase 2.0: Observability System

#### Added
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

#### Changed
- Exposed `EpochLifecycleState` enum in public API (`include/slab_alloc.h`)
- Counter initialization in `allocator_init()` - all Phase 2.0 counters zeroed
- Instrumentation added at three key code points (minimal fast-path impact):
  - `alloc_obj_epoch()` line 795 - epoch rejection attribution
  - `new_slab()` line 657 - cache miss attribution  
  - `cache_push()` lines 606-612 - madvise tracking

#### Performance
- **No regression**: p50: 31ns, p99: 1568ns, p999: 2818ns (unchanged)
- **Overhead**: Five relaxed atomic increments on slow paths only
- **Snapshot cost**: O(classes × epochs) = O(128) iterations + brief locks (~100µs)

#### Documentation
- `docs/OBSERVABILITY_DESIGN.md` - Phase 2.0/2.1/2.2 roadmap with diagnostic patterns
- `include/slab_stats.h` - Comprehensive API documentation with usage examples

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
