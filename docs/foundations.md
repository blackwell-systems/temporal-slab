# Foundations: Memory Allocation and Lifetime-Aware Design

This document builds the theoretical foundation for temporal-slab from first principles, defining each concept before using it to explain the next. It assumes no prior knowledge of memory allocator internals.

## Table of Contents

**Operating System Concepts**
- [Page](#page)
- [Virtual Memory vs Physical Memory](#virtual-memory-vs-physical-memory)
- [Resident Set Size (RSS)](#resident-set-size-rss)

**Allocator Fundamentals**
- [Memory Allocator](#memory-allocator)
- [Spatial Fragmentation](#spatial-fragmentation)
- [Temporal Fragmentation](#temporal-fragmentation)
- [Fragmentation as Entropy](#fragmentation-as-entropy)

**Slab Allocation Model**
- [Lifetime Affinity](#lifetime-affinity)
- [Slab](#slab)
- [Size Class](#size-class)
- [Internal Fragmentation](#internal-fragmentation)
- [Slab Lifecycle](#slab-lifecycle)

**Implementation Techniques**
- [Lock-Free Allocation](#lock-free-allocation)
- [Bounded RSS Through Conservative Recycling](#bounded-rss-through-conservative-recycling)
- [Slab Cache](#slab-cache)
- [Refusal to Unmap](#refusal-to-unmap)
- [O(1) Deterministic Class Selection](#o1-deterministic-class-selection)

**API Design**
- [Handle-Based API vs Malloc-Style API](#handle-based-api-vs-malloc-style-api)
- [What You Should Understand Now](#what-you-should-understand-now)

---

## Page

A page is the smallest unit of memory the operating system manages. On most modern systems (x86-64, ARM64), a page is 4096 bytes (4KB). When a program requests memory, the OS allocates entire pages. The program may request 80 bytes, but the OS grants at least one page. Pages are the fundamental currency of memory management—they can be mapped into a process's address space, unmapped to return them to the OS, or marked with protection attributes (read, write, execute).

## Virtual Memory vs Physical Memory

When a program requests memory, the OS grants virtual memory—address space the program can reference. Virtual addresses are not physical RAM addresses. They are references into a virtual address space managed by the OS. The mapping from virtual to physical addresses happens through page tables maintained by the CPU's memory management unit (MMU).

Virtual memory enables isolation (each process has its own address space) and overcommitment (the OS can grant more virtual memory than physical RAM exists). Physical pages are allocated lazily: when a program first accesses a virtual page, a page fault occurs, the OS allocates a physical page, and the MMU updates the page table to map the virtual address to the physical address.

## Resident Set Size (RSS)

RSS is the amount of physical memory a process currently occupies. When a virtual page is faulted in, it becomes resident—it occupies a physical page in RAM. RSS measures how many pages are resident at any given moment.

RSS is distinct from virtual memory size (which includes unmapped pages and pages the OS has swapped out) and distinct from the working set (the set of pages the program actively uses). A program might have gigabytes of virtual memory but only megabytes of RSS. RSS is what matters for system performance—it determines memory pressure, whether the system swaps, and whether the system runs out of physical memory.

## Memory Allocator

A memory allocator bridges the gap between what the OS provides (pages) and what programs need (arbitrary-sized objects). The OS gives memory in 4KB chunks. A web server needs 80 bytes for a session, 256 bytes for a request, 512 bytes for a response. The allocator subdivides pages into smaller allocations.

The canonical allocator interface is `malloc(size)` and `free(ptr)`. The allocator's job is to maintain a pool of pages, satisfy allocation requests by finding or creating space, and reclaim space when objects are freed. The allocator must answer: where should this allocation go? When can freed memory be reused? How can pages be returned to the OS when they are no longer needed?

## Spatial Fragmentation

Spatial fragmentation occurs when free memory exists but is scattered into unusable fragments. Consider a 4KB page. A program allocates 100 bytes, then 200 bytes, then 50 bytes. They are placed end-to-end. The program frees the 200-byte allocation in the middle. Now there is a hole: 100 bytes used, 200 bytes free, 50 bytes used. If the next allocation is 300 bytes, it will not fit in the 200-byte hole, even though 200 bytes are free.

Over time, as allocations and frees interleave, memory resembles Swiss cheese: plenty of free space in aggregate, but scattered into holes too small to satisfy requests. Allocation cost rises because the allocator must search for suitable holes. Cache locality degrades because objects are scattered. Metadata grows because the allocator must track many small fragments.

Traditional allocators respond with free lists (tracking available holes by size), tree structures (efficiently finding best-fit holes), and splitting (dividing large holes to satisfy small requests). This works for general-purpose workloads, but the complexity accumulates, and search times grow.

## Temporal Fragmentation

Temporal fragmentation is subtler and more damaging than spatial fragmentation. It occurs when objects with vastly different lifetimes share the same page.

Consider a page containing four objects: A (a session object living for hours), B (a request object living milliseconds), C (a cache entry living minutes), D (a logging buffer freed immediately). When B and D are freed, the page is not empty—A and C are still alive. The OS cannot reclaim the page because it contains live data. The page remains resident, even though half of it is unused.

Over time, as allocations and frees continue, pages accumulate mixtures of live and dead objects. RSS grows even though the working set remains constant. Pages cannot be returned to the OS because they are pinned by even a single long-lived object. This is temporal fragmentation: memory that is allocated but not fully utilized, scattered across pages that cannot be reclaimed.

Traditional allocators respond with compaction: moving live objects to consolidate them into fewer pages so old pages can be released. But compaction is expensive (requires copying), unpredictable (causes latency spikes), and incompatible with systems expecting stable pointers (pointers become invalid after compaction).

## Fragmentation as Entropy

In thermodynamics, entropy measures disorder. A low-entropy system is organized—energy is concentrated and useful. A high-entropy system is disordered—energy is dispersed and unusable. Without external work, entropy increases (second law of thermodynamics).

Memory fragmentation follows the same pattern. At startup, memory is organized: pages are either empty or full. As the program runs, allocations and frees interleave. Pages accumulate mixtures of live and dead objects. The system becomes disordered. Memory is dispersed across many pages, much of it unreclaimable. Fragmentation is entropy.

Traditional allocators treat fragmentation as an engineering problem—better data structures or heuristics can reduce it. This is true to a point. But if allocations and frees are uncorrelated with object lifetimes, fragmentation is inevitable. The allocator can delay it but cannot prevent it without reorganizing memory (compaction), which is itself expensive.

The insight is that the allocator should not fight entropy by constantly reordering memory. It should manage entropy by ensuring objects expire in an organized way—grouping objects with similar lifetimes so that when their lifetimes end, entire pages become empty and can be reclaimed as a unit.

## Lifetime Affinity

Lifetime affinity is the principle that objects with similar lifetimes should be placed in the same page. If short-lived objects are grouped together, they die together, and their page can be reclaimed. If long-lived objects are grouped together, their page remains full and useful. The page does not become pinned by a mixture of live and dead objects.

The problem is that the allocator does not know object lifetimes in advance. A session object and a request object have the same type signature—they are both allocations of N bytes. The allocator cannot predict that one will live hours and the other milliseconds.

The solution is allocation-order affinity: group allocations that occur close together in time. Programs exhibit temporal locality—allocations that occur around the same time are often causally related and thus have correlated lifetimes. A web server handling a request allocates a request object, a response object, a buffer, and a session token. These are allocated in quick succession. They are causally related—created for the same request. When the request completes, they are all freed. If they are in the same page, that page becomes empty.

Allocation-order affinity does not require the allocator to predict lifetimes. It requires the allocator to allocate sequentially within pages, so objects allocated in the same epoch end up in the same page. Lifetime correlation emerges naturally from allocation patterns, not from explicit hints.

## Slab

A slab is a fixed-size subdivision of a page. Instead of treating a page as a pool of arbitrary-sized allocations, a slab divides a page into uniform slots of a fixed size. A 4KB page with 64-byte slots yields 63 slots (after accounting for metadata). A page with 128-byte slots yields 31 slots.

A slab allocator maintains separate slabs for each size class. To allocate a 100-byte object, the allocator rounds up to the next size class (e.g., 128 bytes) and allocates a slot from a 128-byte slab. Allocation is finding an available slot (no search required—a bitmap tracks slot state). Free marks the slot as available.

Slabs solve two problems. First, they eliminate spatial fragmentation. There are no holes to search—slots are uniform and pre-allocated. Allocation is O(1) (find first free bit in bitmap). Second, they naturally group allocations by time. A slab is filled sequentially. Objects allocated around the same time end up in the same slab. If those objects have correlated lifetimes (allocation-order affinity), they die together, and the slab becomes empty as a unit.

## Size Class

A size class is a fixed allocation size supported by the slab allocator. A program requests 100 bytes. The allocator rounds up to the next size class, e.g., 128 bytes. The allocator maintains separate slabs for each size class: slabs for 64-byte objects, slabs for 128-byte objects, slabs for 256-byte objects.

The choice of size classes is a trade-off. If classes are too coarse (e.g., 64, 256, 1024), internal fragmentation is high—a 65-byte request wastes 191 bytes in a 256-byte slot (74% waste). If classes are too fine (e.g., 64, 65, 66, ...), the allocator requires many slab pools, increasing metadata and reducing reuse.

temporal-slab uses 8 size classes: 64, 96, 128, 192, 256, 384, 512, 768 bytes. This progression is approximately exponential (ratio ~1.5x) but adjusted to align with common object sizes. This achieves 88.9% average efficiency across realistic workloads (11.1% internal fragmentation).

## Internal Fragmentation

Internal fragmentation is wasted space within an allocation. A program requests 72 bytes. The allocator rounds to 96 bytes (smallest size class that fits). 24 bytes are wasted (33% overhead). This is internal fragmentation—space allocated but not used.

Internal fragmentation is the cost of using fixed size classes. The benefit is eliminating spatial fragmentation (no holes, no search). The trade-off is deliberate: waste a predictable, bounded amount of space per allocation in exchange for O(1) allocation, elimination of search overhead, and guaranteed lifetime grouping.

The key is choosing size classes to minimize waste. If 80% of allocations are between 65-96 bytes, a 96-byte size class reduces waste significantly compared to a 128-byte class.

## Slab Lifecycle

A slab moves through three states: partial, full, empty.

**Partial:** The slab has at least one free slot and at least one allocated slot. It is available for new allocations. The allocator maintains a partial list—a linked list of all partial slabs for a size class.

**Full:** Every slot in the slab is allocated. The slab is moved from the partial list to the full list. It is no longer considered for allocation. Full slabs remain mapped but are set aside.

**Empty:** All slots are free. The slab has no live objects. At this point, the allocator can destroy the slab (unmap the page, return it to the OS) or cache the slab for reuse (keep it mapped, push it to a cache for fast reallocation).

Transitions:
- New slab → Partial (first allocation)
- Partial → Full (last free slot allocated)
- Full → Partial (first slot freed)
- Partial → Empty (last allocated slot freed)

The lifecycle ensures that only completely empty slabs are candidates for recycling or destruction. Partially empty slabs remain on the partial list, available for reuse.

## Lock-Free Allocation

In multi-threaded programs, multiple threads may allocate concurrently. The naive approach is to protect the allocator with a mutex. Before allocating, acquire the lock. This is correct but slow—lock contention becomes a bottleneck.

The solution is to make the fast path lock-free. The fast path is the common case: allocating from a slab that already has free slots. The slow path is the rare case: allocating a new slab when the current one is full.

Lock-free allocation uses atomic operations—CPU instructions that modify memory without locks. The key primitive is compare-and-swap (CAS): atomically check if a memory location equals an expected value, and if so, replace it with a new value. If another thread modified it, CAS fails. The thread retries.

The allocator maintains a `current_partial` pointer (the active slab for fast-path allocations). Allocation:
1. Load `current_partial` atomically.
2. Find a free bit in the slab's bitmap and attempt to set it with CAS.
3. If CAS succeeds, return the slot pointer.
4. If CAS fails (another thread took the slot), retry.
5. If the slab is full (no free bits), fall back to the slow path (acquire mutex, select a new slab).

This achieves sub-100ns allocation latency in the common case with no lock contention.

## Bounded RSS Through Conservative Recycling

RSS grows unboundedly if slabs accumulate indefinitely. The allocator must recycle empty slabs—reuse them for new allocations or destroy them to return memory to the OS.

The naive approach is to recycle any empty slab immediately. The problem is a race condition:

1. Thread A loads `current_partial`, obtaining a pointer to slab S.
2. Thread B frees the last object in slab S, making it empty.
3. The allocator recycles S, pushing it to the cache or destroying it.
4. Thread C pops S from the cache and fills it with new objects.
5. Thread A attempts to allocate from S, unaware it was recycled.

Thread A holds a stale pointer to a slab that has been repurposed. This is a use-after-free race—catastrophic.

The solution is conservative recycling: only recycle slabs that are not published to `current_partial`. Slabs on the partial list may be held by threads in the lock-free fast path. Such slabs are never recycled, even if they become empty. They remain on the partial list indefinitely.

Only slabs on the full list (slabs never published to `current_partial`) are eligible for recycling. Since no thread holds pointers to full slabs, recycling them is safe.

This sacrifices some memory efficiency—a 90% empty slab on the partial list will not be recycled. But it guarantees correctness without complex synchronization (no hazard pointers, no reference counting). The trade-off: predictable behavior over aggressive reclamation.

## Slab Cache

When a slab becomes empty, it is not immediately destroyed. Instead, it is pushed to a per-size-class cache. The cache has fixed capacity (e.g., 32 slabs). When a new slab is needed, the allocator checks the cache first. If non-empty, pop a slab from the cache (fast, no syscall). If empty, allocate a new slab via mmap (slow, syscall).

The cache absorbs transient fluctuations in allocation rate. A workload that allocates 1 million objects, frees them, allocates 1 million more can reuse slabs from the cache without mmap/munmap churn.

If the cache is full and another slab becomes empty, it is pushed to an overflow list. The overflow list has unbounded capacity. Slabs on the overflow list remain mapped—they are not unmapped (no munmap).

## Refusal to Unmap

temporal-slab never unmaps slabs during runtime. When a slab becomes empty, it is pushed to the cache or overflow list but remains mapped. Unmapping only occurs during allocator destruction.

This design choice has two benefits:

1. **Safe handle validation.** temporal-slab uses opaque handles encoding a slab pointer, slot index, and size class. When freeing a handle, the allocator checks the slab's magic number and slot state. If the handle is invalid (double-free, wrong allocator, corrupted), the check fails and returns an error. But this only works if the slab remains mapped. If unmapped, dereferencing the handle triggers a segmentation fault.

2. **Bounded RSS.** RSS is bounded by the high-water mark of allocations. If a workload briefly allocates 10GB then stabilizes at 1GB, RSS is 10GB. This is deliberate: RSS reflects the maximum working set observed, not the current working set. Unmapping slabs would reduce RSS but at the cost of mmap/munmap churn and the inability to validate handles.

## O(1) Deterministic Class Selection

To allocate an object, the allocator must map the requested size to a size class. The naive approach is a linear scan: iterate over size classes and return the first that fits. For 8 size classes, this is up to 8 comparisons—8 conditional branches.

Branch mispredictions are expensive (10-20 cycles on modern CPUs). If the requested size varies unpredictably, every allocation incurs 8 mispredicted branches (~100 cycles, ~30ns). This is not large in absolute terms, but it is unpredictable—a source of jitter.

The solution is a precomputed lookup table. At initialization, build a 768-entry array mapping each possible size (1-768 bytes) to the corresponding size class index. Allocation performs a single array lookup:

```c
uint8_t class_idx = k_class_lookup[size];
```

This is O(1) with zero branches. The cost is constant regardless of size or workload pattern. The lookup table occupies 768 bytes (fits in L1 cache). The benefit is deterministic latency—no jitter from class selection.

## Handle-Based API vs Malloc-Style API

temporal-slab provides two APIs:

**Handle-based:** `alloc_obj(alloc, size, &handle)` returns a pointer and an opaque handle. `free_obj(alloc, handle)` frees by handle. The handle encodes the slab pointer, slot index, and size class. No per-allocation metadata is stored (zero overhead). The application must track handles alongside pointers.

**Malloc-style:** `slab_malloc(alloc, size)` returns a pointer. The handle is stored in an 8-byte header before the pointer. `slab_free(alloc, ptr)` reads the header, extracts the handle, and calls `free_obj` internally. This trades 8 bytes overhead per allocation for API compatibility.

The handle-based API is suitable for performance-critical code where zero overhead is required and explicit handle management is acceptable. The malloc-style API is suitable for drop-in replacement where 8 bytes overhead is tolerable.

## What You Should Understand Now

At this point, you understand the foundational concepts:

Memory allocation bridges the gap between OS-provided pages and application-needed objects. Allocators face two fragmentation problems: spatial (free space scattered into unusable holes) and temporal (pages pinned by mixtures of live and dead objects). Temporal fragmentation is entropy—inevitable without organizing allocations by lifetime.

Slab allocation solves this by dividing pages into fixed-size slots (size classes) and filling slabs sequentially. Objects allocated around the same time end up in the same slab. If they have correlated lifetimes (allocation-order affinity), they die together, and the slab can be recycled as a unit. This manages entropy by grouping objects to expire in an organized way.

temporal-slab extends this with lock-free fast-path allocation (sub-100ns latency), conservative recycling (only full slabs are recycled, eliminating use-after-free races), refusal to unmap (enabling safe handle validation and bounded RSS), and O(1) class selection (eliminating jitter).

The allocator does not predict lifetimes or require application hints. Lifetime alignment emerges naturally from allocation patterns. This is substrate, not policy—a foundation for higher-level systems to build on.

The implementation details—how bitmaps encode slot state, how atomic CAS loops allocate slots, how list membership is tracked—follow from these foundational concepts. Those details are in the source code. This document provides the foundation to understand why they exist.
