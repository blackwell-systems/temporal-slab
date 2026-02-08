# Contention Metrics Design: Multi-Thread Observability

**Purpose:** Add micro-level contention metrics to validate multi-threaded behavior and diagnose CAS/lock bottlenecks.

**Date:** 2026-02-07  
**Status:** Design + implementation plan

---

## Motivation

**Current gap:** We have slow-path attribution (cache_miss, epoch_closed) but no visibility into lock-free contention patterns.

**What we can't answer today:**
- How many CAS retries happen in bitmap allocation/free?
- How often does `current_partial` CAS fail (fast-path contention)?
- What's the lock hold time distribution (proxy for mutex contention)?
- Are 8 threads worse than 4 threads for CAS contention?

**Multi-thread validation needs:**
1. Prove N threads can allocate/free concurrently without pathological behavior
2. Measure CAS retry rates under contention
3. Detect lock bottlenecks (slow vs fast acquisition)
4. Quantify cross-thread free contention

---

## New Counters

### Per-Size-Class Contention Metrics

Add to `SizeClassAlloc` (slab_alloc_internal.h:143+):

```c
/* Phase 2.X: Lock-free contention metrics */
_Atomic uint64_t bitmap_alloc_cas_retries;     /* CAS loops in slab_alloc_slot_ctz */
_Atomic uint64_t bitmap_free_cas_retries;      /* CAS loops in slab_free_slot_atomic */
_Atomic uint64_t current_partial_cas_failures; /* fast-path pointer swap failures */

/* Phase 2.X: Denominators for correct ratios */
_Atomic uint64_t bitmap_alloc_attempts;        /* Successful allocations (denominator) */
_Atomic uint64_t bitmap_free_attempts;         /* Successful frees (denominator) */
_Atomic uint64_t current_partial_cas_attempts; /* All current_partial pointer CAS attempts (denominator) */

/* Phase 2.2: Lock contention metrics (Tier 0 trylock probe, always-on) */
_Atomic uint64_t lock_fast_acquire;            /* Trylock succeeded (no contention) */
_Atomic uint64_t lock_contended;               /* Trylock failed, had to wait */
```

---

## Instrumentation Points

### 1. Bitmap Allocation CAS Retries

**Location:** `slab_alloc_slot_ctz()` (slab_alloc.c:~300-350)

**Current code:**
```c
uint32_t idx = UINT32_MAX;
while (true) {
  uint32_t free_bits = atomic_load_explicit(&word, memory_order_acquire);
  if (free_bits == 0) {
    return UINT32_MAX;  /* Bitmap fully allocated */
  }
  
  uint32_t trailing_zeros = ctz32(free_bits);
  uint32_t mask = 1u << trailing_zeros;
  uint32_t new_bits = free_bits & ~mask;
  
  if (atomic_compare_exchange_weak_explicit(&word, &free_bits, new_bits,
                                             memory_order_release,
                                             memory_order_acquire)) {
    idx = word_offset * 32 + trailing_zeros;
    break;
  }
  /* CAS FAILED - another thread claimed slot, retry */
}
```

**Instrumentation:**
```c
uint32_t retries = 0;  /* Local counter */
while (true) {
  /* ... CAS loop ... */
  if (CAS succeeded) {
    break;
  }
  retries++;  /* Count retry */
}

/* After successful allocation, record attempt + retries */
atomic_fetch_add_explicit(&sc->bitmap_alloc_attempts, 1, memory_order_relaxed);
if (retries > 0) {
  atomic_fetch_add_explicit(&sc->bitmap_alloc_cas_retries, retries, memory_order_relaxed);
}
```

### 2. Bitmap Free CAS Retries

**Location:** `slab_free_slot_atomic()` (slab_alloc.c:~396-450)

**Instrumentation:**
```c
uint32_t retries = 0;
while (true) {
  uint32_t prev_bits = atomic_load_explicit(&word, memory_order_acquire);
  
  if (prev_bits & mask) {
    return false;  /* Double-free detected */
  }
  
  uint32_t new_bits = prev_bits | mask;
  
  if (atomic_compare_exchange_weak_explicit(&word, &prev_bits, new_bits,
                                             memory_order_release,
                                             memory_order_acquire)) {
    *out_prev_fc = prev_fc;
    break;
  }
  retries++;
}

/* After successful free, record attempt + retries */
atomic_fetch_add_explicit(&sc->bitmap_free_attempts, 1, memory_order_relaxed);
if (retries > 0) {
  atomic_fetch_add_explicit(&sc->bitmap_free_cas_retries, retries, memory_order_relaxed);
}
```

### 3. Current_Partial CAS Failures

**Location:** Multiple sites where `current_partial` is swapped

**Example (alloc_obj_epoch fast path):**
```c
Slab* expected = NULL;
atomic_fetch_add_explicit(&sc->current_partial_cas_attempts, 1, memory_order_relaxed);

bool swapped = atomic_compare_exchange_strong_explicit(
    &es->current_partial, &expected, s,
    memory_order_release, memory_order_relaxed);

if (!swapped) {
  atomic_fetch_add_explicit(&sc->current_partial_cas_failures, 1, memory_order_relaxed);
}
```

**Help text:** "current_partial_cas_failures may indicate contention OR expected state mismatch (slab already promoted). Ratio failures/attempts shows true contention rate."

**Sites to instrument:**
- Fast-path promotion (slab becomes current_partial)
- Recycling path (empty slab removed from current_partial)
- Full transition (slab moved off current_partial)

### 4. Lock Wait + Hold Time (Contention + CS Cost)

**Location:** Every `pthread_mutex_lock(&sc->lock)` call

**Pattern (track BOTH wait and hold):**
```c
uint64_t t_before = now_ns();
pthread_mutex_lock(&sc->lock);
uint64_t t_acquired = now_ns();

/* Record acquisition */
atomic_fetch_add_explicit(&sc->lock_acquisitions, 1, memory_order_relaxed);

/* Record wait time (contention signal) */
uint64_t wait_time = t_acquired - t_before;
atomic_fetch_add_explicit(&sc->lock_wait_time_ns, wait_time, memory_order_relaxed);

/* Critical section work */

uint64_t t_release = now_ns();
pthread_mutex_unlock(&sc->lock);

/* Record hold time (CS cost) */
uint64_t hold_time = t_release - t_acquired;
atomic_fetch_add_explicit(&sc->lock_hold_time_ns, hold_time, memory_order_relaxed);
```

**Why BOTH metrics?**
- **Wait time** (t_acquired - t_before) = Time waiting for lock (contention indicator)
  - High wait time → threads blocking each other
  - Includes OS scheduler variance (but still useful signal)
- **Hold time** (t_release - t_acquired) = Time under lock (CS cost)
  - High hold time → critical sections are expensive
  - More stable metric (no OS noise)

**Prometheus queries:**
```promql
# Average contention (wait time)
rate(lock_wait_time_seconds_total[1m]) / rate(lock_acquisitions_total[1m])

# Average CS cost (hold time)
rate(lock_hold_time_seconds_total[1m]) / rate(lock_acquisitions_total[1m])
```

---

## Stats API Extensions

### SlabClassStats (slab_stats.h)

```c
typedef struct SlabClassStats {
  /* ... existing fields ... */
  
  /* Phase 2.X: Contention metrics */
  uint64_t bitmap_alloc_cas_retries;     /* CAS spin count in allocation */
  uint64_t bitmap_free_cas_retries;      /* CAS spin count in free */
  uint64_t current_partial_cas_failures; /* Fast-path pointer swap failures */
  
  /* Phase 2.X: Denominators for correct ratios */
  uint64_t bitmap_alloc_attempts;        /* Successful allocations */
  uint64_t bitmap_free_attempts;         /* Successful frees */
  uint64_t current_partial_cas_attempts; /* All promotion attempts */
  
  /* Phase 2.X: Lock contention metrics */
  uint64_t lock_acquisitions;            /* Number of lock calls */
  uint64_t lock_wait_time_ns;            /* Time waiting for lock (contention) */
  uint64_t lock_hold_time_ns;            /* Time under lock (CS cost) */
  
  /* Derived metrics (computed by stats_dump) */
  double avg_alloc_cas_retries_per_attempt;  /* retries / alloc_attempts */
  double avg_free_cas_retries_per_attempt;   /* retries / free_attempts */
  double current_partial_cas_failure_rate;   /* failures / cas_attempts */
  double avg_lock_wait_time_ns;              /* wait_time_ns / lock_acquisitions */
  double avg_lock_hold_time_ns;              /* hold_time_ns / lock_acquisitions */
} SlabClassStats;
```

### SlabGlobalStats (aggregate)

```c
typedef struct SlabGlobalStats {
  /* ... existing fields ... */
  
  /* Phase 2.X: Global contention totals */
  uint64_t total_bitmap_alloc_cas_retries;
  uint64_t total_bitmap_free_cas_retries;
  uint64_t total_current_partial_cas_failures;
  uint64_t total_bitmap_alloc_attempts;
  uint64_t total_bitmap_free_attempts;
  uint64_t total_current_partial_cas_attempts;
  uint64_t total_lock_acquisitions;
  uint64_t total_lock_wait_time_ns;
  uint64_t total_lock_hold_time_ns;
  
  /* Derived global metrics */
  double avg_global_lock_wait_time_ns;   /* total_wait / total_acquisitions */
  double avg_global_lock_hold_time_ns;   /* total_hold / total_acquisitions */
} SlabGlobalStats;
```

---

## Prometheus Metrics

### New metrics (exported by tslab exporter)

```prometheus
# CAS retry counters (per size class)
temporal_slab_bitmap_alloc_cas_retries_total{size_class="128"} 1250
temporal_slab_bitmap_free_cas_retries_total{size_class="128"} 380
temporal_slab_current_partial_cas_failures_total{size_class="128"} 42

# Denominators for correct ratios
temporal_slab_bitmap_alloc_attempts_total{size_class="128"} 100000
temporal_slab_bitmap_free_attempts_total{size_class="128"} 98000
temporal_slab_current_partial_cas_attempts_total{size_class="128"} 500

# Lock contention metrics (Tier 0 trylock probe)
temporal_slab_lock_fast_acquire_total{size_class="128"} 4850  # Trylock succeeded
temporal_slab_lock_contended_total{size_class="128"} 150      # Trylock failed, blocked
```

### Grafana queries for contention dashboard

**1. CAS retry rate per allocation (CORRECT DENOMINATOR):**
```promql
rate(temporal_slab_bitmap_alloc_cas_retries_total[1m]) 
  / 
rate(temporal_slab_bitmap_alloc_attempts_total[1m])
```

**2. CAS retry rate per free (CORRECT DENOMINATOR):**
```promql
rate(temporal_slab_bitmap_free_cas_retries_total[1m]) 
  / 
rate(temporal_slab_bitmap_free_attempts_total[1m])
```

**3. Lock contention rate (Tier 0 probe):**
```promql
rate(temporal_slab_lock_contended_total[1m]) 
  / 
(rate(temporal_slab_lock_fast_acquire_total[1m]) + rate(temporal_slab_lock_contended_total[1m]))
```

**5. Current_partial CAS failure rate (promotion contention):**
```promql
rate(temporal_slab_current_partial_cas_failures_total[1m])
  /
rate(temporal_slab_current_partial_cas_attempts_total[1m])
```

---

## Grafana Dashboard Row: Contention

### Panel 1: CAS Retry Rate (Per Allocation)

**Query:**
```promql
sum by (size_class) (
  rate(temporal_slab_bitmap_alloc_cas_retries_total[1m])
    /
  rate(temporal_slab_bitmap_alloc_attempts_total[1m])
)
```

**Visualization:** Line chart, X-axis: time, Y-axis: retry ratio (0.0-1.0), Legend: size_class

**Note:** Thread count comparison requires separate runs (threads label only on benchmark metrics, not allocator metrics)

**Expected behavior:**
- 1 thread: ~0 retries (no contention)
- 2-4 threads: Low retries (occasional contention)
- 8+ threads: Higher retries (contention scaling)

### Panel 2: Lock Contention Rate (Tier 0 Trylock Probe)

**Query:**
```promql
rate(temporal_slab_lock_contended_total[1m]) 
  / 
(rate(temporal_slab_lock_fast_acquire_total[1m]) + rate(temporal_slab_lock_contended_total[1m]))
* 100
```

**Visualization:** Line chart, X-axis: time, Y-axis: contention % (0-100), Legend: size_class

**Expected behavior:**
- 1 thread: ~0% (all trylock succeed)
- 2-4 threads: <5% (occasional blocking)
- 8+ threads: 10-30% (contention scaling with threads)

**Thresholds:** Green <5%, Yellow 5-20%, Red >20%

**NOTE:** Measures occurrence, not duration. For wait time, use Tier 1 sampled timing (future work).

### Panel 3: Fast-Path CAS Failure Rate (CORRECTED)

**Query:**
```promql
rate(temporal_slab_current_partial_cas_failures_total[1m])
  /
rate(temporal_slab_current_partial_cas_attempts_total[1m])
* 100
```

**Visualization:** Gauge, Units: %, Thresholds: Green <5%, Yellow 5-20%, Red >20%

**Help text:** "Failures to update current_partial pointer (promotion/demotion/clear). High rate indicates contention or state mismatches."

**Expected behavior:**
- <5%: Healthy (low current_partial contention)
- 5-20%: Moderate (acceptable under load)
- >20%: High contention (consider per-thread caching)

---

## Multi-Thread Test Scenarios

### Scenario 1: Hotspot (all threads, same epoch, same size)

```bash
./synthetic_bench --threads=8 --pattern=hotspot --size=128 --duration_s=60
```

**Expected contention signatures:**
- High bitmap_alloc_cas_retries (8 threads competing for bitmap slots)
- Moderate current_partial_cas_failures (8 threads swapping current_partial)
- Low lock_hold_time (no list operations, just fast-path CAS)

### Scenario 2: Steady (all threads, rotating epochs)

```bash
./synthetic_bench --threads=8 --pattern=steady --size=128 --duration_s=60
```

**Expected contention signatures:**
- Low bitmap_alloc_cas_retries (threads spread across epochs)
- Low current_partial_cas_failures (per-epoch current_partial isolation)
- Low lock_hold_time (epochs drain independently)

### Scenario 3: Straggler (7 threads active, 1 thread stopped)

```bash
./synthetic_bench --threads=8 --pattern=straggler --duration_s=60
```

**Expected contention signatures:**
- 7 active threads: Normal CAS retry rates
- 1 straggler: Zero activity (no retries)
- Global RSS: Straggler epoch holds memory (old slabs not drained)

### Scenario 4: Cross-Thread Free (allocate on thread A, free on thread B)

```bash
./synthetic_bench --threads=8 --pattern=cross_thread_free --duration_s=60
```

**Expected contention signatures:**
- High bitmap_free_cas_retries (cross-thread free contention on bitmap)
- Moderate lock_hold_time (list transitions from full→partial)
- Proof: Cross-thread free is safe (no crashes, correct refcounts)

---

## Implementation Checklist

### Step 1: Add counters to SizeClassAlloc
- [ ] Add 5 new _Atomic uint64_t fields to slab_alloc_internal.h:143+
- [ ] Initialize to 0 in allocator_init()

### Step 2: Instrument CAS loops
- [ ] Add retry counter to slab_alloc_slot_ctz()
- [ ] Add retry counter to slab_free_slot_atomic()
- [ ] Accumulate retries after loop exit

### Step 3: Instrument current_partial CAS sites
- [ ] Track failures in alloc_obj_epoch fast path
- [ ] Track failures in free_obj recycling path
- [ ] Track failures in list transition paths

### Step 4: Instrument lock hold time
- [ ] Wrap all pthread_mutex_lock(&sc->lock) calls with timing
- [ ] Accumulate hold time (not acquisition latency)

### Step 5: Extend stats API
- [ ] Add fields to SlabClassStats
- [ ] Add fields to SlabGlobalStats
- [ ] Update slab_stats_class() to populate new fields
- [ ] Update slab_stats_global() to aggregate new counters

### Step 6: Update stats_dump JSON output
- [ ] Add contention fields to per-class JSON
- [ ] Add contention totals to global JSON
- [ ] Compute derived metrics (avg retry rate, avg hold time)

### Step 7: Update tslab Prometheus exporter
- [ ] Add 5 new metric families
- [ ] Export per-class contention counters
- [ ] Add help text explaining each metric

### Step 8: Add Grafana dashboard row
- [ ] Panel: CAS retry rate vs threads (line chart)
- [ ] Panel: Lock hold time distribution (heatmap)
- [ ] Panel: Fast-path CAS failure rate (gauge)

### Step 9: Implement multi-thread test scenarios
- [ ] Hotspot pattern (all threads, same epoch)
- [ ] Steady pattern (threads rotate epochs)
- [ ] Straggler pattern (1 thread stops)
- [ ] Cross-thread free pattern (alloc on A, free on B)

### Step 10: Document findings
- [ ] Update CONTENTION_RESULTS.md with empirical measurements
- [ ] Update semantics.md with cross-thread free contract confirmation
- [ ] Add grafana screenshots showing contention under load

---

## Cost Analysis

**Memory overhead:**
- 5 counters × 8 bytes × 8 size classes = 320 bytes total (negligible)

**CPU overhead (worst case):**
- CAS retry tracking: 1 atomic add per allocation/free (only if retries > 0)
- current_partial tracking: 1 atomic add per CAS failure (~5% of allocations)
- Lock timing: 3× now_ns() calls per lock acquisition (~60-90ns overhead)

**Estimated total overhead:** <2% CPU with ENABLE_LOCK_TIMING, <0.01% memory

**Mitigation strategies for lock timing overhead:**

1. **Compile-time gate** (recommended):
```c
#ifdef ENABLE_LOCK_TIMING
  uint64_t t_before = now_ns();
  pthread_mutex_lock(&sc->lock);
  uint64_t t_acquired = now_ns();
  // ... timing logic ...
#else
  pthread_mutex_lock(&sc->lock);
#endif
```

2. **Sampling (1/N acquisitions)**:
```c
static _Atomic uint64_t sample_counter = 0;
uint64_t count = atomic_fetch_add_explicit(&sample_counter, 1, memory_order_relaxed);

if ((count & 0x3F) == 0) {  /* Sample 1/64 */
  uint64_t t_before = now_ns();
  pthread_mutex_lock(&sc->lock);
  uint64_t t_acquired = now_ns();
  /* Scale by 64 */
  atomic_fetch_add_explicit(&sc->lock_wait_time_ns, (t_acquired - t_before) * 64, memory_order_relaxed);
}
```

3. **Use CLOCK_MONOTONIC_COARSE** (Linux, ~1ns cost but µs resolution):
```c
uint64_t now_ns_coarse(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
```

**When to disable:**
- Production: Keep enabled with ENABLE_LOCK_TIMING (sub-2% overhead, invaluable diagnostics)
- Benchmarking: Compile with `-DDISABLE_CONTENTION_METRICS` for pure perf
- Hot paths: Use sampling (1/64) if overhead becomes measurable

---

---

## Implementation Status + Ship-Ready Checklist

### ✓ Implemented Today
1. Slab registry with 24-bit generation (ABA protection)
2. Handle encoding with slab_id + generation
3. auto_close with era validation
4. epoch_domain_destroy semantics (frees domain struct, optionally calls epoch_close)
5. Concurrency-safe reclamation (emptiness validated under sc->lock)

### ✓ Phase 2.2 Implementation Status

**Completed (Tasks 1-7):**
1. ✅ Added 11 atomic counters to SizeClassAlloc (9 CAS + 2 lock probe)
2. ✅ Instrumented bitmap allocation CAS retry loops (2 sites: fast + slow path)
3. ✅ Instrumented bitmap free CAS retry loops (1 site: all frees)
4. ✅ Instrumented current_partial CAS sites (3 sites: promotion/demotion/clear)
5. ✅ **Implemented Tier 0 lock probe (HFT-optimized, always-on, zero jitter)**
6. ✅ Updated SlabClassStats + SlabGlobalStats structs with 11 new fields
7. ✅ Exported via stats_dump JSON (all metrics flowing)

**Remaining (Tasks 8-11):**
8. [ ] Update tslab Prometheus exporter (11 new metric families)
9. [ ] Add Grafana dashboard contention row (4 panels)
10. [ ] Run multi-thread tests ({1,2,4,8,16} threads × 4 patterns)
11. [ ] Write CONTENTION_RESULTS.md with empirical curves

**Actual effort so far:** ~8 hours (core instrumentation complete)

---

## Tier 0 Lock Probe: HFT-Optimized Design

### Why NOT Full Lock Timing (clock_gettime)?

**Problem with always-on timing:**
- `clock_gettime(CLOCK_MONOTONIC)` costs ~60ns per call (3 calls per lock = 180ns overhead)
- Adds ~2% throughput tax, but worse: adds **jitter** to tail latency
- In HFT, variance >> mean. Unpredictable 50µs P99 spikes kill performance.

**Solution: Trylock probe (occurrence, not duration)**

### Implementation (Always-On, Zero Jitter)

```c
#define LOCK_WITH_PROBE(mutex, sc) do { \
  if (pthread_mutex_trylock(mutex) == 0) { \
    atomic_fetch_add_explicit(&(sc)->lock_fast_acquire, 1, memory_order_relaxed); \
  } else { \
    atomic_fetch_add_explicit(&(sc)->lock_contended, 1, memory_order_relaxed); \
    pthread_mutex_lock(mutex); \
  } \
} while (0)
```

**Cost:** ~2ns best case (trylock succeeds), same as normal lock on contention  
**Jitter:** None - no syscalls, just CPU instructions + atomic increment  
**Overhead:** <0.1% CPU, negligible compared to lock hold time

### What It Measures

**Answers:**
- "Are threads blocking on this lock?" (yes/no signal)
- "Is contention increasing with thread count?" (trend over time)
- "Which size classes have lock bottlenecks?" (per-class breakdown)

**Does NOT answer:**
- "How long did threads wait?" (use Tier 1 sampled timing for that)
- "What's the P99 wait time?" (requires histogram, future work)

### Instrumented Lock Sites (6 hot paths)

1. **Line 874** - Fast-path full transition (after alloc from current_partial)
2. **Line 922** - Slow-path slab acquisition (cache miss or NULL current_partial)
3. **Line 965** - Slow-path full transition (after alloc from published slab)
4. **Line 1048** - Free path empty transition (epoch-aware recycling decision)
5. **Line 1105** - Free path full→partial (slab regained free slot)
6. **Line 1358** - epoch_close() scanning (reclamation critical path)

**Skipped sites:** Cache operations (rarely contended), destroy paths (cold), label writes (metadata, cold)

### Derived Metric: Lock Contention Rate

```c
lock_contention_rate = lock_contended / (lock_fast_acquire + lock_contended)
```

**Interpretation:**
- `0.0` - No contention (all trylock succeeded)
- `0.01` - 1% of lock acquisitions blocked (light contention)
- `0.20` - 20% blocked (significant contention, investigate)
- `0.50+` - Heavy contention (lock is bottleneck, consider per-thread caching)

### Prometheus Alert Rules (with divide-by-zero guards)

```yaml
- alert: HighCASRetryRate
  expr: |
    (rate(temporal_slab_bitmap_alloc_cas_retries_total[5m]) / rate(temporal_slab_bitmap_alloc_attempts_total[5m])) > 0.20
    AND rate(temporal_slab_bitmap_alloc_attempts_total[5m]) > 100
  annotations:
    summary: "CAS retry rate >20% (contention detected)"

- alert: HighLockContention
  expr: |
    (rate(temporal_slab_lock_contended_total[5m]) / 
     (rate(temporal_slab_lock_fast_acquire_total[5m]) + rate(temporal_slab_lock_contended_total[5m])))
    > 0.20
    AND (rate(temporal_slab_lock_fast_acquire_total[5m]) + rate(temporal_slab_lock_contended_total[5m])) > 100
  annotations:
    summary: "Lock contention rate >20% (threads blocking frequently)"
```

---

## Future Work

### Phase 2.X+1: Per-Thread Contention Breakdown

**Goal:** Attribute contention to specific threads (identify stragglers).

**Approach:**
- Thread-local CAS retry counters
- Export per-thread metrics to Prometheus
- Dashboard: Heatmap showing per-thread retry rates

**Blocker:** Requires thread registration (not yet implemented).

### Phase 2.X+2: Lock Contention Heatmap

**Goal:** Visualize lock hold time distribution over time.

**Approach:**
- Histogram buckets for lock hold time (1ns, 10ns, 100ns, 1µs, 10µs)
- Export histogram to Prometheus
- Grafana heatmap panel

**Blocker:** Requires histogram infrastructure (stats_dump doesn't support histograms yet).

---

## References

- `docs/semantics.md` - Epoch/domain contract (confirms cross-thread free is allowed)
- `OBSERVABILITY_DESIGN.md` - Phase 2 observability roadmap
- `slab_alloc.c:300-450` - CAS loop implementations (bitmap allocation/free)
- `slab_alloc.c:850-900` - current_partial CAS sites (fast-path promotion)

---

**Maintainer:** blackwd  
**Status:** Design complete, ready for implementation  
**Estimated effort:** 4-6 hours (instrumentation + dashboard + testing)
