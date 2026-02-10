# Observability Implementation Status

Current state of the Phase 2 observability roadmap and what's next.

**Last updated:** 2026-02-10

---

## Executive Summary

**Phase 2.0: COMPLETE** - Production-ready monitoring with external tooling  
**Phase 2.1: COMPLETE** - Epoch-close telemetry + era stamping  
**Phase 2.2: COMPLETE** - Multi-threading contention observability (HFT-safe)  
**Phase 2.3: COMPLETE** - Semantic attribution (labels, refcount, leak detection)  
**Phase 2.4: COMPLETE** - RSS delta tracking (kernel cooperation telemetry)  
**Phase 2.5: COMPLETE** - Probabilistic slowpath sampling (tail attribution)  
**Phase 2.6: COMPLETE** - TLS handle cache (high-locality optimization)  

**Current capability:** Full structural observability with Prometheus + Grafana dashboard, sub-1% overhead, root cause attribution for slow-path and RSS behavior, **HFT-safe contention tracking (zero jitter)**, semantic attribution for leak detection, **wait_ns scheduler interference metric**, and adaptive bitmap scanning for contention mitigation.

**Next milestone:** Comparative benchmarks vs malloc/jemalloc, or Phase 3 (kernel integration).

---

## Phase 2.0: Core Stats Infrastructure [COMPLETE]

### What Was Delivered

**Allocator implementation:**
- [COMPLETE] `SlabGlobalStats`, `SlabClassStats`, `SlabEpochStats` structs
- [COMPLETE] `slab_stats_global()`, `slab_stats_class()`, `slab_stats_epoch()` APIs
- [COMPLETE] Atomic counters with `memory_order_relaxed` (<1% overhead)
- [COMPLETE] Per-size-class slow-path attribution (cache_miss vs epoch_closed)
- [COMPLETE] RSS reclamation tracking (madvise calls/bytes/failures)
- [COMPLETE] `stats_dump` CLI tool with dual output (text + JSON)

**External tooling (temporal-slab-tools):**
- [COMPLETE] `tslab stats` - Human summary and JSON passthrough
- [COMPLETE] `tslab watch` - Interval sampling with delta/sec calculations
- [COMPLETE] `tslab top` - Rank classes by metrics (slow_path_hits, madvise_bytes, etc)
- [COMPLETE] `tslab export prometheus` - Prometheus text exposition format (40+ metrics)

**Production monitoring:**
- [COMPLETE] Grafana dashboard (14 panels) - `/temporal-slab-tools/dashboards/temporal-slab-observability.json`
- [COMPLETE] Complete deployment guide - `MONITORING_SETUP.md`
- [COMPLETE] Prometheus alert rules (slow-path rate, madvise failures, RSS growth)
- [COMPLETE] Textfile collector setup (systemd service + timer)

**Documentation:**
- [COMPLETE] `OBSERVABILITY_DESIGN.md` - Complete phase breakdown
- [COMPLETE] `stats_dump_reference.md` - Field-by-field JSON schema documentation
- [COMPLETE] `OBSERVABILITY_PHILOSOPHY.md` (tools repo) - Structural vs emergent observability
- [COMPLETE] `METRICS_PERFORMANCE.md` (tools repo) - Atomic counter performance analysis
- [COMPLETE] `ARM_PORTABILITY.md` - ARM64 atomics and memory ordering

**Testing:**
- [COMPLETE] `VERIFICATION.sh` - 8-test suite covering all commands
- [COMPLETE] Schema validation (JSON Schema Draft 2020-12)
- [COMPLETE] All tests passing on x86-64

---

## What Phase 2.0 Enables

**For SREs:**
- "RSS is growing" → Query Grafana → "Epoch 12 owns 45 MB and has been open 10 minutes"
- Diagnose slow-path spikes: cache undersized vs workload using stale epochs
- Validate kernel cooperation: madvise success rate, RSS drop correlation

**For Performance Engineers:**
- Identify hot size classes: `top classes by slow_path_hits`
- Quantify reclamation effectiveness: `madvise_bytes_total` vs `rss_bytes`
- Profile allocation patterns: slow_cache_miss (99%) vs slow_epoch_closed (1%)

**For Operators:**
- Always-on monitoring (<1% overhead)
- No heap profiler needed (metrics always exported)
- Root cause attribution (not just "allocator slow")

---

## Phase 2.1: Epoch-Close Telemetry + Era Stamping [COMPLETE]

### Status

**Implementation:** COMPLETE  
**Design:** Complete (OBSERVABILITY_DESIGN.md)  
**Blockers:** None  

### What It Adds

**New counters (per size class):**
```c
_Atomic uint64_t epoch_close_calls;          /* How many times epoch_close() called */
_Atomic uint64_t epoch_close_scanned_slabs;  /* Total slabs scanned for reclaimable */
_Atomic uint64_t epoch_close_recycled_slabs; /* Slabs actually recycled */
_Atomic uint64_t epoch_close_total_ns;       /* Total time spent in epoch_close() */
```

**New field (per epoch):**
```c
_Atomic uint32_t empty_partial_count;  /* O(1) reclaimable tracking */
```

### Why It Matters

**Current limitation:**
- Can see `madvise_calls` and `madvise_bytes`, but not **why** they happened
- Can't distinguish: "epoch_close scanned 100 slabs, found 10 reclaimable" vs "scanned 10, found 10"

**Phase 2.1 enables:**
- **Reclamation effectiveness:** `recycled_slabs / scanned_slabs` (efficiency %)
- **Epoch-close cost:** `epoch_close_total_ns / epoch_close_calls` (avg latency)
- **Heuristic validation:** Did scanning find anything? (vs blind madvise)

**Example diagnosis:**
```
epoch_close_calls: 100
epoch_close_scanned_slabs: 10,000
epoch_close_recycled_slabs: 50
```
**Interpretation:** "100 epoch closes scanned 10k slabs, but only 50 were reclaimable. Scanning is expensive (100 slabs/call) but low yield (0.5%)."

**Action:** Reduce scan frequency or improve epoch-close heuristic.

---

### Implementation Effort

**Estimated:** 2-3 hours

**Tasks:**
1. Add 4 atomic counters to `SizeClassAlloc`
2. Increment counters in `epoch_close()` hot path
3. Expose in `SlabClassStats` struct
4. Update `stats_dump` JSON output
5. Update `stats_dump_reference.md` documentation

**No external tool changes needed** (counters flow through existing Prometheus exporter).

---

## Phase 2.2: Multi-Threading Contention Observability [COMPLETE]

### Status

**Implementation:** COMPLETE (2026-02-07)  
**Tested:** COMPLETE (1→16 thread validation)  
**Blockers:** None  

### What Was Delivered

**Allocator implementation (11 new atomic counters):**
```c
// Per-class contention metrics
_Atomic uint64_t bitmap_alloc_cas_retries;      /* CAS retry loops during allocation */
_Atomic uint64_t bitmap_free_cas_retries;       /* CAS retry loops during free */
_Atomic uint64_t current_partial_cas_failures;  /* Pointer-swap CAS failures */
_Atomic uint64_t bitmap_alloc_attempts;         /* Successful allocations (denominator) */
_Atomic uint64_t bitmap_free_attempts;          /* Successful frees (denominator) */
_Atomic uint64_t current_partial_cas_attempts;  /* All CAS attempts (denominator) */
_Atomic uint64_t lock_fast_acquire;             /* Trylock succeeded (no blocking) */
_Atomic uint64_t lock_contended;                /* Trylock failed (had to block) */
```

**LOCK_WITH_PROBE macro (Tier 0 probe):**
```c
#define LOCK_WITH_PROBE(mutex, sc) do { \
  if (pthread_mutex_trylock(mutex) == 0) { \
    atomic_fetch_add(&(sc)->lock_fast_acquire, 1, memory_order_relaxed); \
  } else { \
    atomic_fetch_add(&(sc)->lock_contended, 1, memory_order_relaxed); \
    pthread_mutex_lock(mutex); \
  } \
} while (0)
```

**External tooling (temporal-slab-tools):**
- [COMPLETE] 9 Prometheus metrics (raw counters only, HFT-safe)
- [COMPLETE] 5 Grafana panels (lock contention + CAS retries)
- [COMPLETE] Divide-by-zero guards in all PromQL queries
- [COMPLETE] Single-line HELP text with HFT callouts

**Grafana dashboard (5 new panels, 24→29 total):**
- Lock Contention Rate: 5% yellow, 20% red thresholds
- Bitmap Alloc CAS Retries/Op: 0.01 yellow, 0.05 red
- Bitmap Free CAS Retries/Op: no thresholds (usually near zero)
- current_partial CAS Failure Rate: 80-100% normal (not alarm)
- Merged into main dashboard

**Documentation:**
- [COMPLETE] `CONTENTION_RESULTS.md` - Empirical validation tables (0% → 20% contention, 1→16 threads)
- [COMPLETE] `PHASE_2.2_PROMETHEUS_SPEC.md` - PR-ready spec with PromQL queries
- [COMPLETE] `GRAFANA_DEMO.md` - Step-by-step visualization guide
- [COMPLETE] `demo_threading.sh` - Threading capabilities demonstration

**Performance validation:**
- Overhead: <0.1% (atomic increments only)
- Jitter: Zero (no clock syscalls in Tier 0 probe)
- Scaling: Lock contention plateaus at 20% for 16 threads (healthy)
- CAS retries: <0.05 retries/op for 16 threads (excellent)

### Why It Matters

**HFT Production Requirements:**
- **Variance >> mean:** Jitter more important than average overhead
- **Zero jitter:** clock_gettime costs ~60ns, unpredictable (unacceptable for HFT)
- **Tier 0 probe:** ~2ns overhead, zero variance, production-safe always-on

**Phase 2.2 enables:**
1. **Lock contention diagnosis:** Which size classes are blocking threads?
2. **CAS retry tracking:** How much bitmap/fast-path contention exists?
3. **Scaling validation:** Does contention scale linearly or exponentially?
4. **Tuning guidance:** When to add per-thread caches, slab sizing

**Example Grafana queries:**
```promql
# Lock contention rate (% of acquisitions that blocked)
sum by (object_size) (
  rate(temporal_slab_class_lock_contended_total[1m])
  /
  clamp_min(rate(temporal_slab_class_lock_acquisitions_total[1m]), 0.0001)
) * 100

# CAS retry rate (retries per operation)
sum by (object_size) (
  rate(temporal_slab_class_bitmap_alloc_cas_retries_total[1m])
  /
  clamp_min(rate(temporal_slab_class_bitmap_alloc_attempts_total[1m]), 0.0001)
)
```

**HFT Production Policy:** Tier 0 probe always-on, clock timing off.

---

## Phase 2.3: Semantic Attribution [COMPLETE]

### Status

**Implementation:** COMPLETE (2026-02-07)  
**Tested:** COMPLETE (All commands working, Grafana dashboard live)  
**Blockers:** None  
**Priority:** HIGH (enables application bug detection)

### What Was Delivered

**JSON output (stats_dump):**
```json
{
  "epochs": [
    {
      "epoch_id": 12,
      "epoch_era": 150,
      "state": "ACTIVE",
      "age_sec": 600,
      "refcount": 2,
      "label": "background-worker-3",
      "total_partial_slabs": 100,
      "total_full_slabs": 50,
      "total_reclaimable_slabs": 25,
      "estimated_rss_bytes": 50331648
    }
  ]
}
```

**Interactive tooling:**
```bash
# Rank epochs by age (leak detection)
./stats_dump --no-text | tslab top --epochs --by age_sec --stdin

# Rank epochs by RSS (memory attribution)
./stats_dump --no-text | tslab top --epochs --by rss_bytes --stdin
```

**Prometheus metrics (5 new metrics):**
```
temporal_slab_epoch_age_seconds{epoch="12",state="ACTIVE",label="background-worker-3"} 600
temporal_slab_epoch_refcount{epoch="12",state="ACTIVE",label="background-worker-3"} 2
temporal_slab_epoch_rss_bytes{epoch="12",state="ACTIVE",label="background-worker-3"} 50331648
temporal_slab_epoch_reclaimable_slabs{epoch="12",state="ACTIVE",label="background-worker-3"} 25
temporal_slab_epoch_state{epoch="12",state="ACTIVE",label="background-worker-3"} 1
```

**Grafana dashboard (4 new panels):**
- Epoch age heatmap (ACTIVE epochs, threshold coloring)
- Top epochs by RSS (topk query for memory attribution)
- Refcount tracking for CLOSING epochs (drainage monitoring)
- Reclaimable slabs by epoch (reclamation opportunities)

**Pre-built alerting rules:**
```yaml
- alert: EpochLeak
  expr: max(temporal_slab_epoch_age_seconds) > 300
  annotations:
    summary: "Epoch {{ $labels.epoch }} ({{ $labels.label }}) stuck for {{ $value }}s"

- alert: EpochMemoryLeak
  expr: temporal_slab_epoch_rss_bytes > 52428800 and temporal_slab_epoch_age_seconds > 60
  annotations:
    summary: "Epoch {{ $labels.epoch }} leaking {{ $value | humanize }}B"
```

### Why It Matters

**Current limitation:**
- Can see "Epoch 5 has been open 10 minutes"
- Can't see **why** (which application domain owns it)
- Can't detect **refcount leaks** (domain_enter without domain_exit)

**Phase 2.3 enables:**

**1. Application bug detection:**
```json
{
  "epoch_id": 12,
  "epoch_era": 150,
  "age_sec": 600,
  "refcount": 2,
  "label": "background-worker-3"
}
```
**Diagnosis:** "background-worker-3 domain entered twice, exited zero times. **Application bug:** missing `domain_exit()` calls."

**2. Resource attribution:**
```promql
sum by (label) (temporal_slab_epoch_estimated_rss_bytes)
```
**Result:** "request-handler: 50 MB, background-task: 10 MB"

**3. Leak detection:**
```promql
temporal_slab_epoch_refcount{age_sec > 300} > 0
```
**Alert:** "Epoch has been open >5 minutes with non-zero refcount (domain leak)."

---

### Implementation Effort

**Estimated:** 4-6 hours

**Tasks:**
1. Add `EpochMetadata epoch_meta[EPOCH_COUNT]` to `SlabAllocator`
2. Implement `slab_epoch_set_label()`, `slab_epoch_inc_refcount()`, `slab_epoch_dec_refcount()`
3. Populate `open_since_ns` in `epoch_advance()`
4. Expose in `SlabEpochStats` struct
5. Update `stats_dump` JSON output with per-epoch metadata
6. Update Prometheus exporter to include `label` as Prometheus label
7. Update Grafana dashboard with epoch panels

**External tool changes:**
- Prometheus metrics gain `label` label: `temporal_slab_epoch_rss_bytes{epoch="5",label="request-handler"}`
- Grafana dashboard adds epoch panels: "Top epochs by age", "Top epochs by RSS"

---

### Why Phase 2.3 is High Priority

**This enables the killer feature:** Allocator definitively identifies **application bugs**.

**No other allocator can say:**
> "Epoch 12 (background-worker-3) has been open for 10 minutes with refcount=2. Application entered domain twice but never exited. **Application bug, not allocator issue.**"

malloc/jemalloc see "RSS growing" (symptom), but can't attribute to application structure.

temporal-slab sees "refcount=2, age=600s" (causality), and identifies **who forgot cleanup**.

---

### Phase 2.3 Extension: Contention Label Attribution [COMPLETE]

**Implementation:** [COMPLETE] Complete (2026-02-08)  
**Tested:** [COMPLETE] Compiles with/without ENABLE_LABEL_CONTENTION  
**Production-Ready:** ✅ HFT-safe (ID-based lookup, zero string compares in hot path)

#### What Was Extended

**Original Phase 2.3:**
- Epoch metrics have label attribution (`temporal_slab_epoch_rss_bytes{label="request"}`)
- Domain refcount tracking for leak detection
- Application-level semantic labels (e.g., "request", "frame", "gc")

**New in Phase 2.3 Extension (Contention Attribution):**
- **Contention metrics now include label dimension**
- Answers: "Which subsystem (request/frame/gc) is causing lock/CAS contention?"
- HFT-safe implementation: ID-based (not string-based) hot-path lookup

#### Implementation Architecture

**Data Structures:**
```c
// Phase 2.3: Label registry for bounded semantic attribution
#define MAX_LABEL_IDS 16  // ID 0 = unlabeled, IDs 1-15 = semantic labels

typedef struct LabelRegistry {
  char labels[MAX_LABEL_IDS][32];  // label_id -> label string
  uint8_t count;                   // Next available ID (capped at MAX_LABEL_IDS)
  pthread_mutex_t lock;            // Protects label registration (cold path only)
} LabelRegistry;

// EpochMetadata extended with label_id
typedef struct EpochMetadata {
  char label[32];      // Semantic label string
  uint8_t label_id;    // Stable small ID (0-15) for hot-path lookups
  // ...
} EpochMetadata;

// SizeClassAlloc extended with per-label contention arrays (compile-time optional)
#ifdef ENABLE_LABEL_CONTENTION
  _Atomic uint64_t lock_fast_acquire_by_label[16];
  _Atomic uint64_t lock_contended_by_label[16];
  _Atomic uint64_t bitmap_alloc_cas_retries_by_label[16];
  _Atomic uint64_t bitmap_free_cas_retries_by_label[16];
#endif
```

**Hot-Path Implementation (HFT-Safe):**
```c
// TLS lookup: domain → epoch_id → label_id (~5ns total, zero string compares)
static inline uint8_t current_label_id(SlabAllocator* alloc) {
  epoch_domain_t* d = epoch_domain_current();  // TLS read
  if (!d) return 0;  // No active domain = unlabeled
  return alloc->epoch_meta[d->epoch_id].label_id;  // uint8_t read
}

// Dual-layer metrics pattern (backward compatible)
#ifdef ENABLE_LABEL_CONTENTION
  atomic_fetch_add(&lock_contended, 1);  // Layer 1: Global (always)
  uint8_t lid = current_label_id(alloc); // Layer 2: Per-label (optional)
  atomic_fetch_add(&lock_contended_by_label[lid], 1);
#else
  atomic_fetch_add(&lock_contended, 1);  // Layer 1 only (HFT prod default)
#endif
```

**Cold-Path Implementation (Label Registration):**
```c
void slab_epoch_set_label(SlabAllocator* alloc, EpochId epoch, const char* label) {
  pthread_mutex_lock(&alloc->label_registry.lock);
  
  // Search for existing label (O(n), but n≤16 and cold path)
  for (uint8_t i = 1; i < alloc->label_registry.count; i++) {
    if (strncmp(alloc->label_registry.labels[i], label, 31) == 0) {
      label_id = i;  // Reuse existing ID
      break;
    }
  }
  
  // Allocate new ID if not found and space available
  if (label_id == 0 && alloc->label_registry.count < MAX_LABEL_IDS) {
    label_id = alloc->label_registry.count++;
    strncpy(alloc->label_registry.labels[label_id], label, 31);
  }
  // If registry full, label_id=0 ("unlabeled" / "other" bucket)
  
  pthread_mutex_unlock(&alloc->label_registry.lock);
  
  alloc->epoch_meta[epoch].label_id = label_id;  // Store stable ID
}
```

#### Prometheus Metrics (4 New Series)

**Per-label contention series (emitted only when ENABLE_LABEL_CONTENTION):**
```
temporal_slab_class_lock_fast_acquire_by_label_total{class="2",object_size="128",label="request"} 950000
temporal_slab_class_lock_contended_by_label_total{class="2",object_size="128",label="request"} 50000
temporal_slab_class_lock_contended_by_label_total{class="2",object_size="128",label="frame"} 12000
temporal_slab_class_lock_contended_by_label_total{class="2",object_size="128",label="gc"} 3000

temporal_slab_class_bitmap_alloc_cas_retries_by_label_total{class="2",object_size="128",label="request"} 1200
temporal_slab_class_bitmap_alloc_cas_retries_by_label_total{class="2",object_size="128",label="frame"} 300

temporal_slab_class_bitmap_free_cas_retries_by_label_total{class="2",object_size="128",label="request"} 5
```

**Example PromQL Query:**
```promql
# Which subsystem is causing lock contention?
topk(3,
  sum by (label) (
    rate(temporal_slab_class_lock_contended_by_label_total[1m])
  )
)
# Result: "request: 950 contentions/sec, frame: 200 contentions/sec, gc: 50 contentions/sec"

# Lock contention rate per label
sum by (label) (
  rate(temporal_slab_class_lock_contended_by_label_total[1m])
) /
sum by (label) (
  rate(temporal_slab_class_lock_fast_acquire_by_label_total[1m]) +
  rate(temporal_slab_class_lock_contended_by_label_total[1m])
) * 100
# Result: "request: 5%, frame: 12%, gc: 2%"
```

#### HFT Safety Guarantees

**Hot-path overhead:**
- **Without ENABLE_LABEL_CONTENTION:** 0ns (unchanged from Phase 2.2)
- **With ENABLE_LABEL_CONTENTION:** ~5ns (TLS read + uint8_t index + atomic add)
  - No string compares
  - No hash lookups
  - No mutex locks
  - Deterministic, zero jitter

**Bounded cardinality:**
- Max 16 labels enforced at registration
- Label 0 reserved for "unlabeled" (no active domain)
- 17th label registration fails gracefully → bucketed to ID 0
- Prevents Prometheus cardinality explosion

**Compile-time gating:**
- Default: `ENABLE_LABEL_CONTENTION` OFF (zero overhead, HFT prod)
- Diagnostic mode: `ENABLE_LABEL_CONTENTION` ON (5ns overhead, staging/lab)
- Binary is production-ready in both configurations

#### Why This Extension Matters

**Before Phase 2.3 Extension:**
```promql
temporal_slab_class_lock_contended_total{class="2",object_size="128"} 65000
```
*Answer:* "Size class 2 has 65k contention events" (what)

**After Phase 2.3 Extension:**
```promql
temporal_slab_class_lock_contended_by_label_total{class="2",object_size="128",label="request"} 50000
temporal_slab_class_lock_contended_by_label_total{class="2",object_size="128",label="frame"} 12000
temporal_slab_class_lock_contended_by_label_total{class="2",object_size="128",label="gc"} 3000
```
*Answer:* "Request processing (77%) is causing most contention, not GC" (why)

**This enables:**
1. **Subsystem attribution:** Identify which application component causes contention
2. **Tuning decisions:** "Request handler needs per-thread cache, GC doesn't"
3. **Root cause analysis:** "Contention spike correlates with request label, not frame/gc"
4. **Performance regression detection:** "Request label contention jumped 10× after deploy"

---

## Phase 2.4: RSS Delta Tracking [COMPLETE]

### Status

**Implementation:** COMPLETE (2026-02-09)  
**Tested:** COMPLETE (test_rss_delta.c, 3 scenarios)  
**Blockers:** None  
**Priority:** MEDIUM (kernel cooperation validation)

### What It Adds

**Per-epoch metadata (extends Phase 2.3):**
```c
typedef struct EpochMetadata {
  // ... Phase 2.3 fields ...
  
  /* Phase 2.4: RSS delta tracking */
  uint64_t rss_before_close;    /* RSS snapshot at start of epoch_close() */
  uint64_t rss_after_close;     /* RSS snapshot at end of epoch_close() */
} EpochMetadata;
```

**Global last-close tracking:**
```c
typedef struct LastEpochClose {
  uint64_t timestamp_ns;
  uint64_t rss_before;
  uint64_t rss_after;
  uint64_t madvise_bytes;
} LastEpochClose;
```

### Why It Matters

**Current limitation:**
- Can see `madvise_calls` and `madvise_bytes`
- Can see RSS dropped
- **Can't correlate:** Did RSS drop **because** of madvise? (vs unrelated free)

**Phase 2.4 enables:**

**Kernel cooperation score (heuristic):**
```
cooperation_pct = (rss_after - rss_before) / madvise_bytes * 100
```

**Example 1: Full cooperation**
```json
{
  "rss_before_close": 100_000_000,
  "rss_after_close":   90_000_000,
  "madvise_bytes":     10_000_000,
  "cooperation_pct": 100.0
}
```
**Interpretation:** "Kernel reclaimed exactly what we requested (100% cooperation)."

**Example 2: No cooperation**
```json
{
  "rss_before_close": 100_000_000,
  "rss_after_close":  100_000_000,
  "madvise_bytes":     10_000_000,
  "cooperation_pct": 0.0
}
```
**Interpretation:** "Kernel ignored madvise (0% cooperation). System under memory pressure or transparent huge pages interfering."

---

### Important Caveats

**RSS delta is heuristic, not truth:**
- Other threads might allocate/free during `epoch_close()`
- Kernel might reclaim unrelated memory
- Transparent huge pages complicate accounting

**Phase 2.4 exposes context, not claims:**
- "RSS dropped 10 MB after madvising 10 MB" (observable fact)
- **Not:** "Kernel cooperated 100%" (interpretation, not guarantee)

**Validation results (Feb 9 2026, 200-cycle test):**
- Committed bytes: 0.70 MB → 0.70 MB (0.0% drift, perfect stability)
- Anonymous RSS: 0.36 MB → 0.39 MB (slight growth from application overhead)
- Bounded RSS proven: <10% stability metric (tight oscillation band)
- Proof: No unbounded ratcheting across sustained churn

---

## Phase 2.5: Probabilistic Slowpath Sampling [COMPLETE]

### Status

**Implementation:** COMPLETE (2026-02-10)  
**Tested:** COMPLETE (3 tests: simple_test, contention_sampling_test, zombie_repair_test)  
**Blockers:** None  
**Priority:** HIGH (enables tail latency attribution in virtualized environments)

### What Was Delivered

**Core innovation: wait_ns metric**
```c
wait_ns = wall_ns - cpu_ns  // Scheduler interference
```

**ThreadStats structure (TLS, zero contention):**
```c
typedef struct {
  /* Allocation sampling (1/1024 rate) */
  uint64_t alloc_samples;
  uint64_t alloc_wall_ns_sum;      /* Wall time (includes scheduler) */
  uint64_t alloc_cpu_ns_sum;       /* CPU time (allocator work only) */
  uint64_t alloc_wait_ns_sum;      /* wait_ns = wall - cpu */
  uint64_t alloc_wall_ns_max;
  uint64_t alloc_cpu_ns_max;
  uint64_t alloc_wait_ns_max;
  
  /* Zombie repair timing */
  uint64_t repair_count;
  uint64_t repair_wall_ns_sum;
  uint64_t repair_cpu_ns_sum;
  uint64_t repair_wait_ns_sum;
  uint64_t repair_wall_ns_max;
  uint64_t repair_cpu_ns_max;
  uint64_t repair_wait_ns_max;
  
  /* Repair reason attribution */
  uint64_t repair_reason_full_bitmap;    /* 100% of repairs in adversarial test */
  uint64_t repair_reason_list_mismatch;
  uint64_t repair_reason_other;
} ThreadStats;
```

**Three validation tests:**
1. **simple_test.c** - Single-thread baseline (scheduler overhead measurement)
2. **contention_sampling_test.c** - Multi-thread contention (8 threads, thread pinning)
3. **zombie_repair_test.c** - Adversarial repair trigger (16 threads, batch alloc/free)

**Post-processing:**
- `analyze_sampling.sh` - Truth table generation from test output

### Why It Matters

**Problem:** Tail latency spikes in WSL2/VM environments conflate:
1. **Allocator work** (CAS storms, lock contention, zombie repairs)
2. **Scheduler interference** (hypervisor preemption, context switches)

Aggregate wall-time metrics can't distinguish these.

**Phase 2.5 enables:**
- **Scheduler vs allocator attribution:** `wait_ns > cpu_ns` → scheduler problem, not allocator
- **Contention validation:** CPU time doubles (398ns → 3,200ns) proves contention is real
- **Repair characterization:** 9-14µs CPU-bound, 0.01% rate, 100% `full_bitmap` reason
- **Self-healing validation:** Repairs benign (no corruption, no crashes)

### Validation Results (Feb 10 2026, WSL2)

| Test | Threads | Samples | Avg CPU | Avg Wait | Repairs | Key Finding |
|------|---------|---------|---------|----------|---------|-------------|
| simple_test | 1 | 97 | 398ns | 910ns | 0 | WSL2 adds 2.3× overhead (70% wait) |
| contention_sampling_test | 8 | 776 | 3,200ns | 600ns | 0 | CPU 2× vs 1T (contention real) |
| zombie_repair_test | 16 | 768 | 1,400ns | 380ns | 83 | 0.01% repair rate (CPU-bound) |

**Key insights:**
- **Single-thread:** 70% of latency is scheduler (wait >> cpu)
- **Multi-thread:** 20% of latency is scheduler (cpu >> wait)
- **Proof:** Contention doubles CPU time, not a WSL2 artifact
- **Zombie repairs:** Self-healing works (0.0104% rate, 9-14µs, benign)

**Overhead:** <0.2% amortized (80ns per sample / 1024 allocations)

**Documentation:**
- `workloads/README_SLOWPATH_SAMPLING.md` - Complete Phase 2.5 documentation (342 lines)
- Truth tables, test matrix, validation results, environment notes

---

## Phase 2.6: TLS Handle Cache [COMPLETE]

### Status

**Implementation:** COMPLETE (2026-02-08)  
**Tested:** COMPLETE (locality_bench validation)  
**Blockers:** None  
**Priority:** MEDIUM (high-locality workload optimization)

### What Was Delivered

**Thread-local handle caching:**
```c
// Per-thread cache (256 handles per size class)
typedef struct {
  TLSItem cache[256];    /* LIFO stack of cached handles */
  uint32_t count;        /* Current cache depth */
  bool hard_bypass;      /* Disable if hit rate <2% */
} TLSCache;

typedef struct {
  SlabHandle handle;     /* Cached handle (24 bytes) */
  void* ptr;             /* Pre-resolved pointer */
  EpochId epoch_id;      /* Validity check */
} TLSItem;
```

**Adaptive bypass:**
- Windowed hit-rate tracking (256-op measurement windows)
- Hard bypass when hit rate <2% (adversarial pattern detection)
- Auto-disables for low-locality workloads

**Epoch validation:**
- Cached handles from CLOSING epochs rejected
- TLS flush API: `tls_flush_epoch_all_threads()`
- Ensures `epoch_close()` correctness

### Why It Matters

**Problem:** High-locality workloads (same thread allocates and frees frequently) pay full atomic bitmap CAS cost even when last-freed slot is immediately reused.

**Phase 2.6 enables:**
- **p50 improvement:** -11% (41ns → 36ns) - eliminates atomic bitmap CAS
- **p99 improvement:** -75% (96ns → 24ns) - TLS hits bypass slow path entirely
- **Throughput:** +15% on single-thread high-locality workloads

### Trade-offs

**Alloc-only caching:**
- Frees always update global state (prevents metadata divergence)
- Maintains "handle as stable reference" semantic
- See `TLS_CACHE_DESIGN.md` for design rationale

**Requires explicit build:**
```bash
make CFLAGS="-DENABLE_TLS_CACHE=1 -I../include" TLS_OBJ=slab_tls_cache.o
```

**Not enabled by default:**
- Workload-specific optimization (helps high-locality, hurts low-locality)
- +256 handles × sizeof(TLSItem) per thread per size class memory overhead

### Validation

**Workloads that benefit (>50% temporal locality):**
- Request-local allocation (web servers, API handlers)
- Frame-local allocation (game engines, render loops)
- Transaction-local allocation (databases)

**Workloads that don't benefit:**
- Producer-consumer patterns (allocate in A, free in B)
- Scatter-gather (random allocation/free interleaving)
- Long-lived objects (no reuse opportunity)

---

## Additional Capabilities (Integrated Across Phases)

### Adaptive Bitmap Scanning (Phase 2.2+)

**Problem:** Under high contention, sequential bitmap scanning creates thundering herd (all threads compete for bit 0).

**Solution:** Reactive controller switches between sequential (cache-friendly) and randomized (contention-spreading) based on CAS retry rate.

**Validation (47 production trials, 1-16 threads):**
- T=1: mode=0, switches=0, retries=0.0 (perfect sequential)
- T=2: mode=0.1, switches=2.1 (active switching, proves controller works)
- T=8: mode=1.0, switches=0, retries=2.58 (stable randomized, 19% retry reduction)
- T=16: mode=1.0, switches=0, retries=2.46 (stable randomized)

**Impact:** 5.8× reduction in CAS retries at 16 threads (0.043 → 0.0074 retries/op)

### Zombie Repair Self-Healing (Validated Phase 2.5)

**Problem:** Publication races create "zombie partial" slabs (free_count=0, bitmap full).

**Solution:** Detection + repair on every allocation attempt.

**Validation (16-thread adversarial test):**
- **Repair rate:** 0.0104% (1 per 9,639 allocations)
- **Repair timing:** 9-14µs avg (CPU-bound)
- **Reason attribution:** 100% `full_bitmap` (specific race condition)
- **Result:** No corruption, no crashes, system self-heals

**Why it matters:** Distinguishes production system from research prototype. Race conditions inevitable under high concurrency - self-healing makes them benign, not fatal.

**Dashboard should say:**
- "RSS delta: -10 MB" (fact)
- "madvise bytes: 10 MB" (fact)
- "Correlation: 100%" (heuristic)

---

### Implementation Effort

**Estimated:** 2-3 hours

**Tasks:**
1. Add `rss_before_close`, `rss_after_close` to `EpochMetadata`
2. Sample RSS at start/end of `epoch_close()` (via `read_rss_bytes_linux()`)
3. Expose in `SlabEpochStats` struct
4. Update `stats_dump` JSON output
5. Update Grafana dashboard with "Kernel Cooperation" panel

**Prometheus query:**
```promql
(temporal_slab_epoch_rss_before_close - temporal_slab_epoch_rss_after_close) 
/ 
temporal_slab_epoch_madvise_bytes * 100
```

---

## Phase 2.5: External Tooling Enhancements [DESIGNED] FUTURE

### Not Yet Designed (Ideas)

**1. Rate-based ranking:**
```bash
tslab top --classes --window 1s --by slow_path_rate --n 5
```
Take two snapshots 1s apart, rank by delta/sec instead of cumulative.

**2. HTTP server mode:**
```bash
tslab serve --port 2112 --interval 5s
```
Serve Prometheus metrics on `/metrics` endpoint (alternative to textfile collector).

**3. CI diff mode:**
```bash
tslab diff before.json after.json --fail-on regression
```
Compare snapshots and alert on regressions (slow-path rate +10%, madvise failures >0).

**4. Grafana alerting templates:**
Pre-built alert rules with runbooks (currently manual configuration).

---

## Recommended Next Steps

### Option A: Complete Semantic Attribution (Phase 2.3) - HIGH VALUE

**Why:** Enables application bug detection (refcount leak identification).

**Effort:** 4-6 hours  
**Payoff:** Allocator becomes "smart" about application structure  
**Blockers:** None  

**Deliverables:**
- Epoch labels (`label[32]`)
- Refcount tracking (inc/dec APIs)
- Grafana epoch panels ("Top epochs by age", "Top epochs by refcount")
- Alert rule: `temporal_slab_epoch_refcount{age_sec > 300} > 0`

**Impact:**
- SRE can query "Which domain leaked?" (not just "RSS growing")
- Allocator identifies **who** owns the bug (application vs allocator vs kernel)

---

### Option B: Epoch-Close Telemetry (Phase 2.1) - QUICK WIN

**Why:** Quantifies reclamation effectiveness (low implementation cost).

**Effort:** 2-3 hours  
**Payoff:** Validate epoch-close heuristic is working  
**Blockers:** None  

**Deliverables:**
- `epoch_close_calls`, `epoch_close_scanned_slabs`, `epoch_close_recycled_slabs`
- Prometheus query: `recycled_slabs / scanned_slabs` (reclamation efficiency)
- Alert rule: `epoch_close_recycled_slabs / epoch_close_scanned_slabs < 0.1` (low yield)

**Impact:**
- Performance engineer can tune epoch-close scan frequency
- Identify if scanning is expensive but low-yield

---

### Option C: Kernel Cooperation Telemetry (Phase 2.4) - NICE-TO-HAVE

**Why:** Correlates madvise with RSS drops (interesting but not critical).

**Effort:** 2-3 hours  
**Payoff:** Diagnose kernel cooperation issues  
**Blockers:** None  

**Deliverables:**
- `rss_before_close`, `rss_after_close` per epoch
- Grafana panel: "Kernel Cooperation %" (heuristic)
- Alert rule: `cooperation_pct < 50` (kernel not cooperating)

**Impact:**
- SRE can identify system memory pressure (kernel ignoring madvise)
- Validate transparent huge pages aren't interfering

---

## Summary: Where We Stand

**Production-ready observability:**
- [COMPLETE] Full monitoring stack (Prometheus + Grafana)
- [COMPLETE] Sub-1% overhead (atomic relaxed counters)
- [COMPLETE] Structural observability (causality, not patterns)
- [COMPLETE] Root cause attribution (slow-path, RSS, madvise)
- [COMPLETE] External tooling (4 commands, 40+ metrics)
- [COMPLETE] Complete documentation (7 docs, 4,479 lines)

**Next capability unlock:**
- 🎯 **Phase 2.3** (semantic attribution) → Application bug detection
- 🎯 **Phase 2.1** (epoch-close telemetry) → Reclamation effectiveness
- 🎯 **Phase 2.4** (kernel cooperation) → Validate kernel behavior

**Current gap:**
- Can't identify **which application domain** owns an epoch
- Can't detect **refcount leaks** (domain_enter without domain_exit)
- Can't quantify **reclamation effectiveness** (scan yield)

**Recommendation:** Implement **Phase 2.3** next (highest value, enables killer feature).

---

**Maintainer:** blackwd  
**Related docs:**
- `OBSERVABILITY_DESIGN.md` - Complete phase breakdown
- `stats_dump_reference.md` - JSON field definitions
- `../temporal-slab-tools/docs/OBSERVABILITY_PHILOSOPHY.md` - Why structural observability matters
- `../temporal-slab-tools/docs/MONITORING_SETUP.md` - Prometheus + Grafana deployment
