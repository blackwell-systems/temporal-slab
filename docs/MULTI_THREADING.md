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

3. **epoch_advance() / epoch_close()** - Thread-safe state transitions
   - Atomic epoch state updates (ACTIVE → CLOSING)
   - Null `current_partial` pointers to prevent new allocations
   - Mutex-protected slab list scanning

**Synchronization points:**

| Operation | Fast Path (Lock-Free) | Slow Path (Mutex) |
|-----------|----------------------|-------------------|
| **Allocation** | Atomic load + CAS bitmap | Pick/create slab, promote PARTIAL→FULL |
| **Free** | CAS bitmap | Demote FULL→PARTIAL, recycle empty slabs |
| **Epoch transition** | Atomic state change | Null pointers, scan lists |

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

**When to use:** **Always-on in production** for HFT systems

**Answers:**
- "Are threads blocking on this lock?" (yes/no)
- "How much CAS contention exists?" (retries per operation)
- "Which size classes have contention?" (per-class attribution)

**Does NOT answer:**
- "How long did threads block?" (duration not measured)
- "What is P99 lock hold time?" (no timing data)

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

### HFT Production Policy

**Always use Tier 0, never Tier 1 or Tier 2.**

**Rationale:**
- **Variance >> mean in HFT:** A single 200ns clock_gettime jitter spike violates SLA
- **Tier 0 is zero-jitter:** Trylock is a single CPU instruction (~2ns, deterministic)
- **Occurrence data is actionable:** Knowing contention exists is sufficient for tuning

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

**Key validation:**
- ✅ Lock contention <15% at peak (16 threads)
- ✅ CAS retry rate <0.01 at peak (16 threads)
- ✅ CoV decreases with thread count (more predictable at scale)

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

**GitHub Actions validation (native x86_64 Linux):**
- 16 threads: 14.78% lock contention (median), 5.7% CoV (extremely consistent)
- 16 threads: 0.0074 CAS retries/op (well below 0.01 threshold)
- Contention plateau confirms healthy lock-free design

**When to tune:**
- Lock contention >20%: Consider per-thread caches or larger slabs
- CAS retries >0.05: Increase slab size or pre-allocate
- Throughput not scaling: Check hot size classes, epoch churn

**Further reading:**
- `CONTENTION_RESULTS.md` - Empirical validation data
- `CONTENTION_METRICS_DESIGN.md` - Tier 0 probe design rationale
- `PERFORMANCE_TUNING.md` - HFT-specific tuning strategies
