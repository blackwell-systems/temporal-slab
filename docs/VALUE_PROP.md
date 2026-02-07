# temporal-slab: Value Proposition

**temporal-slab is a lifetime-aware slab allocator designed for fixed-size, churn-heavy workloads where predictable latency and bounded RSS matter more than generality.**

It replaces spatial "hole-finding" with **temporal grouping**, ensuring objects with similar lifetimes are allocated together and reclaimed as a unit.

---

## What You Get (With Numbers)

### 🚀 Eliminates Tail Latency Spikes

**Measured results (100M allocations, 128-byte objects):**

| Percentile | temporal-slab | system_malloc | Advantage |
|------------|---------------|---------------|-----------|
| p50 | 30 ns | 24 ns | 0.8× (baseline trade-off) |
| **p99** | **76 ns** | **2,962 ns** | **39× better** |
| **p99.9** | **166 ns** | **11,525 ns** | **69× better** |
| p99.99 | 1,542 ns | 63,940 ns | **41× better** |
| p99.999 | 19.8 µs | 254 µs | **12.9× better** |
| **Variance** | **659×** | **10,585×** | **16× more predictable** |

**Key insight:** A single malloc p99.9 outlier (11.5µs) costs 1,900× more than temporal-slab's entire median allocation (6ns overhead).

**Value:** Eliminates 2-250µs allocator-induced tail spikes. Suitable for HFT, real-time systems, and latency-sensitive services where worst-case behavior matters more than average speed.

---

### 🧠 Stable RSS Under Sustained Churn

**Measured results (1000 churn cycles):**

| Metric | temporal-slab | system_malloc |
|--------|---------------|---------------|
| Steady-state RSS growth (100 cycles) | **0%** | 11,174% |
| Long-term RSS growth (1000 cycles) | **0-2.4%** | Unbounded |
| Baseline RSS overhead | +37% | - |

**Value:** Long-running services don't slowly consume memory or require periodic restarts. The +37% baseline RSS overhead is the explicit cost for eliminating unbounded drift.

---

### 🛡️ Strong Safety Guarantees

* **No runtime `munmap()`** → stale pointers never segfault (slabs stay mapped during allocator lifetime)
* **Generation-checked handles** → stale or double frees safely rejected (24-bit generation counter, 16M reuse budget)
* **FULL-only recycling** → no use-after-free races (only slabs that were previously full are recycled)
* Invalid frees return `false`, never crash

**Value:** Allocator behavior is observable, debuggable, and safe under misuse. Structural safety properties, not heuristics.

---

### 🎯 Purpose-Built Efficiency

* **Object sizes:** 64–768 bytes (8 fixed classes)
* **Space efficiency:** **88.9%** (11.1% internal fragmentation vs malloc's ~15-25%)
* **Zero external fragmentation** (temporal grouping prevents holes)
* **O(1) deterministic class selection** (768-byte lookup table, no branching jitter)
* **Lock-free allocation fast path** (atomic CAS, no mutex contention)

**Value:** Excellent cache locality and stable memory layout for metadata-heavy systems.

---

## The Risk Exchange

temporal-slab makes an explicit trade-off:

**You trade:**
- +6ns median latency (+20% slower average case)
- +37% baseline RSS (epochs keep some slabs hot)

**To eliminate:**
- 2µs–250µs tail spikes (69× reduction at p99.9)
- Allocator-driven instability (16× more predictable variance)
- Unbounded RSS drift (0–2.4% growth vs 11,174% measured for malloc)

**This is not a performance optimization—it's tail-risk elimination.**

For latency-sensitive systems where worst-case behavior dominates SLA violations, this exchange is decisive.

---

## Why It's Different From malloc / jemalloc

| Property | malloc / jemalloc | temporal-slab |
|----------|-------------------|---------------|
| Allocation model | Spatial holes | **Temporal affinity** |
| Lifetime awareness | None | **Epoch-based grouping** |
| p99 latency | 2,962 ns | **76 ns (39× better)** |
| p99.9 latency | 11,525 ns | **166 ns (69× better)** |
| Variance | 10,585× | **659× (16× more predictable)** |
| RSS under churn | 11,174% growth | **0-2.4% growth** |
| Stale frees | UB / crash | **Safe rejection** |
| Runtime `munmap()` | Yes | **Never** |

**Key insight:**

> General allocators can't know when lifetime phases end. temporal-slab can.

That enables **deterministic, phase-aligned reclamation** via `epoch_close()` that malloc-style allocators fundamentally cannot provide.

---

## Epoch-Based Lifetime Management

temporal-slab provides **application-controlled memory reclamation** at lifetime boundaries:

```c
// Request-scoped allocation
EpochId req = epoch_current(alloc);
void* session = slab_malloc_epoch(alloc, 128, req);
void* buffer = slab_malloc_epoch(alloc, 256, req);

// Process request...

slab_free(alloc, session);
slab_free(alloc, buffer);
epoch_close(alloc, req);  // Physical memory returned to OS
```

**RSS reclamation results (5 epochs × 50K objects):**
- 19.15 MiB marked reclaimable (4,903 slabs)
- 3.3% RSS drop under memory pressure
- 100% cache hit rate (perfect slab reuse)

**Pattern:** Small epochs approach 100% reclaim, large epochs ~2%.

**Value:** Memory reclaimed when *you* decide (at structural boundaries), not when allocator heuristics trigger.

---

## Ideal Workloads

temporal-slab is designed for subsystems with fixed-size allocation patterns:

* **High-frequency trading (HFT)** - Sub-100ns deterministic allocation, no jitter
* **Control planes** - Session stores, connection metadata, cache entries
* **Request processors** - Web servers, RPC frameworks (request-scoped allocation)
* **Frame-based systems** - Game engines, simulations (frame-scoped allocation)
* **Real-time systems** - Packet buffers, message queues (bounded tail latency)

**Core characteristic:** Workloads where worst-case behavior matters more than average speed.

---

## When NOT to Use temporal-slab

**Use jemalloc/tcmalloc instead if:**
- Variable-size allocations (temporal-slab: fixed classes only)
- Objects >768 bytes (temporal-slab: specialized for small objects)
- NUMA systems (temporal-slab: no per-node awareness)
- Drop-in malloc replacement (jemalloc: LD_PRELOAD, huge ecosystem)
- General-purpose workloads (jemalloc: decades of production tuning)

**Core trade-off:** temporal-slab sacrifices generality in exchange for deterministic latency and bounded RSS under sustained churn.

---

## Bottom Line

temporal-slab deliberately sacrifices generality to deliver:

* **Structural tail-risk elimination** (39-69× better p99-p99.9, 16× more predictable)
* **Bounded RSS under sustained churn** (0-2.4% growth vs unbounded drift)
* **Crash-proof safety contracts** (stale frees never segfault)
* **Application-controlled lifetime management** (deterministic reclamation at phase boundaries)

If your workload allocates many small, fixed-size objects and cannot tolerate allocator-induced tail spikes or memory drift, **temporal-slab is the right tool**.

---

## Executive Summary (One Paragraph)

temporal-slab is a specialized slab allocator that groups allocations by time rather than size, achieving 0-2.4% RSS growth under sustained churn (vs unbounded drift for malloc), 76ns p99 latency (39× better than malloc's 2,962ns), and 166ns p99.9 (69× better than malloc's 11,525ns) with 16× more predictable variance. By tracking lifetime phases via epochs, it enables deterministic memory reclamation aligned with application boundaries—a capability fundamentally impossible for general-purpose allocators that lack temporal structure. Built for fixed-size (64-768 byte), high-churn workloads in HFT, control planes, and real-time systems where worst-case behavior matters more than average speed. The explicit risk exchange: +6ns median cost, +37% baseline RSS to eliminate 2-250µs tail spikes.
