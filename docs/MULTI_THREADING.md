# Multi-Threading in temporal-slab

Comprehensive guide to threading model, contention characteristics, and scaling behavior.

**Target audience:** Performance engineers, HFT developers, systems architects

**Prerequisites:** Understanding of atomic operations, CAS loops, and lock-free programming

---

## Table of Contents

1. [Threading Model](#threading-model)
2. [Contention Characteristics](#contention-characteristics)
3. [Observability Tiers](#observability-tiers)
4. [Performance Validation](#performance-validation)
5. [Diagnostic Patterns](#diagnostic-patterns)
6. [Tuning Strategies](#tuning-strategies)

---

## Threading Model

### Design Philosophy

temporal-slab is designed as a **concurrent slab allocator** with the following properties:

1. **Cross-thread free support** - Objects allocated in thread A can be freed in thread B
2. **Lock-free fast path** - Atomic `current_partial` pointer eliminates mutex in common case
3. **Lock-free bitmap operations** - CAS loops for slot allocation/freeing (zero syscalls)
4. **Bounded contention** - Mutex fallback for slow path (slab exhaustion, list management)

**Not a thread-local allocator:** temporal-slab does NOT use per-thread caches by default. All threads share the same size-class allocators. This design choice prioritizes:
- **Memory efficiency** - No per-thread fragmentation
- **Cross-thread free** - No ownership tracking needed
- **Predictable RSS** - Global reclamation policy

### Thread Safety Guarantees

**Provably safe operations:**

1. **alloc_obj_epoch()** - Thread-safe via atomic CAS loops
   - Atomic load of `current_partial` (acquire ordering)
   - CAS loop on bitmap (no race conditions)
   - Mutex-protected list promotion (PARTIAL→FULL)

2. **free_obj()** - Thread-safe via atomic CAS loops
   - CAS loop on bitmap (no race conditions)
   - Mutex-protected list demotion (FULL→PARTIAL)
   - Handle validation through registry (ABA-proof)
   - Empty slabs retained on partial list (deferred reclamation)

3. **epoch_advance() / epoch_close()** - Thread-safe state transitions
   - Atomic epoch state updates (ACTIVE → CLOSING)
   - Null `current_partial` pointers to prevent new allocations
   - Mutex-protected slab list scanning
   - Safe empty slab reclamation (quiescence point)

**Synchronization points:**

| Operation | Fast Path (Lock-Free) | Slow Path (Mutex) |
|-----------|----------------------|-------------------|
| **Allocation** | Atomic load + CAS bitmap | Pick/create slab, promote PARTIAL→FULL |
| **Free** | CAS bitmap | Demote FULL→PARTIAL, move empty slabs FULL→PARTIAL |
| **Epoch close** | Atomic state change | Scan lists, recycle empty slabs |

### Memory Ordering

**Critical ordering choices:**

```c
// Fast path: current_partial load (ACQUIRE)
Slab* cur = atomic_load_explicit(&es->current_partial, memory_order_acquire);
// ↑ Sees all writes before RELEASE store by previous allocator

// Bitmap CAS: ACQ_REL ordering
atomic_compare_exchange_weak_explicit(&bm[w], &x, desired,
    memory_order_acq_rel,  // Success: acquire + release barrier
    memory_order_relaxed); // Failure: no ordering needed

// Contention counters: RELAXED ordering (performance, not correctness)
atomic_fetch_add_explicit(&sc->lock_fast_acquire, 1, memory_order_relaxed);
```

**Why ACQ_REL for bitmap CAS:**
- **Acquire:** Sees writes from previous allocations (prevent double-alloc)
- **Release:** Publishes allocation to other threads (prevent double-free)

**Why RELAXED for contention counters:**
- No correctness dependency on ordering
- Observability-only (eventual consistency acceptable)
- Eliminates memory fence overhead

---

## Contention Characteristics

### Empirical Scaling (Validated 1→16 Threads on GitHub Actions)

**Environment:** GitHub Actions ubuntu-latest, native x86_64 Linux, 10 trials per thread count

**Lock contention scaling:**

| Threads | Lock Contention Rate (Median) | CoV | Interpretation |
|---------|------------------------------|-----|----------------|
| 1       | 0.00%                        | 0.0% | Single-threaded (baseline) |
| 4       | 10.96%                       | 56.5% | Light contention, early scaling variance |
| 8       | 13.19%                       | 13.7% | Moderate contention, stabilizing |
| 16      | 14.78%                       | 5.7% | **Plateaus at 15%, extremely consistent** |

**Key insight:** Contention **plateaus at 15%**, not exponential growth. Decreasing CoV (56.5% → 5.7%) shows **more predictable behavior under higher load**—critical for tail latency guarantees.

**CAS retry scaling (bitmap allocation):**

| Threads | Retries/Op (Median) | Interpretation |
|---------|-------------------|----------------|
| 1       | 0.0000            | Zero contention |
| 4       | 0.0025            | Negligible |
| 8       | 0.0033            | Excellent |
| 16      | 0.0074            | **Well below 0.05 threshold** |

**CAS retry scaling (bitmap free):**

| Threads | Retries/Op | Interpretation |
|---------|-----------|----------------|
| 1-16    | <0.0002   | Free path is uncontended (as expected) |

**Why free is uncontended:** Freeing typically happens in batch, less concurrent pressure than allocation.

**Note:** Earlier WSL2 measurements showed 18-20% lock contention and 0.043 CAS retries/op at 16 threads. Native Linux shows 30% lower lock contention and 70% lower CAS retry rates, confirming WSL2 introduces artificial contention from hypervisor scheduling.

### Contention Sources

**1. Lock contention (mutex-based):**

Occurs during:
- **Slab exhaustion** - current_partial is full, need to pick/create new slab
- **List management** - Promoting PARTIAL→FULL, demoting FULL→PARTIAL
- **Epoch transitions** - Scanning lists during epoch_close()

**Measured via Tier 0 probe:**
```c
if (pthread_mutex_trylock(mutex) == 0) {
  lock_fast_acquire++;  // No blocking
} else {
  lock_contended++;     // Had to block
  pthread_mutex_lock(mutex);
}
```

**2. CAS retry contention (lock-free):**

Occurs during:
- **Bitmap allocation** - Multiple threads allocating from same slab
- **Bitmap free** - Multiple threads freeing to same slab (rare)
- **current_partial swap** - Fast-path pointer updates

**Measured via retry counters:**
```c
while (1) {
  // CAS loop for bitmap allocation
  if (CAS success) {
    bitmap_alloc_attempts++;
    return idx;
  }
  bitmap_alloc_cas_retries++;  // Count retry
}
```

### Adaptive Bitmap Scanning (Phase 2.2+)

**Problem:** Under high contention, multiple threads scanning the same bitmap sequentially (0, 1, 2, ...) creates a **thundering herd** - all threads compete for the first free bit, amplifying CAS retry storms.

**Solution:** Adaptive controller that switches between **sequential** and **randomized** scanning based on observed CAS retry rate.

**How it works:**

```c
// Mode 0: Sequential scan (0, 1, 2, ...)
// Mode 1: Randomized scan (hash(thread_id) % words, then wrap)

// Controller checks every 262,144 allocations (no clock syscalls)
if ((alloc_count & 0x3FFFF) == 0) {
  double retry_rate = retries_delta / allocs_delta;
  
  if (mode == 0 && retry_rate > 0.30) {
    mode = 1;  // Switch to randomized (spread threads)
    dwell = 50;  // Prevent flapping
  } else if (mode == 1 && retry_rate < 0.10) {
    mode = 0;  // Switch back to sequential (better cache locality)
    dwell = 50;
  }
}
```

**Key design decisions:**

1. **Windowed deltas, not lifetime averages** - Fast convergence to new workload patterns
2. **Allocation-count triggered** - Zero clock syscalls (no jitter)
3. **TLS-cached offsets** - Hash computed once per thread (amortized cost)
4. **Hysteresis (dwell countdown)** - Prevents mode flapping under marginal conditions

**Thresholds:**

| Threshold | Value | Rationale |
|-----------|-------|-----------|
| **Enable randomized** | 0.30 retries/op | 30% of allocations retry = contention severe enough to justify randomization overhead |
| **Disable randomized** | 0.10 retries/op | Below 10%, sequential is faster (better cache locality) |
| **Dwell countdown** | 50 checks | ~13M allocations between switches (prevent flapping) |

**Observability:**

```c
SlabClassStats stats;
slab_stats_class(alloc, 2, &stats);

printf("Adaptive scanning state:\n");
printf("  Current mode:     %u (0=sequential, 1=randomized)\n", stats.scan_mode);
printf("  Total checks:     %u\n", stats.scan_adapt_checks);
printf("  Mode switches:    %u\n", stats.scan_adapt_switches);
printf("  CAS retry rate:   %.4f\n", stats.avg_alloc_cas_retries_per_attempt);
```

**When it helps:**

- **High thread count (8-16 threads)** - Sequential scan creates hotspots
- **Small slab count** - All threads competing for same 2-3 slabs
- **Bursty allocation** - Sudden load spikes trigger mode switch

**When it doesn't help:**

- **Single-threaded** - Always mode 0 (no contention)
- **Large slab pool** - Threads naturally spread across slabs
- **Low allocation rate** - Not enough samples to trigger adaptation

**Performance impact:**

- **Sequential mode:** ~2ns per allocation (cache-friendly linear scan)
- **Randomized mode:** ~5ns per allocation (hash + modulo + wrap logic)
- **Mode switch cost:** 0ns (purely observational, no synchronization)

**Critical insight:** This is a **reactive controller**, not predictive. It responds to past contention patterns, so there's a lag (up to 262K allocations) before adaptation. For workloads with sustained contention, this is perfect. For microsecond-scale bursts, the controller may not react in time.

### Label-Based Contention Attribution (Phase 2.3)

**Problem:** Aggregate contention metrics don't answer: **"Which workload phase is causing contention?"**

Example: Lock contention is 25%, but is it from request processing, background tasks, or GC?

**Solution:** Per-label contention counters that attribute lock/CAS operations to semantic domains.

**Requires compile flag:** `ENABLE_LABEL_CONTENTION=1`

**How it works:**

```c
// Application registers semantic labels
epoch_domain_t* request = epoch_domain_create(alloc, "request:abc123");
epoch_domain_enter(request);

// All allocations/frees now attributed to "request:abc123"
void* obj = slab_malloc(alloc, 128);
// → lock_fast_acquire_by_label[3]++  (label ID 3 = "request:abc123")

epoch_domain_exit(request);
```

**Label registry (bounded cardinality):**

- **16 label IDs** max (0-15)
- **ID 0:** Unlabeled (no active domain)
- **ID 1-15:** Registered labels (first-come-first-served)
- If registry full, new labels map to ID 0 ("other" bucket)

**Per-label metrics collected:**

```c
#ifdef ENABLE_LABEL_CONTENTION
typedef struct {
  uint64_t lock_fast_acquire_by_label[16];        // Trylock succeeded
  uint64_t lock_contended_by_label[16];           // Trylock failed (blocked)
  uint64_t bitmap_alloc_cas_retries_by_label[16]; // CAS retry loops (alloc)
  uint64_t bitmap_free_cas_retries_by_label[16];  // CAS retry loops (free)
} SlabClassStats;
#endif
```

**Diagnostic query:**

```c
SlabClassStats stats;
slab_stats_class(alloc, 2, &stats);

// Cross-reference label IDs with registry
for (uint8_t lid = 0; lid < 16; lid++) {
  const char* label = alloc->label_registry.labels[lid];
  if (label[0] == '\0') break;  // End of registered labels
  
  uint64_t total_ops = stats.lock_fast_acquire_by_label[lid] + 
                       stats.lock_contended_by_label[lid];
  double contention_rate = (double)stats.lock_contended_by_label[lid] / total_ops;
  
  printf("Label [%u] \"%s\": %.2f%% lock contention, %lu CAS retries\n",
         lid, label, contention_rate * 100.0, 
         stats.bitmap_alloc_cas_retries_by_label[lid]);
}
```

**Example output:**

```
Label [0] "(unlabeled)":       2.1% lock contention, 1,234 CAS retries
Label [1] "request:GET":      18.3% lock contention, 45,678 CAS retries  ← HOT
Label [2] "request:POST":      8.7% lock contention, 12,345 CAS retries
Label [3] "background:gc":     1.2% lock contention, 567 CAS retries
Label [4] "frame:render":     22.5% lock contention, 78,901 CAS retries  ← HOT
```

**Performance cost:**

- **Without flag:** 0ns (feature compiled out entirely)
- **With flag:** ~5ns per lock acquisition (TLS lookup + array index)
- **Hot-path overhead:** Acceptable for diagnostics, **not recommended for HFT production**

**When to enable:**

- **Incident investigation:** "Which workload is causing the contention spike?"
- **Capacity planning:** "How much contention does each request type generate?"
- **A/B testing:** "Did the new code path reduce contention?"

**When to disable:**

- **HFT production:** 5ns overhead unacceptable
- **Single-phase workloads:** All allocations from one domain (no attribution value)
- **Memory-constrained:** 16 × 4 × uint64_t per size class = 512 bytes overhead

### Slowpath Sampling (Phase 2.5)

**Problem:** Tail latency spikes (p99 > 1µs) could be from:
1. **Allocator work** (CAS storms, zombie repairs, lock contention)
2. **Scheduler interference** (WSL2 hypervisor, preemption, context switches)

Aggregate metrics can't distinguish these.

**Solution:** Probabilistic end-to-end sampling with **wall vs CPU time split**.

**Requires compile flag:** `ENABLE_SLOWPATH_SAMPLING=1`

**How it works:**

```c
// 1/1024 sampling (fast & modulo, no random number generation)
bool sample = ((++tls_sample_ctr & 0x3FF) == 0);

if (sample) {
  clock_gettime(CLOCK_MONOTONIC, &wall_start);         // Wall time
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start); // CPU time
}

void* obj = alloc_obj_epoch(alloc, size, epoch, &handle);

if (sample) {
  clock_gettime(CLOCK_MONOTONIC, &wall_end);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
  
  uint64_t wall_ns = wall_end - wall_start;  // Includes scheduler delays
  uint64_t cpu_ns = cpu_end - cpu_start;     // Only actual CPU work
  
  tls_stats.alloc_wall_ns_sum += wall_ns;
  tls_stats.alloc_cpu_ns_sum += cpu_ns;
  tls_stats.alloc_samples++;
}
```

**Key metric: wait_ns** (Phase 2.5)

The critical insight is the **wait time** metric:

```c
wait_ns = wall_ns - cpu_ns  // Scheduler interference
```

- **wait_ns > cpu_ns:** Scheduler interference dominates (WSL2/hypervisor preemption)
- **wait_ns < cpu_ns:** Allocator work dominates (CAS storms, lock contention)
- **wait_ns << cpu_ns:** Clean measurement (minimal OS interference)

**Truth table for tail attribution:**

| Condition | Diagnosis |
|-----------|-----------|
| `wait_ns < cpu_ns` | Allocator work dominates (CAS, locks, repairs) |
| `wait_ns ≈ cpu_ns` | Balanced (50% allocator, 50% scheduler) |
| `wait_ns > cpu_ns` | Scheduler interference dominates (OS preemption) |
| `wait_ns > cpu_ns × 2` | Severe hypervisor overhead (WSL2/VM) |
| `cpu_ns > wall_ns` | Clock skew (rare, ignore sample) |

**Per-thread statistics:**

```c
ThreadStats stats = slab_stats_thread();

if (stats.alloc_samples > 0) {
  uint64_t avg_wall = stats.alloc_wall_ns_sum / stats.alloc_samples;
  uint64_t avg_cpu = stats.alloc_cpu_ns_sum / stats.alloc_samples;
  
  printf("Allocation tail latency (1/1024 sampling):\n");
  printf("  Samples:        %lu (out of ~%lu total)\n", 
         stats.alloc_samples, stats.alloc_samples * 1024);
  printf("  Avg wall time:  %lu ns (max: %lu ns)\n", 
         avg_wall, stats.alloc_wall_ns_max);
  printf("  Avg CPU time:   %lu ns (max: %lu ns)\n", 
         avg_cpu, stats.alloc_cpu_ns_max);
  printf("  Wall/CPU ratio: %.2fx\n", (double)avg_wall / avg_cpu);
  
  if (avg_wall > avg_cpu * 2) {
    printf("  ⚠ WARNING: wall >> cpu suggests scheduler interference\n");
  }
}
```

**Zombie repair timing (Phase 2.5):**

Also tracks repair operations (when slabs on PARTIAL list are detected as full):

```c
if (stats.repair_count > 0) {
  uint64_t avg_repair_cpu = stats.repair_cpu_ns_sum / stats.repair_count;
  uint64_t avg_repair_wait = stats.repair_wait_ns_sum / stats.repair_count;
  
  printf("Zombie repair statistics:\n");
  printf("  Total repairs:     %lu\n", stats.repair_count);
  printf("  Avg CPU time:      %lu ns (max: %lu ns)\n",
         avg_repair_cpu, stats.repair_cpu_ns_max);
  printf("  Avg wait time:     %lu ns (max: %lu ns)\n",
         avg_repair_wait, stats.repair_wait_ns_max);
  printf("  Reasons:\n");
  printf("    Full bitmap:    %lu (free_count=0, bitmap full)\n", 
         stats.repair_reason_full_bitmap);
  printf("    List mismatch:  %lu (list_id wrong)\n", 
         stats.repair_reason_list_mismatch);
}
```

**Validated impact (Feb 10 2026, 16-thread adversarial test):**
- **Repair rate:** 0.0104% (1 per 9,639 allocations)
- **Repair timing:** 9-14µs avg CPU (CPU-bound list/bitmap work)
- **Reason attribution:** 100% `full_bitmap` (proves specific race condition)
- **Wait time:** Avg 272ns (repairs rarely scheduler-blocked)
- **Outliers:** Occasional 400µs wall spikes (WSL2 preemption during repair)

**Interpretation:** Self-healing works correctly. Repairs are rare (<0.02% under adversarial load), CPU-intensive (not blocking on I/O), and always triggered by the same publication race (`full_bitmap`). This validates that zombie partials are a benign race condition, not a correctness bug.

**Performance cost:**

- **Sampling overhead:** 1/1024 × (6 × clock_gettime) = ~350ns / 1024 = **0.34ns avg**
- **Jitter:** High (clock_gettime is 50-200ns variable)
- **TLS storage:** 9 × uint64_t = 72 bytes per thread

**When to enable:**

- **Incident investigation:** "Is tail latency from allocator or OS?"
- **WSL2 validation:** "How much hypervisor overhead exists?"
- **Repair diagnosis:** "Are invariant violations contributing to latency?"

**When to disable:**

- **HFT production:** Clock syscalls introduce unacceptable jitter
- **Low-latency paths:** Even 0.34ns average overhead adds up
- **After diagnosis:** Once root cause identified, disable for production

**Critical insight:** Slowpath sampling is a **diagnostic tool**, not a production monitoring solution. The wall/CPU split is invaluable for distinguishing allocator bugs from environmental noise, but the jitter makes it unsuitable for always-on monitoring in latency-sensitive systems.

### Healthy Contention Patterns

**Excellent (validated on GitHub Actions):**
- Lock contention <15% (GitHub Actions validated: 14.78% at 16 threads)
- Bitmap CAS retries <0.01 retries/op (GitHub Actions validated: 0.0074 at 16 threads)
- CoV decreases with load (more predictable behavior at scale)

**Normal (no action needed):**
- Lock contention 15-20%
- Bitmap CAS retries 0.01-0.05 retries/op
- current_partial CAS failures 80-100% (expected state mismatches)

**Moderate (monitor, may optimize):**
- Lock contention 20-30%
- Bitmap CAS retries 0.05-0.10 retries/op

**High (optimize immediately):**
- Lock contention >30% (threads blocking frequently)
- Bitmap CAS retries >0.10 retries/op (excessive retries)

---

## Observability Tiers

temporal-slab implements **tiered contention observability** to balance production safety with diagnostic depth.

### Tier 0: Occurrence Probe (Always-On, HFT-Safe)

**What:** Trylock probe pattern (zero jitter, zero syscalls)

**Overhead:** ~2ns per lock acquisition (single instruction)

**Jitter:** **Zero** (no clock_gettime calls)

**Metrics collected:**
- `lock_fast_acquire` - Trylock succeeded
- `lock_contended` - Trylock failed (had to block)
- `bitmap_alloc_cas_retries` - CAS retry loops
- `bitmap_free_cas_retries` - CAS retry loops
- `current_partial_cas_failures` - Pointer swap failures
- `scan_adapt.mode` - Current bitmap scanning mode (0=sequential, 1=randomized)
- `scan_adapt.checks` - Controller evaluations performed
- `scan_adapt.switches` - Mode transitions (sequential ↔ randomized)

**When to use:** **Always-on in production** for HFT systems

**Answers:**
- "Are threads blocking on this lock?" (yes/no)
- "How much CAS contention exists?" (retries per operation)
- "Which size classes have contention?" (per-class attribution)
- "Is adaptive scanning active?" (mode + switches count)

**Does NOT answer:**
- "How long did threads block?" (duration not measured)
- "What is P99 lock hold time?" (no timing data)
- "Which workload phase caused contention?" (needs Tier 0.5)

### Tier 0.5: Label-Based Attribution (Diagnostic Mode)

**What:** Per-label contention counters (semantic attribution)

**Overhead:** ~5ns per lock acquisition (TLS lookup + array index)

**Jitter:** **Zero** (no clock syscalls, purely counter increments)

**Requires:** `ENABLE_LABEL_CONTENTION=1` compile flag

**Metrics collected:**
- `lock_fast_acquire_by_label[16]` - Trylock succeeded per label
- `lock_contended_by_label[16]` - Trylock failed per label
- `bitmap_alloc_cas_retries_by_label[16]` - CAS retries per label
- `bitmap_free_cas_retries_by_label[16]` - Free CAS retries per label

**When to use:** **Incident investigation only**, not HFT production

**Answers:**
- "Which workload phase is causing contention?" (request, GC, render, etc.)
- "Does the new code path reduce contention?" (A/B testing)
- "How much contention per request type?" (capacity planning)

**Does NOT answer:**
- "How long did each phase block?" (no timing data)
- "What is P99 by label?" (needs Tier 1 + label)

**Trade-off:** 5ns overhead is acceptable for diagnostics, but 2.5× slower than Tier 0. Use only when you need to attribute contention to specific workload phases.

### Tier 1: Sampled Timing (Incident Mode Only)

**What:** clock_gettime() timing with 1/1024 sampling

**Overhead:** ~60ns per sampled lock (3× clock_gettime calls)

**Jitter:** **High** (syscall latency unpredictable, 50-200ns variance)

**Metrics collected:**
- `lock_hold_time_ns` - Duration holding mutex
- `lock_wait_time_ns` - Duration waiting for mutex
- Distribution histogram (P50/P99/P99.9)

**When to use:** **Incident investigation only**, never in HFT production

**Answers:**
- "How long are threads blocked?" (duration distribution)
- "What is P99.9 lock wait time?" (tail latency)

**Trade-off:** Adds 60ns × 1/1024 = 0.06ns average overhead, but **jitter is unacceptable for HFT**.

### Tier 2: Full Timing (Perf Lab Only)

**What:** clock_gettime() on every lock acquisition

**Overhead:** ~180ns per lock (3× clock_gettime calls)

**Jitter:** **Extreme** (unpredictable syscall latency)

**When to use:** **Dedicated performance labs only**, never in production

**Why exists:** Detailed profiling, lock hold time analysis, contention hotspot identification

### Tier 2.5: Slowpath Sampling (Allocator/OS Split)

**What:** Probabilistic end-to-end allocation timing with wall vs CPU time split

**Overhead:** 1/1024 × (6 × clock_gettime) = **0.34ns average**

**Jitter:** **High** (clock_gettime variance 50-200ns, even with sampling)

**Requires:** `ENABLE_SLOWPATH_SAMPLING=1` compile flag

**Metrics collected:**
- `alloc_samples` - Number of sampled allocations
- `alloc_wall_ns_sum/max` - Wall time (includes scheduler delays)
- `alloc_cpu_ns_sum/max` - CPU time (actual allocator work)
- `repair_count` - Zombie slab repairs detected
- `repair_wall_ns_sum/max` - Wall time spent in repairs

**When to use:** **Post-incident analysis only**

**Answers:**
- "Is tail latency from allocator or OS?" (compare `wait_ns` vs `cpu_ns`)
- "How much WSL2 hypervisor overhead exists?" (single metric: `wait_ns = wall_ns - cpu_ns`)
- "Are zombie repairs contributing to latency?" (separate repair timing with wait decomposition)
- "What percentage of tail is scheduler interference?" (`100 × wait_ns / wall_ns`)

**Does NOT answer:**
- "Which specific operation caused the spike?" (sampled, not traced)
- "What is P99.9 by size class?" (insufficient sample density)

**Critical use case:** Distinguishing allocator bugs from environmental noise.

- **If wait_ns > cpu_ns:** Problem is OS/hypervisor, not allocator code
- **If wait_ns < cpu_ns:** Problem is allocator work (CAS storms, lock contention, repairs)
- **If cpu_ns > 1µs AND wait_ns < cpu_ns:** Contention is real (not scheduler artifact)

**Phase 2.5 validation (Feb 10 2026):**

| Test | Threads | Avg CPU | Avg Wait | Finding |
|------|---------|---------|----------|----------|
| simple_test | 1 | 398ns | 910ns | WSL2 adds 2.3× overhead (70% wait) |
| contention_sampling_test | 8 | 3,200ns | 600ns | Contention is real (CPU 2× vs 1T) |
| zombie_repair_test | 16 | 1,400ns | 380ns | Repairs are CPU-bound (9-14µs) |

**Key insight from validation:**

- **Single-thread:** 70% of latency is scheduler (wait >> cpu)
- **Multi-thread:** 20% of latency is scheduler (cpu >> wait)
- **Proof:** Contention doubles CPU time (398ns → 3,200ns), not an artifact of WSL2
- **Conclusion:** wait_ns metric successfully separates allocator work from OS interference

### HFT Production Policy

**Always use Tier 0, optionally Tier 0.5 for diagnostics. Never Tier 1, 2, or 2.5 in production.**

**Rationale:**
- **Variance >> mean in HFT:** A single 200ns clock_gettime jitter spike violates SLA
- **Tier 0 is zero-jitter:** Trylock is a single CPU instruction (~2ns, deterministic)
- **Tier 0.5 is zero-jitter:** Counter increments only (~5ns, deterministic)
- **Occurrence data is actionable:** Knowing contention exists is sufficient for tuning
- **Label attribution aids diagnosis:** Tier 0.5 identifies which workload phase to optimize

**Diagnostic escalation path:**
1. **Tier 0 (always-on):** Detect contention existence, measure magnitude
2. **Tier 0.5 (incident mode):** Attribute contention to workload phases
3. **Tier 2.5 (post-mortem):** Distinguish allocator work from OS interference
4. **Tier 1/2 (perf lab):** Deep profiling in isolated environment

---

## Performance Validation

### Benchmark Setup

**Workload:** `benchmark_threads N` (N = 1, 2, 4, 8, 16 threads)

- 100,000 allocations per thread
- 128-byte objects (class 2)
- Cross-thread free (allocated in thread A, freed in thread B)
- Measure contention via Tier 0 probe

**Hardware:** AMD Ryzen 9 / Intel Xeon (representative HFT hardware)

### Results Summary (GitHub Actions Validation)

**Lock contention scaling:**

```
Threads:  1      4       8       16
Rate:    0.00%  10.96%  13.19%  14.78%
CoV:     0.0%   56.5%   13.7%   5.7%
```

**Interpretation:** Plateaus at 15%, not exponential. Decreasing CoV shows increasing consistency under load.

**CAS retry scaling (allocation):**

```
Threads:  1      4       8       16
Retries:  0.000  0.0025  0.0033  0.0074
```

**Interpretation:** Well below 0.01 retries/op across all thread counts. Excellent lock-free bitmap design.

**Phase 2.5 slowpath sampling validation (Feb 10 2026, WSL2):**

```
Test                         | Threads | Samples | Avg CPU  | Avg Wait | Repairs | Finding
-----------------------------|---------|---------|----------|----------|---------|------------------------
simple_test                  | 1       | 97      | 398ns    | 910ns    | 0       | WSL2 adds 2.3× overhead
contention_sampling_test     | 8       | 776     | 3,200ns  | 600ns    | 0       | CPU 2× vs 1T (contention real)
zombie_repair_test           | 16      | 768     | 1,400ns  | 380ns    | 83      | 0.01% repair rate (CPU-bound)
```

**Key findings:**
- **wait_ns metric validates scheduler vs allocator**: Single-thread shows 70% scheduler overhead, multi-thread shows 20%
- **Contention is real allocator work**: CPU time doubles (398ns → 3,200ns) under 8-thread load
- **Zombie repairs are benign**: 0.0104% rate (1 per 9,639 allocations), 9-14µs CPU-bound, 100% `full_bitmap` reason
- **Repair self-healing works**: No corruption, no crashes, system remains healthy

**Key validation:**
- ✅ Lock contention <15% at peak (16 threads)
- ✅ CAS retry rate <0.01 at peak (16 threads)
- ✅ CoV decreases with thread count (more predictable at scale)
- ✅ wait_ns metric separates scheduler from allocator work (Phase 2.5)
- ✅ Zombie repair rate <0.02% under adversarial load (Phase 2.5)

### Validation Methodology

**1. Baseline (1 thread):**
- Lock contention: 0.0% (expected, no concurrency)
- CAS retries: 0.000 (expected, no competition)

**2. Scaling test (2, 4, 8, 16 threads):**
- Monitor lock contention rate (should plateau <30%)
- Monitor CAS retry rate (should stay <0.10 retries/op)
- Monitor throughput scaling (should be near-linear up to 8 threads)

**3. Sustained load (10 minutes at peak threads):**
- Verify contention stays stable (no exponential growth)
- Verify RSS stays bounded (no fragmentation leaks)
- Verify P99 latency stays <200ns

---

## Diagnostic Patterns

### Pattern 1: Lock Contention Spike

**Symptom:**
```promql
# Lock contention rate suddenly jumps 10% → 40%
rate(temporal_slab_class_lock_contended_total[1m]) /
  rate(temporal_slab_class_lock_acquisitions_total[1m]) > 0.40
```

**Diagnosis steps:**

1. **Check thread count:** Did workload scale up?
   ```bash
   ps -eLf | grep <process> | wc -l  # Count threads
   ```

2. **Check size class distribution:**
   ```promql
   topk(5, rate(temporal_slab_class_lock_contended_total[5m]) by (object_size))
   ```
   - If one class dominates, workload is hot-spotting

3. **Check epoch state:**
   ```promql
   temporal_slab_closing_epoch_count
   ```
   - If many CLOSING epochs, epoch_close() may be blocking allocations

**Root causes:**
- **Thread count increase** - More concurrent allocators
- **Hot size class** - All threads allocating same object size
- **Epoch churn** - Frequent epoch_close() causing list scans
- **Slab exhaustion** - current_partial keeps going full (need larger slabs)

**Remediation:**
- Add per-thread caches (if cross-thread free not needed)
- Increase slab size for hot classes
- Reduce epoch_close() frequency
- Load-balance across multiple size classes

### Pattern 2: CAS Retry Storm

**Symptom:**
```promql
# CAS retry rate suddenly jumps 0.02 → 0.15 retries/op
rate(temporal_slab_class_bitmap_alloc_cas_retries_total[1m]) /
  rate(temporal_slab_class_bitmap_alloc_attempts_total[1m]) > 0.15
```

**Diagnosis steps:**

1. **Check bitmap contention per class:**
   ```bash
   ./tslab top --classes --by bitmap_alloc_cas_retries --n 5
   ```

2. **Check slab reuse frequency:**
   - High CAS retries + low slab count = same slabs reused heavily

3. **Check thread count vs slab count:**
   - 16 threads + 2 slabs = high contention (not enough slabs)

**Root causes:**
- **Insufficient slabs** - All threads competing for same 2-3 slabs
- **Small slab size** - Slabs fill quickly, forcing threads to share
- **Bursty allocation** - All threads allocate simultaneously

**Remediation:**
- Increase slab size (more slots per slab = less contention)
- Pre-allocate slabs (reduce slab creation spikes)
- Stagger thread start times (reduce burst)

### Pattern 3: current_partial CAS Failures (Normal!)

**Symptom:**
```promql
# current_partial CAS failure rate 80-100%
rate(temporal_slab_class_current_partial_cas_failures_total[1m]) /
  rate(temporal_slab_class_current_partial_cas_attempts_total[1m]) > 0.80
```

**Important:** This is **NORMAL**, not a problem!

**Why high failure rate is expected:**
- current_partial CAS fails for many reasons:
  - Slab was promoted to FULL (expected state mismatch)
  - Another thread nulled it (expected race)
  - Slab was recycled (expected lifecycle)
- **Only true contention** causes brief retry, most failures are one-shot

**When to investigate:**
- If failure rate suddenly drops to <50% (may indicate allocator starvation)
- If CAS attempts drop to zero (allocator stuck)

**Do NOT alert on this metric alone.**

---

## Tuning Strategies

### Strategy 1: Per-Thread Caches (Eliminate Contention)

**When:** Lock contention >30%, CAS retries >0.10, and cross-thread free NOT needed

**Implementation:**
```c
// Thread-local slab cache (no sharing)
__thread SlabAllocator* thread_local_alloc;

void thread_init() {
  thread_local_alloc = slab_allocator_create();
}

void* alloc(size_t size) {
  return slab_malloc(thread_local_alloc, size);
}
```

**Trade-offs:**
- **Pro:** Zero contention (each thread has own allocator)
- **Pro:** Zero lock overhead
- **Con:** Higher RSS (per-thread fragmentation)
- **Con:** Cannot free across threads
- **Con:** Memory not shared (each thread pre-allocates)

**Best for:** CPU-bound workers, no cross-thread communication

### Strategy 2: Larger Slabs (Reduce Contention Frequency)

**When:** Lock contention 20-30%, bitmap CAS retries 0.05-0.10

**Implementation:**
```c
// Increase SLAB_PAGE_SIZE (must be power of 2)
#define SLAB_PAGE_SIZE 8192   // Was 4096

// Or: Use larger size classes (more objects per slab)
static const uint32_t k_size_classes[] = {
  64, 128, 256, 512, 1024, 2048  // Fewer, larger classes
};
```

**Trade-offs:**
- **Pro:** More slots per slab = less lock contention
- **Pro:** Fewer slab transitions (PARTIAL→FULL less frequent)
- **Con:** Higher baseline RSS (larger slabs stay resident)
- **Con:** Lower space efficiency (more internal fragmentation)

**Best for:** High-throughput, latency-tolerant workloads

### Strategy 3: Epoch-Based Batching (Reduce Allocation Frequency)

**When:** Lock contention >20%, workload is bursty

**Implementation:**
```c
// Allocate batch of objects in same epoch
EpochId batch_epoch = epoch_current(alloc);

for (int i = 0; i < batch_size; i++) {
  objects[i] = slab_malloc_epoch(alloc, size, batch_epoch);
}

// Process batch...

// Reclaim entire batch at once
epoch_close(alloc, batch_epoch);
```

**Trade-offs:**
- **Pro:** Amortizes contention over batch (fewer locks per object)
- **Pro:** Better cache locality (objects allocated together)
- **Pro:** Deterministic reclamation (entire batch freed at once)
- **Con:** Requires batch-oriented workload
- **Con:** Longer per-object lifetime

**Best for:** Batch processing, request-scoped allocations

### Strategy 4: Load Balancing Across Size Classes

**When:** One size class has >80% of contention

**Implementation:**
```c
// Distribute objects across multiple size classes
void* alloc_distributed(size_t size) {
  // Round up to next power of 2 (spreads load)
  size_t rounded = next_pow2(size);
  return slab_malloc(alloc, rounded);
}
```

**Trade-offs:**
- **Pro:** Spreads contention across multiple locks
- **Pro:** More slabs in circulation (reduces per-slab pressure)
- **Con:** Higher space overhead (objects larger than needed)
- **Con:** May increase cache misses (less dense packing)

**Best for:** Workloads with size variance

---

## Thread Safety Validation

### Critical Bugs Fixed

#### Bug: Handle Invalidation from Premature Recycling (Feb 2026)

**Symptom:** Multi-threaded `smoke_tests` failed with "free_obj returns false" when using bulk alloc/free patterns.

**Root cause:** When a slab became fully empty, `free_obj()` immediately recycled it via `cache_push()`, incrementing the generation counter. This invalidated outstanding handles held by other threads.

**The race:**
```
Thread A: Allocates 500K objects, holds handles h[0..499999]
Thread B: Frees last object in slab X → slab empty
Thread B: cache_push(slab X) → generation++ (slab recycled)
Thread A: Tries to free handle to slab X
Thread A: reg_lookup_validate() → generation mismatch → FAIL
```

**Fix:** Empty slabs now stay on the partial list instead of immediate recycling:
- If on FULL list → move to PARTIAL (enable reuse)
- If on PARTIAL list → increment `empty_partial_count`
- Reclamation deferred to `epoch_close()` (safe quiescence point)

**Why safe:**
1. Handles remain valid until epoch closes
2. Empty slabs can be reused (performance benefit)
3. Generation only increments during epoch_close() when no outstanding allocations exist

**Verification:**
- smoke_tests: 3/3 runs PASS (was 0/3 before fix)
- Zombie repairs: 644 → 12 (98% reduction)
- All production benchmarks: PASS

**See:** `docs/DEBUGGING_SAGA.md` for complete debugging chronicle

---

### ThreadSanitizer (TSan) Validation

temporal-slab passes ThreadSanitizer with zero warnings:

```bash
cd src
make TSAN=1
./test_epochs       # Validates epoch isolation
./benchmark_threads 16  # Validates 16-thread contention
```

**What TSan validates:**
- Data race detection (atomic operations correct)
- Lock order validation (no deadlocks)
- Memory ordering verification (acquire/release barriers correct)

**See:** `src/TSAN_VALIDATION.md` for full TSan test results

### Demo: Threading Capabilities

Run the comprehensive threading demo:

```bash
cd src
./demo_threading.sh
```

**What it demonstrates:**
- Contention scaling (1→16 threads)
- Cross-thread free support
- Lock contention metrics
- CAS retry metrics
- ThreadSanitizer validation instructions

---

## Summary

**Key takeaways:**

1. **temporal-slab is thread-safe** - Cross-thread free supported, provably safe
2. **Lock-free fast path** - Atomic operations eliminate mutex in common case
3. **Bounded contention** - Plateaus at 15% on native Linux (GitHub Actions validated)
4. **Tier 0 probe is HFT-safe** - Zero jitter, <0.1% overhead, always-on
5. **Excellent scaling validated** - Lock 14.78%, CAS 0.0074 retries/op at 16 threads
6. **Predictable under load** - CoV decreases from 56.5% → 5.7% as threads increase
7. **Adaptive bitmap scanning** - Automatically switches sequential ↔ randomized based on CAS retry rate (0.30 enable / 0.10 disable)
8. **Label-based attribution** - Per-label contention tracking (ENABLE_LABEL_CONTENTION) for workload phase diagnosis
9. **Slowpath sampling** - Wall vs CPU time split (ENABLE_SLOWPATH_SAMPLING) to distinguish allocator work from OS interference

**GitHub Actions validation (native x86_64 Linux):**
- 16 threads: 14.78% lock contention (median), 5.7% CoV (extremely consistent)
- 16 threads: 0.0074 CAS retries/op (well below 0.01 threshold)
- Contention plateau confirms healthy lock-free design

**Adaptive features (Phase 2.2+):**
- Bitmap scanning automatically optimizes for contention patterns
- Sequential mode (mode 0): Best for low contention, cache-friendly
- Randomized mode (mode 1): Spreads threads under high contention (>0.30 retries/op)
- Zero-cost controller: Allocation-count triggered (no clock syscalls)

**Diagnostic features (compile-time optional):**
- **Label attribution (Tier 0.5):** Identify which workload phase causes contention (~5ns overhead)
- **Slowpath sampling (Tier 2.5):** Distinguish allocator bugs from OS noise (~0.34ns overhead, high jitter)
- Use for incident investigation only, not HFT production

**When to tune:**
- Lock contention >20%: Consider per-thread caches or larger slabs
- CAS retries >0.05: Increase slab size or pre-allocate
- CAS retries >0.30: Adaptive scanning should activate automatically (check scan_adapt.switches)
- Throughput not scaling: Check hot size classes, epoch churn, or enable label attribution to identify hot phases

**Compile flags for diagnostics:**
```bash
# Baseline (always-on Tier 0 metrics)
make

# Add label-based contention attribution (Tier 0.5)
make CFLAGS="$(CFLAGS) -DENABLE_LABEL_CONTENTION=1"

# Add wall vs CPU time sampling (Tier 2.5)
make CFLAGS="$(CFLAGS) -DENABLE_SLOWPATH_SAMPLING=1"

# Full diagnostics (incident investigation)
make CFLAGS="$(CFLAGS) -DENABLE_LABEL_CONTENTION=1 -DENABLE_SLOWPATH_SAMPLING=1"
```

**Further reading:**
- `CONTENTION_RESULTS.md` - Empirical validation data
- `CONTENTION_METRICS_DESIGN.md` - Tier 0 probe design rationale
- `PERFORMANCE_TUNING.md` - HFT-specific tuning strategies
- `workloads/README_SLOWPATH_SAMPLING.md` - Wall vs CPU diagnosis guide
