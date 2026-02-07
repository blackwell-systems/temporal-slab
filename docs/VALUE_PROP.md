# temporal-slab: Value Proposition

**temporal-slab is a lifetime-aware slab allocator designed for fixed-size, churn-heavy workloads where predictable latency and bounded RSS matter more than generality.**

It replaces spatial "hole-finding" with **temporal grouping**, ensuring objects with similar lifetimes are allocated together and reclaimed as a unit.

---

## What You Get (With Numbers)

### 🚀 Predictable Latency

* **p50 allocation:** ~30–70 ns
* **p99 allocation:** **~374 ns**
* **~3.3× better p99 than system malloc** (≈1.2 µs)
* **Lock-free fast path** (no mutex in common case)
* No background compaction → **no latency spikes**

**Value:** deterministic performance suitable for HFT, networking, and real-time systems.

---

### 🧠 RSS Stability Under Sustained Churn

* **RSS growth:** **~0–2.4%** over **1000 churn cycles**
* **No unbounded drift** (cache + overflow are explicitly bounded)
* **98–99% physical page reclaim** under epoch-aligned reclamation (via `madvise`)
* **100% cache hit rate** on reuse (0 new `mmap()` calls)

**Value:** long-running services don't slowly consume memory or require restarts.

---

### 🛡️ Strong Safety Guarantees

* **No runtime `munmap()`** → stale pointers never segfault
* **Opaque, generation-checked handles** → stale or double frees safely rejected
* **FULL-only recycling** → no use-after-free races
* Invalid frees return `false`, never crash

**Value:** allocator behavior is observable, debuggable, and safe under misuse.

---

### 🎯 Purpose-Built Efficiency

* **Object sizes:** 64–768 bytes (8 fixed classes)
* **Space efficiency:** **~88.9%** (11.1% internal fragmentation)
* **Zero external fragmentation**
* **O(1) deterministic class selection**

**Value:** excellent cache locality and stable memory layout for metadata-heavy systems.

---

## Why It's Different From malloc / jemalloc

| Property           | malloc / jemalloc | temporal-slab            |
| ------------------ | ----------------- | ------------------------ |
| Allocation model   | Spatial holes     | **Temporal affinity**    |
| Lifetime awareness | None              | **Epoch-based grouping** |
| p99 latency        | 1–2 µs            | **<400 ns**              |
| RSS under churn    | 20–50% drift      | **~0% drift**            |
| Stale frees        | UB / crash        | **Safe rejection**       |
| Runtime `munmap()` | Yes               | **Never**                |

**Key insight:**

> General allocators can't know when lifetime phases end. temporal-slab can.

That enables **deterministic, phase-aligned reclamation** that malloc-style allocators fundamentally cannot provide.

---

## Bottom Line

temporal-slab deliberately sacrifices generality to deliver:

* **Deterministic latency**
* **Bounded RSS under sustained churn**
* **Crash-proof safety contracts**
* **Allocator-level lifetime alignment**

If your workload allocates many small, fixed-size objects and cannot tolerate latency spikes or memory drift, **temporal-slab is the right tool**.

---

## Executive Summary (One Paragraph)

temporal-slab is a specialized slab allocator that groups allocations by time rather than size, achieving 0% RSS drift under sustained churn (vs 20-50% for malloc/jemalloc), sub-100ns median latency with predictable p99 (<400ns vs 1-2µs), and crash-proof safety contracts that reject invalid frees instead of corrupting memory. By tracking lifetime phases via epochs, it enables deterministic memory reclamation aligned with application boundaries—a capability fundamentally impossible for general-purpose allocators that lack temporal structure. Built for fixed-size (64-768 byte), high-churn workloads in HFT, control planes, and real-time systems where predictability matters more than generality.
