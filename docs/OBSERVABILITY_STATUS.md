# Observability Implementation Status

Current state of the Phase 2 observability roadmap and what's next.

**Last updated:** 2026-02-07

---

## Executive Summary

**Phase 2.0: COMPLETE** - Production-ready monitoring with external tooling  
**Phase 2.1: COMPLETE** - Epoch-close telemetry + era stamping  
**Phase 2.2: COMPLETE** - Multi-threading contention observability (HFT-safe)  
**Phase 2.3: COMPLETE** - Semantic attribution (labels, refcount, leak detection)  
**Phase 2.4: DESIGNED** - Kernel cooperation telemetry  

**Current capability:** Full structural observability with Prometheus + Grafana dashboard, sub-1% overhead, root cause attribution for slow-path and RSS behavior, **HFT-safe contention tracking (zero jitter)**, and semantic attribution for leak detection.

**Next milestone:** Phase 2.4 (kernel cooperation telemetry) or comparative benchmarks vs malloc/jemalloc.

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

## Phase 2.3: Semantic Attribution [COMPLETE] COMPLETE

### Status

**Implementation:** [COMPLETE] Complete (2025-02-07)  
**Tested:** [COMPLETE] All commands working, Grafana dashboard live  
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

## Phase 2.4: Kernel Cooperation Telemetry [DESIGNED] DESIGNED

### Status

**Implementation:** Not started  
**Design:** Complete (OBSERVABILITY_DESIGN.md)  
**Blockers:** None  
**Priority:** MEDIUM (nice-to-have after 2.3)

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
