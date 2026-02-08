# HFT Performance Tuning Guide

Production-ready performance tuning strategies for high-frequency trading and ultra-low-latency systems.

**Target audience:** HFT engineers, performance engineers, latency-sensitive systems

**Prerequisites:** Understanding of HFT performance requirements (variance >> mean), hardware performance counters, CPU cache behavior

---

## Table of Contents

1. [HFT Performance Philosophy](#hft-performance-philosophy)
2. [Observability Overhead Budget](#observability-overhead-budget)
3. [Contention Tuning](#contention-tuning)
4. [Cache Hit Optimization](#cache-hit-optimization)
5. [RSS vs Latency Trade-offs](#rss-vs-latency-trade-offs)
6. [Hardware-Specific Tuning](#hardware-specific-tuning)
7. [Production Deployment Checklist](#production-deployment-checklist)

---

## HFT Performance Philosophy

### Variance >> Mean (The HFT Axiom)

**In HFT systems, variance is more important than average performance.**

**Example:**
- Allocator A: 50ns median, 200ns P99 (4× variance)
- Allocator B: 55ns median, 11.5µs P99 (209× variance)

**Allocator B is 58× WORSE despite only 10% slower median.**

**Why:** A single P99.9 outlier (11.5µs) costs 1,900× more than the median overhead difference (5ns).

### Jitter Elimination

**Jitter sources in allocators:**

| Source | Variance | HFT Impact |
|--------|----------|------------|
| **clock_gettime()** | 50-200ns | ❌ UNACCEPTABLE (unpredictable syscall) |
| **Mutex contention** | 100ns-10µs | ⚠️ BOUNDED (predictable if <20% rate) |
| **Page faults** | 1-10µs | ❌ CRITICAL (must pre-fault) |
| **Cache misses** | 50-200ns | ⚠️ ACCEPTABLE (if bounded) |
| **Branch mispredicts** | 10-20ns | ✅ NEGLIGIBLE |

**temporal-slab eliminates high-variance jitter:**
1. ✅ Zero clock_gettime() calls in fast path (Tier 0 probe)
2. ✅ Lock-free fast path (atomic operations only)
3. ✅ O(1) deterministic class selection (no branches per class)
4. ✅ No syscalls in fast path (no page faults, no madvise)

### The Allocator Performance Hierarchy

**From most to least important for HFT:**

1. **Eliminate tail spikes** (variance reduction)
2. **Eliminate jitter** (deterministic worst-case)
3. **Optimize median** (average-case performance)
4. **Minimize RSS** (memory efficiency)

temporal-slab prioritizes 1→2→3→4 in that order.

---

## Observability Overhead Budget

### Tier 0: Zero-Jitter Probe (Always-On)

**Overhead:**
- **Median:** ~2ns per lock acquisition (single trylock instruction)
- **P99:** ~2ns (deterministic, no jitter)
- **Total:** <0.1% for typical workloads

**What it costs:**
```c
// Tier 0 probe (single instruction overhead)
if (pthread_mutex_trylock(mutex) == 0) {
  atomic_fetch_add(&lock_fast_acquire, 1, memory_order_relaxed);  // ~1 cycle
} else {
  atomic_fetch_add(&lock_contended, 1, memory_order_relaxed);      // ~1 cycle
  pthread_mutex_lock(mutex);  // Would happen anyway
}
```

**Cost breakdown:**
- Trylock: ~2ns (already needed for lock-free design)
- Atomic increment: ~1ns (memory_order_relaxed, no fence)
- **Total added cost:** ~1ns per lock

**Why HFT-safe:**
- Zero syscalls (no clock_gettime)
- Zero jitter (single CPU instruction)
- Memory_order_relaxed (no fence overhead)

**Production policy:** **ALWAYS ON**

### Tier 1: Sampled Timing (Incident-Only)

**Overhead:**
- **Median:** 0.06ns (sampled at 1/1024 rate)
- **P99:** 60ns (when sampled)
- **Jitter:** 50-200ns (clock_gettime variance)

**What it costs:**
```c
// Tier 1 probe (3× clock_gettime per lock)
if (should_sample()) {  // 1/1024 rate
  uint64_t t1 = now_ns();  // clock_gettime (~60ns, unpredictable)
  pthread_mutex_lock(mutex);
  uint64_t t2 = now_ns();  // clock_gettime (~60ns)
  // ... critical section ...
  uint64_t t3 = now_ns();  // clock_gettime (~60ns)
  
  wait_time = t2 - t1;
  hold_time = t3 - t2;
}
```

**Cost breakdown:**
- 3× clock_gettime: ~180ns per sample
- Sampled at 1/1024: ~0.18ns average
- **Jitter:** 50-200ns when sampled (UNACCEPTABLE for HFT)

**Production policy:** **NEVER ON in HFT prod**, incident investigation only

### Tier 2: Full Timing (Lab-Only)

**Overhead:**
- **Median:** ~180ns per lock
- **Total:** 5-10% overhead

**Production policy:** **NEVER in production**, dedicated perf labs only

### Tier Selection Matrix

| Environment | Tier 0 | Tier 1 | Tier 2 |
|-------------|--------|--------|--------|
| **HFT Production** | ✅ Always | ❌ Never | ❌ Never |
| **Staging/Pre-Prod** | ✅ Always | ⚠️ Incident | ❌ Never |
| **Dev/Test** | ✅ Always | ✅ Optional | ⚠️ Lab-Only |
| **Perf Lab** | ✅ Always | ✅ Yes | ✅ Yes |

---

## Contention Tuning

### Healthy Contention Thresholds

**Based on empirical validation (1→16 threads):**

| Metric | Green | Yellow | Red | Action |
|--------|-------|--------|-----|--------|
| **Lock contention rate** | <10% | 10-20% | >20% | Add per-thread caches |
| **Bitmap CAS retries/op** | <0.01 | 0.01-0.05 | >0.05 | Increase slab size |
| **Bitmap free retries/op** | <0.001 | 0.001-0.01 | >0.01 | Investigate cross-thread free hotspots |
| **Throughput scaling** | Linear | Sublinear | Plateau | Load-balance or cache |

### Tuning Strategy 1: Per-Thread Caches

**When:** Lock contention >20%, cross-thread free NOT needed

**Implementation:**

```c
// Option A: Thread-local allocator (zero sharing)
__thread SlabAllocator* thread_alloc = NULL;

void thread_init() {
  thread_alloc = slab_allocator_create();
}

void* alloc(size_t size) {
  return slab_malloc(thread_alloc, size);
}

// Option B: Per-thread epoch (shared allocator, isolated epochs)
__thread EpochId thread_epoch;

void thread_init(SlabAllocator* global_alloc) {
  thread_epoch = epoch_current(global_alloc);
  epoch_advance(global_alloc);  // Allocate private epoch
}

void* alloc(SlabAllocator* global_alloc, size_t size) {
  return slab_malloc_epoch(global_alloc, size, thread_epoch);
}
```

**Trade-offs:**

| Approach | Contention | RSS | Cross-Thread Free | Complexity |
|----------|-----------|-----|-------------------|------------|
| **Thread-local allocator** | Zero | High | ❌ No | Low |
| **Per-thread epoch** | Zero alloc | Medium | ✅ Yes | Medium |
| **Shared allocator** | ~20% | Low | ✅ Yes | Low |

**Recommendation for HFT:**
- **CPU-bound workers:** Thread-local allocator (zero contention)
- **Network I/O:** Per-thread epoch (allows cross-thread free)
- **Mixed workload:** Shared allocator (low RSS, bounded contention)

### Tuning Strategy 2: Slab Size Tuning

**When:** CAS retries 0.01-0.05 (moderate contention)

**Current slab size:** 4096 bytes (1 page)

**Tuning options:**

```c
// Option A: Increase page size (more slots per slab)
#define SLAB_PAGE_SIZE 8192   // 2 pages = 2× slots

// Option B: Use larger size classes (coarser granularity)
static const uint32_t k_size_classes[] = {
  128, 256, 512, 1024  // Fewer, larger classes
};
```

**Impact analysis:**

| Page Size | Slots (128B class) | Lock Contention | CAS Retries | RSS Overhead |
|-----------|-------------------|----------------|-------------|--------------|
| **4096B** | 27 slots | Baseline | Baseline | Baseline |
| **8192B** | 58 slots | -40% | -50% | +100% |
| **16384B** | 120 slots | -60% | -70% | +300% |

**Recommendation:**
- Start with 4096B (1 page)
- If contention >20%, try 8192B (2 pages)
- Measure RSS impact before committing

### Tuning Strategy 3: Pre-Allocation (Eliminate Cold Start)

**When:** First allocation shows latency spike (cold start penalty)

**Implementation:**

```c
// Pre-allocate slabs during startup (before trading begins)
void warm_cache(SlabAllocator* alloc) {
  EpochId epoch = epoch_current(alloc);
  
  // Allocate + free to populate slab cache
  for (size_t class_idx = 0; class_idx < NUM_CLASSES; class_idx++) {
    for (int i = 0; i < 32; i++) {  // 32 slabs per class
      void* obj = alloc_obj_epoch(alloc, k_size_classes[class_idx], epoch, NULL);
      free_obj(alloc, *(SlabHandle*)obj);
    }
  }
  
  // Cache now populated (no mmap during trading)
}
```

**Impact:**
- **Before:** First alloc: 2-5µs (mmap syscall)
- **After:** First alloc: 50ns (cache hit)

**Recommendation:** Always pre-allocate in HFT systems

---

## Cache Hit Optimization

### Target: 97%+ Cache Hit Rate

**Cache hit rate formula:**
```
hit_rate = 1 - (new_slab_count / (slow_path_hits + fast_path_hits))
```

**Monitoring:**
```promql
1 - (rate(temporal_slab_new_slab_count_total[5m]) /
     rate(temporal_slab_slow_path_hits_total[5m]))
```

### Cache Miss Diagnosis

**Symptom:** Cache hit rate <95%

**Diagnosis steps:**

1. **Check cache size:**
   ```promql
   temporal_slab_class_cache_size{class="2"}  # Should be <32 (default capacity)
   ```

2. **Check overflow:**
   ```promql
   temporal_slab_class_cache_overflow_len{class="2"}  # Should be 0-10
   ```

3. **Check recycling rate:**
   ```promql
   rate(temporal_slab_class_empty_slab_recycled_total[5m])
   ```

**Root causes:**

| Pattern | Cause | Fix |
|---------|-------|-----|
| Cache size always 32, overflow growing | Excessive recycling | Increase cache capacity |
| Cache size <10, new_slab_count high | Insufficient recycling | Call epoch_close() more frequently |
| Cache size varies wildly | Bursty workload | Pre-allocate slabs |

### Cache Capacity Tuning

**Default:** 32 slabs per size class (128KB total)

**Tuning:**

```c
// Increase cache capacity (reduce overflow)
a->classes[i].cache_capacity = 64;  // Was 32

// Or: Increase per-class based on hotness
if (is_hot_class(i)) {
  a->classes[i].cache_capacity = 128;  // Hot class
} else {
  a->classes[i].cache_capacity = 16;   // Cold class
}
```

**Trade-offs:**
- **Pro:** Higher hit rate (fewer mmap syscalls)
- **Con:** Higher baseline RSS (more cached slabs)

**Recommendation:** Start with 32, increase for hot classes if hit rate <97%

---

## RSS vs Latency Trade-offs

### The Fundamental Trade-off

**temporal-slab offers a dial between RSS and latency:**

| Configuration | Baseline RSS | Latency P99 | Cache Hit Rate |
|---------------|-------------|-------------|----------------|
| **Minimal RSS** | +20% | 200ns | 90% |
| **Balanced** | +37% | 76ns | 97% |
| **Ultra-Low Latency** | +50% | 50ns | 99.9% |

**How to tune:**

1. **Minimal RSS:** Small cache (16 slabs), frequent epoch_close()
2. **Balanced:** Default cache (32 slabs), moderate epoch_close()
3. **Ultra-Low Latency:** Large cache (64+ slabs), rare epoch_close()

### epoch_close() Timing Strategies

**Strategy 1: Request-Scoped (Ultra-Low Latency)**

```c
// One epoch per request (isolate RSS per request)
EpochId req_epoch = epoch_current(alloc);
epoch_advance(alloc);  // Next request gets new epoch

process_request(alloc, req_epoch);  // All allocations in req_epoch

epoch_close(alloc, req_epoch);  // Reclaim immediately
```

**Characteristics:**
- **Latency:** Minimal (no cross-request RSS buildup)
- **RSS:** Bounded per request
- **Overhead:** High (epoch_close() per request)

**Best for:** HFT order processing (1-10ms request duration)

**Strategy 2: Batch-Scoped (Balanced)**

```c
// One epoch per batch of N requests
EpochId batch_epoch = epoch_current(alloc);

for (int i = 0; i < batch_size; i++) {
  process_request(alloc, batch_epoch);
}

epoch_close(alloc, batch_epoch);  // Reclaim batch
```

**Characteristics:**
- **Latency:** Low (amortized epoch_close() cost)
- **RSS:** Bounded per batch
- **Overhead:** Medium

**Best for:** Batch processing (10-100 requests per batch)

**Strategy 3: Time-Scoped (Minimal Overhead)**

```c
// Close epoch every N seconds
if (now_ns() - last_close_ns > CLOSE_INTERVAL_NS) {
  epoch_close(alloc, last_epoch);
  last_epoch = epoch_current(alloc);
  epoch_advance(alloc);
  last_close_ns = now_ns();
}
```

**Characteristics:**
- **Latency:** Variable (RSS grows between closes)
- **RSS:** Grows then drops (sawtooth pattern)
- **Overhead:** Minimal

**Best for:** Long-running services (minutes-hours between closes)

### RSS Monitoring

**Key metrics:**

```promql
# RSS growth rate (should be flat or sawtooth)
rate(temporal_slab_rss_bytes[5m])

# RSS per epoch (identify leaks)
topk(5, temporal_slab_epoch_rss_bytes)

# Reclamation effectiveness
rate(temporal_slab_madvise_bytes_total[5m])
```

**Alert on:**
- Monotonic RSS growth (no sawtooth = no reclamation)
- Single epoch >50% of total RSS (leak candidate)
- madvise_bytes_total = 0 (reclamation not happening)

---

## Hardware-Specific Tuning

### CPU Cache Line Considerations

**Slab header size:** 64 bytes (exactly 1 cache line)

**Why this matters:**
- Header + bitmap fit in L1 cache (32KB)
- Atomic operations stay within cache line (no false sharing)

**False sharing prevention:**

```c
// Slab header is cache-line aligned (64 bytes)
struct Slab {
  // ... header fields (64 bytes total) ...
} __attribute__((aligned(64)));

// Bitmap immediately follows header (next cache line)
_Atomic uint32_t* bitmap = slab_bitmap_ptr(slab);  // 64-byte aligned
```

**Validation:**
```bash
# Check cache line alignment
./stats_dump | grep -A 5 "Slab header"
# Should show: "Header size: 64 bytes (aligned)"
```

### NUMA Considerations

**Problem:** Cross-NUMA allocation/free causes cache coherence traffic

**Solution:** NUMA-aware epoch allocation

```c
// Allocate per-NUMA-node allocator
SlabAllocator* numa_allocators[NUM_NUMA_NODES];

void init_numa() {
  for (int node = 0; node < NUM_NUMA_NODES; node++) {
    numa_allocators[node] = slab_allocator_create();
    // Pin allocator memory to NUMA node
    numa_tonode_memory(numa_allocators[node], sizeof(SlabAllocator), node);
  }
}

void* alloc(size_t size) {
  int node = numa_node_of_cpu(sched_getcpu());
  return slab_malloc(numa_allocators[node], size);
}
```

**Impact:**
- **Before:** 50-200ns cache coherence latency (cross-NUMA)
- **After:** 20-50ns (local NUMA node)

**Recommendation:** Only if profiling shows NUMA as bottleneck

### Huge Pages (2MB Pages)

**Problem:** TLB misses add 50-100ns latency

**Solution:** Use huge pages for slab allocations

```c
// Allocate with huge pages (Linux-specific)
void* map_one_page_huge(void) {
  void* p = mmap(NULL, SLAB_PAGE_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,  // Huge pages
                 -1, 0);
  return p;
}
```

**Requirements:**
- Enable huge pages: `echo 128 > /proc/sys/vm/nr_hugepages`
- 2MB alignment (SLAB_PAGE_SIZE must be 2MB multiple)

**Impact:**
- **Before:** 512 TLB entries (4KB pages) = TLB pressure
- **After:** 1 TLB entry (2MB page) = TLB hit

**Recommendation:** Measure TLB miss rate first (use `perf stat`)

---

## Production Deployment Checklist

### Pre-Deployment Validation

**1. Contention validation:**
```bash
# Run multi-thread benchmark
./benchmark_threads 16

# Verify contention metrics
./stats_dump | grep -E "lock_contention_rate|bitmap_alloc_cas"

# Expected: lock_contention_rate <0.20, cas_retries <0.05
```

**2. Latency validation:**
```bash
# Run latency benchmark (100M samples)
./benchmark_latency 100000000

# Verify P99/P99.9
# Expected: P99 <200ns, P99.9 <500ns
```

**3. RSS validation:**
```bash
# Run churn test (1000 cycles)
./churn_test 1000

# Verify RSS growth
# Expected: <5% growth over 1000 cycles
```

**4. ThreadSanitizer validation:**
```bash
# Compile with TSan
make TSAN=1

# Run all tests
./test_epochs
./benchmark_threads 16

# Expected: Zero TSan warnings
```

### Production Monitoring Setup

**1. Enable Tier 0 probe (always-on):**

Ensure `LOCK_WITH_PROBE` macro is active (default):
```c
#define LOCK_WITH_PROBE(mutex, sc) /* ... Tier 0 probe ... */
```

**2. Configure Prometheus exporter:**

```bash
# Push metrics every 10s
while true; do
  ./stats_dump --json --no-text | \
    ./tslab export prometheus --stdin | \
    curl --data-binary @- http://pushgateway:9091/metrics/job/temporal_slab
  sleep 10
done
```

**3. Import Grafana dashboard:**

```bash
# Import dashboards/temporal-slab-observability.json
# Includes 29 panels (contention + RSS + epoch telemetry)
```

**4. Configure alerts:**

```yaml
groups:
  - name: temporal_slab_hft
    rules:
      - alert: HighLockContention
        expr: |
          rate(temporal_slab_class_lock_contended_total[5m]) /
          rate(temporal_slab_class_lock_acquisitions_total[5m]) > 0.30
        annotations:
          summary: "Lock contention >30% (consider per-thread caches)"
      
      - alert: HighCASRetries
        expr: |
          rate(temporal_slab_class_bitmap_alloc_cas_retries_total[5m]) /
          rate(temporal_slab_class_bitmap_alloc_attempts_total[5m]) > 0.10
        annotations:
          summary: "CAS retries >0.10/op (consider larger slabs)"
      
      - alert: LowCacheHitRate
        expr: |
          1 - (rate(temporal_slab_new_slab_count_total[5m]) /
               rate(temporal_slab_slow_path_hits_total[5m])) < 0.95
        annotations:
          summary: "Cache hit rate <95% (increase cache capacity)"
```

### Production Tuning Workflow

**Week 1: Baseline measurement**
1. Deploy with default settings
2. Monitor contention metrics (Tier 0 probe always-on)
3. Record baseline: lock contention %, CAS retries/op, cache hit %

**Week 2: Identify hotspots**
1. Query Grafana: "Top classes by lock contention"
2. Query Grafana: "Top classes by CAS retries"
3. Identify 1-2 hot size classes

**Week 3: Tune hot classes**
1. If lock contention >20%: Add per-thread caches
2. If CAS retries >0.05: Increase slab size
3. If cache hit <97%: Increase cache capacity

**Week 4: Validate improvement**
1. Re-run benchmarks
2. Compare before/after metrics
3. Document tuning decisions

---

## Summary

**HFT Performance Hierarchy:**
1. ✅ Eliminate tail spikes (39-69× better P99-P99.9)
2. ✅ Eliminate jitter (Tier 0 probe: zero clock_gettime)
3. ✅ Bounded contention (plateaus at 20%, not exponential)
4. ⚠️ RSS trade-off (+37% for deterministic behavior)

**Production Configuration (HFT):**
- **Observability:** Tier 0 probe always-on (zero jitter, <0.1% overhead)
- **Contention target:** Lock <20%, CAS <0.05 retries/op
- **Cache target:** 97%+ hit rate
- **Epoch strategy:** Request-scoped (minimize RSS buildup)

**When to tune:**
- Lock contention >30%: Add per-thread caches
- CAS retries >0.10: Increase slab size (4KB → 8KB)
- Cache hit <95%: Increase cache capacity (32 → 64 slabs)
- RSS growing unbounded: Call epoch_close() more frequently

**Further reading:**
- `MULTI_THREADING.md` - Threading model and contention characteristics
- `CONTENTION_RESULTS.md` - Empirical validation data (1→16 threads)
- `docs/results.md` - Tail latency benchmarks vs malloc
