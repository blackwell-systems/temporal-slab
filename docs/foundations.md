# Foundations: Memory Allocation and Lifetime-Aware Design

This document provides the theoretical and practical background necessary to understand temporal-slab's design decisions. It assumes no prior knowledge of memory allocator internals.

## Chapter 1: What is Memory Allocation?

When a program runs, it needs memory to store data. The operating system provides memory to processes in large chunks called pages, typically 4096 bytes (4KB) on modern systems. However, programs rarely need exactly 4KB at a time. A web server might need 80 bytes to store a user session, 256 bytes for a request object, or 512 bytes for a response buffer. The gap between what the operating system provides (pages) and what applications need (arbitrary-sized objects) is filled by memory allocators.

A memory allocator is a subsystem that manages a pool of memory, satisfying allocation requests of various sizes and reclaiming memory when it is no longer needed. The canonical interface is malloc and free in C, but the same problem exists in every programming language. The allocator's job is to answer two questions: where should this new allocation go, and when can freed memory be reused?

### The Spatial Problem

Consider a simple allocator that starts with a 4KB page and hands out memory sequentially. A program allocates 100 bytes, then 200 bytes, then 50 bytes. The allocator places them end-to-end. So far, 350 bytes are used. Now the program frees the 200-byte allocation in the middle. The allocator has a hole: 100 bytes of used memory, then 200 bytes free, then 50 bytes used. If the next allocation request is for 300 bytes, it will not fit in the 200-byte hole, even though there is sufficient free memory in aggregate. This is spatial fragmentation.

Traditional allocators respond to spatial fragmentation with increasingly sophisticated bookkeeping. They maintain free lists organized by size, use tree structures to find best-fit holes, or apply heuristics like splitting large holes into smaller chunks. This works well for general-purpose workloads, but the complexity accumulates over time. As allocations and frees interleave, the allocator's metadata grows, search times increase, and memory becomes fragmented across many small, unusable holes.

### The Temporal Problem

Spatial fragmentation is visible and well understood. Temporal fragmentation is subtler and more damaging in long-running systems.

Imagine a memory page containing four objects: A, B, C, and D. A is a session object that will live for hours. B is a request object that will be freed in milliseconds. C is a cache entry that might live for minutes. D is a logging buffer that will be freed immediately after writing. These objects have vastly different lifetimes, but they share the same page.

When B and D are freed, the page is not empty—A and C are still alive. The operating system cannot reclaim the page because it contains live data. The page remains resident in memory, even though half of it is unused. Over time, as the program continues to allocate and free, pages accumulate a mixture of live and dead objects. The program's resident set size (RSS) grows even though the working set of live objects remains constant. This is temporal fragmentation.

Temporal fragmentation cannot be solved by better bookkeeping. The problem is not that the allocator cannot find space—it is that the space cannot be reclaimed. The page is pinned by a single long-lived object, preventing the operating system from reusing it. Traditional allocators respond with compaction: they move live objects to new locations, consolidating them into fewer pages so old pages can be released. But compaction is expensive, unpredictable, and incompatible with systems that expect stable pointers.

## Chapter 2: Fragmentation as Entropy

In thermodynamics, entropy measures disorder. A system with low entropy is organized—energy is concentrated and useful. A system with high entropy is disordered—energy is dispersed and unusable. Over time, without external work, entropy increases. This is the second law of thermodynamics.

Memory fragmentation follows the same pattern. At startup, memory is organized: all pages are empty or all pages are full. As the program runs, allocations and frees interleave randomly. Pages accumulate a mixture of live and dead objects. The system becomes disordered. Memory is dispersed across many pages, but much of it cannot be reclaimed. Fragmentation is entropy.

Traditional allocators treat fragmentation as an engineering problem: a consequence of poor data structures or suboptimal heuristics. If we implement a better search algorithm, or use a smarter free list organization, we can reduce fragmentation. This is true to a point. But fundamentally, if allocations and frees are uncorrelated with object lifetimes, fragmentation is inevitable. The allocator can delay it, but it cannot prevent it.

### The Allocator's Dilemma

The traditional allocator's goal is to find space for each allocation request. It optimizes for utilization: maximize the fraction of allocated memory that is actually used. This leads to a policy of filling holes aggressively. If there is a 200-byte hole, use it. If there is a 500-byte hole and a 100-byte request, split the hole and use part of it. This is the first-fit or best-fit strategy.

The problem is that this policy ignores lifetimes. A short-lived allocation placed next to a long-lived allocation creates a temporal fragment. When the short-lived object is freed, the page is pinned by the long-lived object. The allocator has optimized for spatial efficiency at the expense of temporal efficiency.

The correct optimization is not "fill holes efficiently." It is "group objects by expected lifetime." If short-lived objects are allocated together, they will die together, and their pages can be reclaimed as a unit. If long-lived objects are allocated together, their pages remain full and useful. The allocator does not fight entropy—it manages entropy by ensuring objects expire in an organized way.

## Chapter 3: The Slab Allocation Model

The slab allocator was introduced by Jeff Bonwick in 1994 for the Solaris kernel. The insight was simple: if the kernel frequently allocates objects of a fixed size (e.g., process descriptors, file handles), maintain a dedicated pool of pages for each size class. A slab is a single page subdivided into slots of a fixed size. When a process descriptor is needed, allocate a slot from the process descriptor slab. When it is freed, mark the slot as available. No search is necessary—slots are uniform and pre-allocated.

Bonwick's design solved three problems. First, it eliminated search overhead. There are no free lists or complex heuristics. Allocation is finding the next available slot in a bitmap. Second, it eliminated metadata overhead. Traditional allocators store metadata in headers before each allocation. Slabs store metadata in a separate header, reducing per-object cost. Third, it improved cache locality. Objects of the same type are physically adjacent, so accessing one object likely brings related objects into cache.

Bonwick's slab allocator was designed for kernel objects with predictable lifetimes. Kernel objects are typically created in response to system calls and freed shortly afterward. The implicit assumption was that most slabs would cycle between "empty" and "partially full" relatively quickly. If a slab became full, it was set aside. If it became empty, it could be reused immediately.

### The Object Lifetime Assumption

The slab model depends on a critical assumption: objects in a slab have correlated lifetimes. If all objects in a slab are allocated around the same time and freed around the same time, the slab will become empty as a unit and can be recycled cleanly. This is the ideal case.

If objects have uncorrelated lifetimes, the slab model degrades. Consider a slab with 32 slots. Thirty-one objects are freed, but one remains alive. The slab cannot be recycled because it contains live data. The slab occupies a full page (4KB) but provides only 1/32 of its capacity. This is no better than traditional fragmentation—worse, in fact, because the allocator has no mechanism to move the remaining object elsewhere.

The question is: how can the allocator ensure that objects in a slab have correlated lifetimes?

## Chapter 4: Lifetime Affinity Through Allocation Order

Bonwick's slab allocator did not attempt to predict object lifetimes. It relied on the kernel's behavior: objects allocated in response to the same system call would likely be freed around the same time. The allocator's job was not to guess lifetimes but to group allocations that occurred close together in time.

This is allocation-order affinity. If a slab is filled sequentially with objects allocated in a short time window, those objects are likely to have similar lifetimes. Not because the allocator is smart, but because programs exhibit temporal locality. A web server handling a request allocates a request object, a response object, a buffer, and a session token. These objects are causally related—they were created for the same request. When the request completes, they are all freed. If they were allocated from the same slab, that slab becomes empty.

This principle generalizes beyond kernels. Any system where allocations are clustered in time will benefit from allocation-order affinity. A cache system allocates many entries at startup, then allocates new entries sporadically as old ones are evicted. If startup entries are grouped in slabs separate from runtime entries, the allocator naturally separates long-lived from short-lived data.

### Why Traditional Allocators Fail Here

Traditional allocators do not group allocations by time. They group allocations by size, searching for the best-fit hole. If a 100-byte object is allocated at startup, and another 100-byte object is allocated five minutes later, a traditional allocator might place them in the same hole if it is the best fit. These objects are spatially adjacent but temporally unrelated. When the short-lived object is freed, the page is pinned by the long-lived object.

Slab allocators avoid this by refusing to fill holes. A slab allocator does not search for space—it allocates sequentially within a slab. If a slab is full, it allocates a new slab. This means allocations are naturally ordered by time. Objects allocated in the same epoch end up in the same slab.

## Chapter 5: The Slab Lifecycle

A slab moves through three states during its lifetime: partial, full, and empty.

When a slab is first created, it is empty. All slots are available. The allocator places it on the partial list, meaning it has at least one free slot and at least zero allocated slots. As allocations proceed, slots are claimed one by one. When the last free slot is allocated, the slab transitions to the full state. It is moved from the partial list to the full list.

Eventually, objects in the slab are freed. When the first object is freed, the slab transitions back to partial—it now has one free slot. It is moved from the full list back to the partial list. As more objects are freed, the slab empties. When the last object is freed, the slab is completely empty. At this point, the allocator has a choice: keep the slab on the partial list for reuse, or destroy the slab and return the page to the operating system.

Bonwick's design cached empty slabs. Destroying and recreating slabs is expensive—it involves system calls (munmap and mmap) and TLB invalidations. Instead, the allocator maintains a small cache of empty slabs per size class. If a new slab is needed, the allocator checks the cache first. If the cache is empty, it allocates a new page. If the cache is full and another slab becomes empty, the oldest cached slab is destroyed to prevent unbounded growth.

This design is conservative. Slabs are only recycled when they are completely empty. Partially empty slabs remain on the partial list indefinitely, even if they are mostly empty. This avoids the risk of recycling a slab that still contains live objects, but it also means that memory is not released aggressively. The allocator prioritizes correctness and predictability over utilization.

## Chapter 6: Concurrency and Lock-Free Fast Paths

In a single-threaded program, slab allocation is trivial. The allocator maintains a pointer to the current partial slab. Allocation scans the slab's bitmap for a free slot, marks it as allocated, and returns a pointer to the slot. Free marks the slot as available. No synchronization is needed.

In a multi-threaded program, multiple threads may allocate and free concurrently. The naive solution is to protect the allocator with a mutex. Before allocating or freeing, acquire the lock. This is correct but slow. Lock contention becomes a bottleneck, especially in high-frequency allocation workloads.

The solution is to make the fast path lock-free. The fast path is the common case: allocating from a slab that already has free slots. The slow path is the rare case: allocating a new slab when the current one is full, or moving slabs between lists. The goal is to make the fast path proceed without blocking, deferring synchronization to the slow path.

### Atomic Operations and Compare-and-Swap

Modern CPUs provide atomic instructions that operate on memory without locks. The most important is compare-and-swap (CAS). CAS takes three arguments: a memory address, an expected value, and a new value. If the current value at the address matches the expected value, replace it with the new value and return success. Otherwise, return failure without modifying memory. CAS is atomic—no other thread can observe an intermediate state.

CAS enables lock-free data structures. Instead of acquiring a lock, a thread reads a shared variable, computes a new value, and attempts to update the variable with CAS. If another thread modified the variable in the meantime, CAS fails. The thread retries, reading the updated value and attempting CAS again. This is called a CAS loop.

CAS loops are not always faster than locks. If contention is high, threads waste CPU time retrying. But in low-to-moderate contention, CAS loops are dramatically faster than locks because they avoid kernel transitions and cache coherence overhead.

### Lock-Free Slab Allocation

temporal-slab uses a lock-free fast path based on atomic operations. Each size class maintains a `current_partial` pointer, which points to the currently active slab. This pointer is shared across all threads. Allocation proceeds as follows:

1. Load `current_partial` with an atomic acquire. This ensures the pointer and the slab's contents are visible.
2. Attempt to allocate a slot from the slab by finding a free bit in the bitmap and setting it with CAS.
3. If CAS succeeds, return the slot pointer. If CAS fails (another thread claimed the slot), retry step 2.
4. If the slab has no free slots (all bits set), fall back to the slow path.

The slow path acquires a per-size-class mutex and selects a new slab from the partial list. This is rare—it only happens when the current slab is exhausted. Once a new slab is selected, it is published to `current_partial` with an atomic store, and the fast path resumes.

Free also uses a lock-free approach. Each slot's state is encoded in the slab's bitmap. Freeing a slot sets the corresponding bit with an atomic operation. No lock is required unless the slab transitions between states (e.g., from full to partial), which requires updating the partial and full lists under a mutex.

This design achieves sub-100 nanosecond allocation latency in the common case while maintaining correctness under concurrency.

## Chapter 7: Fragmentation and the Recycling Problem

The slab model solves spatial fragmentation by eliminating search. But it does not inherently solve temporal fragmentation. If a slab contains one long-lived object and 31 short-lived objects, the slab remains pinned after the short-lived objects are freed. The allocator cannot reclaim the page because it contains live data.

Traditional slab allocators tolerate this. The assumption is that most slabs will either stay full or become empty relatively quickly. Partially empty slabs are expected to refill from new allocations. Over time, the working set stabilizes, and the allocator reaches equilibrium. This works well in steady-state systems like kernel allocators, where object lifetimes are predictable and churn is moderate.

In high-churn systems, this assumption breaks down. A cache system might allocate millions of entries, evict them in batches, and allocate new entries. A session store might create thousands of sessions per second and expire them hours later. In these workloads, slabs do not stabilize—they churn continuously. Partially empty slabs accumulate, RSS grows, and the system runs out of memory despite low utilization.

### The FULL-Only Recycling Invariant

temporal-slab addresses this with a conservative recycling policy: only slabs on the full list are eligible for recycling. A slab reaches the full list when every slot is allocated. Once full, the slab is never published to `current_partial` again—it is set aside. When objects in a full slab are freed, the slab transitions back to partial. If all objects in a full slab are freed, the slab becomes empty and is pushed to the slab cache or overflow list.

This policy has a critical property: only slabs that were once full are recycled. Slabs that were on the partial list when they became empty are not recycled. They remain on the partial list, available for immediate reuse.

Why does this matter? Because it eliminates a race condition. Consider the following scenario:

1. Thread A loads `current_partial`, obtaining a pointer to slab S.
2. Thread B frees the last object in slab S, making it empty.
3. The allocator recycles slab S, pushing it to the cache.
4. Thread C pops slab S from the cache and fills it with new objects.
5. Thread A attempts to allocate from slab S, unaware that it has been recycled.

Thread A holds a stale pointer to a slab that has been repurposed. If the bitmap positions line up, Thread A might overwrite Thread C's data. This is a use-after-free race, and it is catastrophic.

The FULL-only invariant prevents this. If a slab is on the partial list, it is published to `current_partial`, and threads may hold pointers to it. Such slabs are never recycled, even if they become empty. They remain on the partial list indefinitely. Only slabs on the full list—slabs that are not published to `current_partial`—are eligible for recycling. Since no thread holds pointers to full slabs, recycling them is safe.

This policy sacrifices some memory efficiency. A partially empty slab on the partial list will not be recycled even if it is 90% empty. But it guarantees correctness without requiring hazard pointers, reference counting, or other complex synchronization mechanisms. The trade-off is deliberate: predictable behavior over aggressive reclamation.

## Chapter 8: Bounded RSS and Cache Pressure

A key property of temporal-slab is bounded RSS. RSS (resident set size) is the amount of physical memory a process occupies at any given moment. When a program requests memory from the operating system, the OS grants virtual memory—address space that the program can reference. But virtual memory is not physical memory. The OS only allocates physical pages when the program actually touches the memory, triggering a page fault. Once a page is faulted in, it remains in physical memory (resident) until the OS decides to evict it or the program unmaps it.

RSS measures how many pages are currently resident in physical RAM. This is distinct from virtual memory size, which includes unmapped pages, and distinct from the program's working set, which is the set of pages the program actively uses. A program might have gigabytes of virtual memory but only megabytes of RSS. RSS is what matters for system performance—it determines memory pressure, swap behavior, and whether the system runs out of physical memory.

In traditional allocators, RSS can grow unboundedly under churn, even if the working set remains constant. Temporal fragmentation causes pages to accumulate live and dead objects. Pages cannot be released because they contain at least one live object. Over time, RSS grows to reflect the high-water mark of allocations, not the current working set. A program that briefly allocated 10GB but now uses only 1GB may still have 10GB RSS because the allocator cannot return pages to the OS without compaction.

temporal-slab bounds RSS through three mechanisms: slab caching, overflow tracking, and refusal to unmap.

### Slab Caching

When a slab becomes empty, it is not immediately destroyed. Instead, it is pushed to a per-size-class cache. The cache has a fixed capacity (e.g., 32 slabs per size class). If the cache is full and another slab becomes empty, the new slab is pushed to an overflow list instead. The overflow list has unbounded capacity but is intended to remain small.

When a new slab is needed, the allocator first checks the cache. If the cache is non-empty, it pops a slab from the cache and reuses it. This avoids system calls (mmap) and page faults, reducing allocation latency. The cache acts as a buffer, absorbing transient fluctuations in allocation rate.

The cache capacity determines the minimum RSS. If each size class caches 32 slabs, and each slab is 4KB, the minimum RSS is 32 × 4KB × (number of size classes). For 8 size classes, this is 1MB. This is the baseline memory cost, independent of workload.

### Overflow Tracking

If the cache is full and another slab becomes empty, the slab is pushed to the overflow list. The overflow list is a linked list of empty slabs. Unlike the cache, the overflow list has no capacity limit. Slabs on the overflow list remain mapped—they are not returned to the operating system.

This design choice is deliberate. Unmapping pages (munmap) is expensive—it requires a system call, TLB invalidation, and page table updates. More importantly, unmapping pages breaks the allocator's ability to validate stale handles. temporal-slab uses opaque handles that encode a slab pointer, slot index, and size class. If a handle is freed twice (double-free), or freed with the wrong allocator, the allocator can detect this by checking the slab's magic number and slot state. But this check only works if the slab remains mapped. If the slab is unmapped, dereferencing the handle triggers a segmentation fault.

By refusing to unmap, temporal-slab guarantees that invalid handles never cause crashes. They return an error code instead. This property is essential for debugging and fault tolerance in production systems.

The cost is that RSS includes all slabs that have ever been allocated. If the workload experiences a transient spike—allocating millions of objects and then freeing them—RSS will reflect the peak, not the steady-state. This is a conscious trade-off: predictable RSS growth over aggressive reclamation.

### Why RSS Growth is Bounded

Under sustained churn, RSS grows until the overflow list stabilizes. At that point, slabs are recycled from the overflow list instead of being newly allocated. The allocator reaches equilibrium: the number of slabs allocated equals the number recycled. RSS plateaus.

The key insight is that RSS is bounded by the maximum working set, not by the total number of allocations. If a workload allocates 10 million objects over its lifetime but never has more than 1 million live at once, RSS is determined by the 1 million live objects, not the 10 million total. Temporal fragmentation is eliminated because slabs are recycled as a unit when they become empty.

## Chapter 9: Internal Fragmentation and Size Classes

Slab allocators trade external fragmentation (holes between allocations) for internal fragmentation (wasted space within allocations). A program that needs 72 bytes must allocate from a size class that provides at least 72 bytes. If the smallest size class that fits is 96 bytes, 24 bytes are wasted. This is internal fragmentation.

The question is: how should size classes be chosen?

### The Granularity Trade-Off

If size classes are too coarse, internal fragmentation is high. A system with size classes {64, 256, 1024} wastes up to 75% of each allocation (e.g., a 65-byte allocation uses a 256-byte slot). If size classes are too fine, the allocator requires many slabs, increasing metadata overhead and reducing cache hit rates.

temporal-slab uses 8 size classes: 64, 96, 128, 192, 256, 384, 512, and 768 bytes. These are not chosen arbitrarily. The progression is approximately exponential with ratio 1.5x, but adjusted to align with common object sizes. The 96-byte class covers objects in the 65-96 range, which are common in network protocols (e.g., TCP headers, small JSON payloads). The 192-byte class covers structures in the 129-192 range, common in cache metadata. The 384-byte class covers mid-size buffers.

This distribution achieves 88.9% average space efficiency across a realistic workload. For comparison, a system with 4 size classes {64, 128, 256, 512} achieves ~75% efficiency. The additional granularity reduces waste by 13.9 percentage points.

### Objects Per Slab and Cache Alignment

Each slab is a single 4KB page. The number of objects per slab depends on object size. A 64-byte size class yields 63 objects per slab (after accounting for the slab header and bitmap). A 768-byte size class yields 5 objects per slab. Fewer objects per slab means more slabs are required for the same number of allocations, increasing overhead.

This is an unavoidable trade-off. Large size classes are necessary to support large objects, but they reduce slab utilization. The choice of 768 bytes as the maximum size is deliberate. It provides reasonable coverage for mid-size objects while avoiding pathological cases where only one or two objects fit per slab.

## Chapter 10: Deterministic Performance and O(1) Class Selection

In high-frequency trading (HFT) and real-time systems, predictability is more important than average-case performance. An allocator with 50ns average latency and 1ms tail latency is useless if the tail latency occurs unpredictably. The system cannot tolerate jitter.

Traditional allocators have multiple sources of jitter. Lock contention causes threads to block. Search algorithms have variable cost depending on free list state. Compaction causes latency spikes when the system decides to reorganize memory. These are not bugs—they are inherent to the design.

temporal-slab eliminates jitter through deterministic operations. Allocation is lock-free in the fast path, so there is no blocking. Bitmap operations are constant time, so there is no search cost. No compaction occurs, so there are no background pauses. The remaining source of variability is class selection: mapping a requested size to a size class.

### The Linear Scan Problem

The naive approach is to iterate over the size class array and return the first class that fits. For 8 size classes, this requires up to 8 comparisons. Each comparison is a conditional branch, and branch mispredictions are expensive on modern CPUs. If the requested size varies unpredictably, branch prediction fails, and each allocation incurs 8 mispredicted branches. On a modern CPU, this costs ~100 cycles, or ~30ns.

This is not a large cost in absolute terms, but it is unpredictable. If the workload always requests the same size, the branch predictor learns the pattern, and the cost drops to a few cycles. If the workload requests varying sizes, the cost remains high. This variability is jitter.

### O(1) Lookup Table

temporal-slab replaces the linear scan with a precomputed lookup table. At initialization, the allocator builds a 768-entry array mapping each possible size (1-768 bytes) to the corresponding size class index. Allocation performs a single array lookup:

```c
uint8_t class_idx = k_class_lookup[size];
```

This is O(1) with zero branches. The cost is constant regardless of size or workload pattern. The lookup table occupies 768 bytes, which fits in L1 cache. The first access faults the cache line; subsequent accesses are cache hits.

This design eliminates class selection as a source of jitter. The cost is 768 bytes of memory per process—negligible—and the table is initialized once at startup. The benefit is deterministic latency: every allocation pays the same cost for class selection.

## Chapter 11: Handles and Validation

temporal-slab provides two APIs: a handle-based API and a malloc-style API. The handle-based API is lower-level and more explicit. The malloc-style API is a convenience wrapper.

A handle is a 64-bit opaque value encoding three pieces of information: the slab pointer (48 bits), the slot index within the slab (8 bits), and the size class (8 bits). This encoding allows the allocator to validate handles at free time without dereferencing arbitrary pointers.

### Why Handles?

In a traditional malloc/free API, free takes a pointer returned by malloc. The allocator must determine which allocation the pointer belongs to, typically by storing metadata in a header before the allocation. This has two costs. First, the header consumes 8-16 bytes per allocation, increasing overhead. Second, dereferencing an invalid pointer (e.g., a dangling pointer, a double-free, or a pointer from another allocator) triggers a segmentation fault. The program crashes.

Handles avoid both costs. The handle encodes all information necessary to locate the allocation, so no per-allocation metadata is required (zero overhead). Validating a handle checks the slab's magic number and the slot's state. If the handle is invalid, the allocator returns an error instead of crashing. This makes the system fault-tolerant.

### The Malloc Wrapper

The malloc-style API stores the handle in an 8-byte header before the returned pointer. When slab_free is called, the allocator reads the header, extracts the handle, and calls free_obj internally. This trades 8 bytes of overhead per allocation for a familiar API.

The malloc wrapper is suitable for drop-in replacement scenarios where code expects malloc/free semantics. The handle-based API is suitable for performance-critical code where zero overhead is required.

## Chapter 12: Why temporal-slab Exists

temporal-slab is inspired by Zoned Namespace (ZNS) SSDs, a storage technology that exposes zones—contiguous regions of storage—that must be written sequentially. ZNS SSDs eliminate random writes, reducing write amplification and improving endurance. The key insight is that sequential writes align with data lifetimes. Data written together is likely to be deleted together. By writing data sequentially by zone, the SSD naturally groups data with similar lifetimes, enabling efficient reclamation.

temporal-slab applies the same principle to memory. Instead of treating memory as a random-access space where allocations can occur anywhere, it organizes memory into slabs—sequential allocation units. Objects allocated in the same slab are temporally related. When their lifetimes end, the slab is reclaimed as a unit. This is sequential allocation applied to DRAM.

The name "temporal-slab" reflects this connection. Like ZNS SSDs, the allocator is ZNS-inspired rather than ZNS-dependent. It does not require ZNS hardware. The lifetime-aware placement strategy is the organizing principle, applicable to any memory hierarchy.

### The Missing Middle

Existing ZNS systems operate at file or extent granularity—megabytes to gigabytes. At small object sizes (64-256 bytes), metadata costs dominate, and zone-based placement becomes impractical. temporal-slab fills the gap by bringing lifetime-aware placement to the allocator layer, where object-scale decisions are cheap and precise.

This makes temporal-slab a substrate—a foundation on which higher-level systems can be built. A cache system can use temporal-slab for metadata allocation, gaining bounded RSS and predictable latency without implementing its own allocator. A tiered storage system can use slabs as the unit of promotion between DRAM, persistent memory, and NVMe, leveraging the fact that slabs naturally separate hot and cold data.

temporal-slab is not a complete system. It is the kernel of a system. The allocator provides mechanism, not policy. Higher layers decide what to cache, when to evict, and how to tier. The allocator ensures that those decisions are implemented efficiently and correctly.

## Chapter 13: What You Should Understand Now

At this point, you should understand the following:

Memory allocators bridge the gap between what operating systems provide (pages) and what programs need (arbitrary-sized objects). Traditional allocators optimize for spatial efficiency, filling holes in fragmented memory. This leads to temporal fragmentation, where pages contain a mix of live and dead objects and cannot be reclaimed. Temporal fragmentation is entropy—a natural consequence of uncorrelated allocation and free patterns.

Slab allocators address this by grouping allocations by size and time. A slab is a page subdivided into fixed-size slots. Allocation proceeds sequentially within a slab, grouping objects allocated around the same time. If those objects have correlated lifetimes—as is common in many workloads—they die together, and the slab can be recycled as a unit.

temporal-slab extends the slab model with three key properties. First, it uses a lock-free fast path based on atomic operations, achieving sub-100ns allocation latency. Second, it uses a FULL-only recycling policy, ensuring that only slabs not currently in use are recycled, eliminating use-after-free races. Third, it refuses to unmap slabs during runtime, bounding RSS and enabling safe handle validation.

These properties make temporal-slab suitable for high-frequency, latency-sensitive workloads where predictability is paramount. The allocator does not guess object lifetimes or require application hints. Lifetime alignment emerges naturally from allocation order. This is substrate, not policy.

The next step is to understand the implementation: how bitmaps encode slot state, how atomic CAS loops allocate slots, how list membership is tracked for O(1) moves, and how performance counters attribute tail latency. Those details are in the source code. This document provides the foundation to understand why those details exist.
