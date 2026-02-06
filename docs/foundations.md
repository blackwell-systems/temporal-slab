# Foundations: Memory Allocation and Lifetime-Aware Design

This document builds the theoretical foundation for temporal-slab from first principles, defining each concept before using it to explain the next. It assumes no prior knowledge of memory allocator internals.

## Table of Contents

**Operating System Concepts**
- [Page](#page)
- [Virtual Memory vs Physical Memory](#virtual-memory-vs-physical-memory)
- [Overcommitment](#overcommitment)
- [Page Fault](#page-fault)
- [Resident Set Size (RSS)](#resident-set-size-rss)
- [Anonymous Memory vs File-Backed Memory](#anonymous-memory-vs-file-backed-memory)
- [mmap and munmap](#mmap-and-munmap)

**Allocator Fundamentals**
- [Memory Allocator](#memory-allocator)
- [Metadata](#metadata)
- [Free List](#free-list)
- [Churn](#churn)
- [Spatial Fragmentation](#spatial-fragmentation)
- [Temporal Fragmentation](#temporal-fragmentation)
- [Fragmentation as Entropy](#fragmentation-as-entropy)
- [Working Set](#working-set)

**Slab Allocation Model**
- [Slab Allocator](#slab-allocator)
- [Lifetime Affinity](#lifetime-affinity)
- [Slab](#slab)
- [Size Class](#size-class)
- [Internal Fragmentation](#internal-fragmentation)
- [External Fragmentation](#external-fragmentation)
- [Slab Lifecycle](#slab-lifecycle)
- [Bitmap Allocation](#bitmap-allocation)

**Implementation Techniques**
- [Lock-Free Allocation](#lock-free-allocation)
- [Compare-and-Swap (CAS)](#compare-and-swap-cas)
- [Atomic Operations](#atomic-operations)
- [Bounded RSS Through Conservative Recycling](#bounded-rss-through-conservative-recycling)
- [Slab Cache](#slab-cache)
- [Refusal to Unmap](#refusal-to-unmap)
- [O(1) Deterministic Class Selection](#o1-deterministic-class-selection)
- [Branch Prediction and Misprediction](#branch-prediction-and-misprediction)

**API Design**
- [Handle-Based API vs Malloc-Style API](#handle-based-api-vs-malloc-style-api)
- [What You Should Understand Now](#what-you-should-understand-now)

---

## Page

A page is the smallest unit of memory the operating system manages. On most modern systems (x86-64, ARM64), a page is 4096 bytes (4KB). When a program requests memory, the OS allocates entire pages. The program may request 80 bytes, but the OS grants at least one page. Pages are the fundamental currency of memory management—they can be mapped into a process's address space, unmapped to return them to the OS, or marked with protection attributes (read, write, execute).

## Virtual Memory vs Physical Memory

When a program requests memory, the OS grants virtual memory—address space the program can reference. Virtual addresses are not physical RAM addresses. They are references into a virtual address space managed by the OS. The mapping from virtual to physical addresses happens through page tables maintained by the CPU's memory management unit (MMU).

Virtual memory enables isolation (each process has its own address space) and overcommitment (the OS can grant more virtual memory than physical RAM exists). Physical pages are allocated lazily: when a program first accesses a virtual page, a page fault occurs, the OS allocates a physical page, and the MMU updates the page table to map the virtual address to the physical address.

## Overcommitment

Overcommitment is the operating system's ability to grant more virtual memory than physical RAM exists. When a program requests memory via malloc or mmap, the OS allocates virtual address space immediately—entries in the page table marking that region as valid. But no physical RAM is assigned yet. The OS makes a promise: "You can use this memory." The physical backing is provided later, on demand, when the program first touches the memory.

Consider a system with 8GB of physical RAM. Three processes each request 4GB of virtual memory. The OS grants all three requests, allocating 12GB of virtual memory—50% more than physical RAM. This is overcommitment. The system has promised 12GB but only has 8GB to deliver.

How can this work? Most programs allocate more memory than they actually use. A program might request a 1GB buffer but only write to the first 100MB. Or it might fork child processes that inherit the parent's address space but only modify a small portion (copy-on-write optimizes this further). Overcommitment exploits this gap between allocated and used memory.

**Why overcommitment matters for allocators:**

An allocator might request 1GB of virtual address space via mmap to subdivide into small allocations. The OS grants the request immediately—no physical RAM is consumed. Only when the allocator writes to pages (causing page faults) does RSS grow. This allows allocators to maintain large virtual address ranges for predictable address layout without forcing physical memory allocation upfront.

**The failure mode:**

If programs actually use all the virtual memory the OS granted, physical RAM runs out. The OS must either swap pages to disk (slow) or invoke the out-of-memory (OOM) killer to terminate processes (catastrophic). Overcommitment is a bet that the sum of actual usage remains below physical RAM. When the bet fails, the system degrades or crashes.

temporal-slab exploits overcommitment deliberately: it mmaps large regions for address space predictability but only faults in pages as slabs are allocated. RSS grows proportionally to actual slab usage, not to the size of the mmapped region.

## Page Fault

A page fault is a hardware exception that occurs when a program accesses a virtual address that is not currently mapped to physical memory. When the CPU's memory management unit (MMU) translates a virtual address to a physical address, it consults the page table. If the page table entry is marked as "not present," the MMU triggers a page fault exception. The CPU traps into the kernel, and the operating system's page fault handler runs.

There are three types of page faults:

**1. Minor page fault (soft fault):** The virtual page exists in the address space but has no physical page assigned yet. This happens when a program first accesses a newly mmapped region. The OS allocates a physical page, zeros it (for security—prevent reading previous data), updates the page table to map the virtual address to the physical address, and resumes the program. The program retries the memory access and succeeds. Minor page faults are how lazy allocation works—the cost of assigning physical memory is deferred until actual use.

**2. Major page fault (hard fault):** The virtual page exists but the physical page was swapped out to disk. The OS reads the page from the swap file, allocates a physical page, loads the data, updates the page table, and resumes the program. Major faults are expensive—disk I/O takes milliseconds compared to nanoseconds for RAM access. When a system starts thrashing (constantly swapping pages in and out), major faults dominate and performance collapses.

**3. Invalid page fault (segmentation fault):** The virtual address is not valid—the page was never mapped, or it was unmapped. The OS cannot satisfy the access. It sends a SIGSEGV signal to the process, typically terminating it with "Segmentation fault (core dumped)."

**Why page faults matter for allocators:**

When an allocator mmaps a 1GB region, that memory is virtual—accessing it before touching it will not fault. The first write to each 4KB page triggers a minor fault. If the allocator touches 1 million pages at startup, that's 1 million page faults—potentially seconds of latency. Well-designed allocators either tolerate the lazy faulting cost (spreading it across many allocations) or explicitly pre-fault pages if startup latency is critical.

temporal-slab tolerates minor page faults naturally: each slab is faulted in when first used. Since slabs are allocated on-demand over time, page faults are amortized across the workload. Slab creation cost includes ~1 page fault per slab (~1-2 microseconds), which is acceptable in the slow path.

## Resident Set Size (RSS)

RSS is the amount of physical memory a process currently occupies. When a virtual page is faulted in, it becomes resident—it occupies a physical page in RAM. RSS measures how many pages are resident at any given moment.

RSS is distinct from virtual memory size (which includes unmapped pages and pages the OS has swapped out) and distinct from the working set (the set of pages the program actively uses). A program might have gigabytes of virtual memory but only megabytes of RSS. RSS is what matters for system performance—it determines memory pressure, whether the system swaps, and whether the system runs out of physical memory.

**RSS and Allocator Design:**

RSS is the ultimate measure of an allocator's efficiency in long-running systems. Virtual memory is cheap (the OS can grant terabytes via overcommitment), but RSS is constrained by physical RAM. An allocator that lets RSS grow unboundedly will eventually exhaust system memory, trigger swapping (catastrophic performance), or cause the OOM killer to terminate processes.

The critical question for allocators: **Does RSS remain bounded under sustained churn?**

**Traditional allocators (malloc, tcmalloc) allow RSS drift:**

Consider a session store that maintains 50,000 active sessions at steady state, with 10,000 allocations and 10,000 frees per second (high churn). Each session is 200 bytes.

Initial state: 50,000 sessions allocated → RSS = 50,000 × 200 = 10 MB (rounded to pages)

After 1 hour of churn (36 million allocations/frees):
- Live sessions: Still 50,000 (steady state)
- Expected RSS: 10 MB (same as initial)
- Actual RSS with traditional allocator: **12-15 MB (20-50% growth)**

Why does RSS grow despite steady-state object count? **Temporal fragmentation.**

Traditional allocators place allocations wherever free space exists. As sessions with different lifetimes (some live seconds, others hours) interleave on the same pages, those pages become pinned by long-lived objects. When short-lived objects die, their memory cannot be returned to the OS because the page still contains live data. Over time, more pages accumulate with mixed lifetimes. RSS grows even though the working set (number of live objects) remains constant.

**Concrete example of RSS drift:**

```
Page 1 at startup:
[Session A: 2hr lifetime][Session B: 5sec lifetime][Session C: 10sec lifetime]...

After 10 minutes:
[Session A: still alive ][FREED             ][FREED              ]...
                          ↑                   ↑
                    75% of page free, but page remains resident because Session A is alive

After 1 hour:
100 pages like this: mostly free, pinned by 1-2 long-lived objects
RSS = 100 pages = 400 KB wasted (unable to reclaim)
```

Over days or weeks, this compounds. A service that should stabilize at 50 MB RSS slowly drifts to 75 MB, then 100 MB. This is **unbounded RSS growth**—not a leak (objects are freed), but inability to reclaim pages.

**temporal-slab prevents RSS drift through lifetime grouping:**

The same session store using temporal-slab with epoch-based allocation:

Initial state: 50,000 sessions → RSS = 10 MB

After 1 hour of churn:
- Live sessions: 50,000 (steady state)
- RSS: **10 MB (0% growth)**

After 1 week:
- Live sessions: 50,000 (steady state)  
- RSS: **10 MB (0% growth)**

Why does RSS remain bounded? **Temporal affinity.**

Sessions allocated in the same epoch (e.g., all sessions created in a 10-second window) are placed in the same slabs. When those sessions expire, the entire slab becomes empty—no long-lived objects pin it. Empty slabs are recycled (reused for new allocations) or cached. Pages can be reused immediately because they contain no live data.

```
Slab A (Epoch 1, sessions allocated at T=0):
[Session 1: 5sec][Session 2: 5sec][Session 3: 5sec]...

At T=10 seconds:
[FREED          ][FREED          ][FREED          ]...
All sessions dead → Entire slab empty → Slab recycled or cached → RSS stable
```

**The RSS guarantee:**

temporal-slab provides **bounded RSS under sustained churn**: RSS is proportional to the peak live set observed, not to the total number of allocations over time.

Traditional allocators:
```
RSS(t) = baseline + drift(t)
where drift(t) grows linearly with churn duration
```

temporal-slab:
```
RSS(t) = max_live_set + allocator_overhead
where max_live_set is constant at steady state
```

This is the fundamental advantage: temporal-slab's RSS is **determined by what you're using**, not by how long you've been running.

## Anonymous Memory vs File-Backed Memory

Memory mappings come in two forms: anonymous and file-backed. The distinction determines where the physical data lives and how it behaves.

**Anonymous memory** has no backing file on disk. When you allocate memory via malloc or mmap with MAP_ANONYMOUS, the OS creates a mapping that exists only in RAM (and potentially swap space). The contents are purely in-memory—they are not synchronized to any file. When the process exits, anonymous memory disappears. The classic use case is heap allocations: temporary data structures that exist only during program execution.

Anonymous memory starts zeroed. When you mmap an anonymous region, the OS lazily assigns pages filled with zeros (for security—prevent reading previous data). This is cheap because the OS uses copy-on-write zero pages: all virtual pages initially map to a single shared physical page of zeros. Only when you write to a page does the OS allocate a unique physical page.

**File-backed memory** is mapped from a file on disk. When you mmap a file, the OS maps the file's contents into the process's address space. Reading from the mapped region reads from the file (via page cache). Writing to the mapped region (with MAP_SHARED) writes back to the file. File-backed memory is how programs access large files without explicitly reading and writing: the OS handles paging transparently.

**Why this matters for allocators:**

Most allocators use anonymous memory (MAP_ANONYMOUS) for their heap. The memory is transient—it exists only for allocations, not to persist data. Anonymous memory is also what the OS overcommits: it can grant massive anonymous mappings without reserving physical RAM upfront.

temporal-slab uses anonymous memory exclusively. Each slab is a 4KB anonymous page, allocated on-demand. When a slab is no longer needed, it remains resident (temporal-slab refuses to unmap during runtime). This keeps RSS predictable and avoids mmap/munmap churn.

An allocator could theoretically use file-backed memory (mmap a file, subdivide it into slabs) to persist allocations across restarts. But this is rare—most allocators treat memory as ephemeral.

## mmap and munmap

mmap and munmap are the system calls programs use to request and return memory from the operating system.

**mmap (memory map)** asks the OS to map a range of virtual addresses into the process's address space. The signature is:

```c
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
```

Common usage for allocators:

```c
void* ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

This requests one page (4096 bytes) of read-write memory, with no backing file (MAP_ANONYMOUS), at an address chosen by the kernel (NULL). The OS returns a pointer to the start of the mapped region. No physical memory is allocated yet—the pages are mapped but not resident. They become resident when first accessed (minor page faults).

**munmap (memory unmap)** returns a previously mapped region to the OS:

```c
munmap(ptr, 4096);
```

This removes the mapping from the process's address space. If physical pages were resident, they are freed and returned to the system. Accessing the region after munmap causes a segmentation fault.

**Why mmap/munmap matter for allocators:**

mmap is how allocators obtain bulk memory from the OS. Instead of requesting one 4KB page at a time, allocators often mmap large regions (e.g., 64MB) and subdivide them internally. This amortizes the syscall overhead—one mmap can provide thousands of pages.

munmap is how allocators return memory to the OS when it is no longer needed. The naive approach is to munmap pages immediately when they become empty. But munmap is expensive (syscall overhead) and irreversible (accessing unmapped memory crashes). Most allocators cache empty pages to avoid mmap/munmap churn.

temporal-slab uses mmap to obtain 4KB slabs one at a time (or in small batches). It refuses to munmap during runtime—slabs remain mapped even when empty. This trades RSS for safety (unmapped memory causes segfaults, breaking handle validation) and performance (no munmap churn).

## Memory Allocator

A memory allocator bridges the gap between what the OS provides (pages) and what programs need (arbitrary-sized objects). The OS gives memory in 4KB chunks. A web server needs 80 bytes for a session, 256 bytes for a request, 512 bytes for a response. The allocator subdivides pages into smaller allocations.

The canonical allocator interface is `malloc(size)` and `free(ptr)`. The allocator's job is to maintain a pool of pages, satisfy allocation requests by finding or creating space, and reclaim space when objects are freed. The allocator must answer: where should this allocation go? When can freed memory be reused? How can pages be returned to the OS when they are no longer needed?

## Metadata

Metadata is the information an allocator stores about its own state—data structures tracking which memory is allocated, which is free, and how to find available space. Metadata is overhead: memory the allocator uses for bookkeeping, not for user data.

Consider a simple allocator maintaining a free list—a linked list of available memory blocks. Each list node stores the block's address and size. If the allocator manages 1 million blocks, the free list might contain hundreds of thousands of nodes, consuming megabytes of RAM. This is metadata: memory spent tracking memory.

Metadata cost scales with the number of allocations, not their size. Managing 1 million 64-byte allocations (64MB of user data) might require 8MB of metadata (8 bytes per allocation to store block size and next pointer). That's 12.5% overhead. The ratio worsens for smaller allocations.

**Types of allocator metadata:**

**1. Per-allocation headers:** Many allocators store a small header before each allocation containing its size and status. When you call free(ptr), the allocator reads ptr - 8 to find the header, checks the size, and returns the block to the appropriate free list. Header cost: typically 4-16 bytes per allocation.

**2. Free lists:** Linked lists or trees tracking available blocks by size. Each node stores a pointer and metadata (size, status). Free list cost: proportional to number of free blocks, worst case O(N) where N is total allocations.

**3. Bitmaps:** Arrays of bits indicating which slots are allocated (1) or free (0). A 4KB slab with 64-byte slots needs 64 bits (8 bytes) to track all slots. Bitmap cost: fixed per slab, typically <1% overhead.

**4. Page-level structures:** The allocator tracks which pages exist, their states (partial, full, empty), and their membership in lists. Cost: typically 32-128 bytes per page (4KB data), <3% overhead.

**Why metadata matters:**

Metadata consumes memory (reduces effective capacity), pollutes CPU caches (allocator metadata competes with application data for cache lines), and increases fragmentation (metadata structures themselves must be allocated, creating more fragmentation). Good allocators minimize metadata overhead.

temporal-slab uses bitmaps for slot tracking (8 bytes per slab) and intrusive lists for slab management (no separate nodes—slab headers contain next/prev pointers). Total metadata: ~64 bytes per 4KB slab = 1.6% overhead. This is one of the lowest metadata ratios among production allocators.

## Free List

A free list is a data structure tracking available memory blocks. When memory is freed, the allocator adds it to the free list. When memory is requested, the allocator searches the free list for a suitable block.

The simplest free list is a singly linked list where each free block stores a pointer to the next free block. The allocator's state is just a head pointer. To allocate, pop the first block. To free, push the block to the front. This is O(1) for both operations but wastes memory if blocks vary in size (internal fragmentation).

**Free list strategies:**

**1. Single free list (LIFO/FIFO):** All free blocks in one list, regardless of size. Allocation requires scanning the list for a block large enough (O(N) worst case). Simple but slow for large numbers of allocations.

**2. Segregated free lists:** Separate lists per size class. Allocations of size 64-128 bytes go to list A, 128-256 bytes to list B, etc. Allocation is O(1) if the right list has entries. This is the foundation of slab allocation.

**3. Best-fit trees:** Binary search trees or red-black trees storing blocks sorted by size. Allocation finds the smallest block that fits (minimizes waste). Tree operations are O(log N), better than linear scan but still non-trivial.

**Why free lists cause fragmentation:**

Free lists track **available** memory but do not consolidate adjacent blocks. If you allocate A, B, C in sequence, then free B, the free list contains one entry: B. If you later free A and C, the list contains three separate entries: A, B, C. Even though A-B-C are contiguous in memory, they are tracked separately. A request for the size of A+B+C cannot be satisfied despite sufficient total space. This is fragmentation at the metadata level.

Allocators using free lists must periodically coalesce adjacent blocks, which requires scanning (expensive) or maintaining sorted lists (complex). temporal-slab avoids free lists entirely—slabs use bitmaps, and slab-level state is binary (partial or full). No searching, no coalescing required.

## Churn

Churn is the rate at which a system allocates and frees memory, particularly when those allocations are short-lived and happen continuously. Churn measures how fast objects come and go, not how big they are or how many exist at once.

High-churn workloads allocate thousands of small objects per second, which live for microseconds or milliseconds, are freed quickly, and are immediately replaced by new allocations. This cycle repeats indefinitely. Even if peak memory usage is small, the allocator is constantly working.

**What churn looks like in practice:**

A session store allocates 200-byte session objects at 10,000 requests per second. Each session lives 5 seconds on average. At any moment, 50,000 sessions exist (steady state). But the allocator handles 10,000 allocations and 10,000 frees per second—20,000 operations per second total. This is high churn.

A cache system allocates entries as they are populated and frees them on eviction. If the cache has 1 million entries with a 10-minute TTL and uniform access, it churns ~1,667 entries per second. The cache size is large, but the churn rate is moderate.

**Why churn stresses allocators:**

Under high churn, allocators must continuously find space, track freed objects, merge or split blocks, and maintain metadata. This leads to allocator jitter (occasional slow allocations), RSS drift (memory footprint creeping upward), cache misses from scattered metadata, and unpredictable latency under load.

**Why churn matters more than throughput in HFT:**

HFT engines do not allocate huge amounts of memory—they allocate small amounts constantly. A trading system might allocate 100-byte order objects at 100,000 per second. Peak memory is only a few megabytes, but the churn rate is extreme. Instability comes from the allocator's internal bookkeeping, not from the volume of data. A single 10µs allocation spike can cause a missed trade worth millions.

Churn is the entropy generator that slowly destabilizes long-running processes. temporal-slab is designed specifically for this: grouping allocations by when they happen so short-lived objects die together. This eliminates per-object bookkeeping overhead, fragmentation from interleaved lifetimes, and jitter from metadata growth.

## Spatial Fragmentation

Spatial fragmentation occurs when free memory exists but is scattered into unusable fragments. Consider a 4KB page. A program allocates 100 bytes, then 200 bytes, then 50 bytes. They are placed end-to-end. The program frees the 200-byte allocation in the middle. Now there is a hole: 100 bytes used, 200 bytes free, 50 bytes used. If the next allocation is 300 bytes, it will not fit in the 200-byte hole, even though 200 bytes are free.

Over time, as allocations and frees interleave, memory resembles Swiss cheese: plenty of free space in aggregate, but scattered into holes too small to satisfy requests. Allocation cost rises because the allocator must search for suitable holes. Cache locality degrades because objects are scattered. Metadata grows because the allocator must track many small fragments.

Traditional allocators respond with free lists (tracking available holes by size), tree structures (efficiently finding best-fit holes), and splitting (dividing large holes to satisfy small requests). This works for general-purpose workloads, but the complexity accumulates, and search times grow.

## Temporal Fragmentation

Temporal fragmentation is subtler and more damaging than spatial fragmentation. It occurs when objects with vastly different lifetimes share the same page. This is the primary cause of unbounded RSS growth in long-running systems.

Consider a page containing four objects: A (a session object living for hours), B (a request object living milliseconds), C (a cache entry living minutes), D (a logging buffer freed immediately). When B and D are freed, the page is not empty—A and C are still alive. The OS cannot reclaim the page because it contains live data. The page remains resident, even though half of it is unused.

Over time, as allocations and frees continue, pages accumulate mixtures of live and dead objects. RSS grows even though the working set remains constant. Pages cannot be returned to the OS because they are pinned by even a single long-lived object. This is temporal fragmentation: memory that is allocated but not fully utilized, scattered across pages that cannot be reclaimed.

**Concrete example of temporal fragmentation causing RSS growth:**

A web server handles 1000 requests per second. Each request allocates:
- 1 connection handle (lives 60 seconds, long-lived)
- 10 request objects (live 10ms, short-lived)

Traditional allocator (mixed lifetimes on same pages):

```
Page 1: [Connection 1][Req 1-1][Req 1-2][Connection 2][Req 2-1][Req 2-2]...

After 100ms (requests freed, connections still alive):
Page 1: [Connection 1][FREED ][FREED ][Connection 2][FREED ][FREED ]...
        ↑ Pins page                  ↑ Pins page

Page contains 25% live data, 75% freed, but cannot be reclaimed
```

After 1 hour of operation:
- Live objects: 60,000 connections + transient requests = ~60,000 objects
- Expected RSS: 60,000 × 200 bytes = 12 MB
- Actual RSS: **15-18 MB (25-50% overhead)**
- Cause: 3-6 MB of pages are pinned by sparse long-lived objects

After 24 hours:
- Live objects: Still 60,000 (steady state)
- Expected RSS: 12 MB
- Actual RSS: **18-24 MB (50-100% overhead)**
- Cause: Drift compounds as more pages accumulate mixed lifetimes

**Why this is worse than spatial fragmentation:**

Spatial fragmentation (holes between allocations) can be solved by:
- Free list management (track holes, reuse them)
- Coalescing (merge adjacent free blocks)
- Splitting (divide large blocks for small requests)

Temporal fragmentation cannot be solved without:
- **Compaction** - Move live objects to consolidate pages (expensive: requires copying, invalidates pointers, causes latency spikes)
- **Lifetime prediction** - Guess which objects will live longer (unreliable: applications don't provide hints)
- **Temporal grouping** - Place objects allocated together on the same page (temporal-slab's approach)

Traditional allocators respond with compaction: moving live objects to consolidate them into fewer pages so old pages can be released. But compaction is expensive (requires copying), unpredictable (causes latency spikes), and incompatible with systems expecting stable pointers (pointers become invalid after compaction).

**temporal-slab eliminates temporal fragmentation through lifetime affinity:**

The same web server using temporal-slab with epoch separation:

```
Slab A (Epoch 0, long-lived connections):
[Connection 1][Connection 2][Connection 3]...

Slab B (Epoch 1, short-lived requests at T=0-10s):
[Req 1-1][Req 1-2][Req 2-1][Req 2-2]...

After 100ms:
Slab A: [Connection 1][Connection 2][Connection 3]... (100% full, no waste)
Slab B: [FREED   ][FREED   ][FREED   ][FREED   ]... (100% empty, recyclable)
```

Result:
- Slab A remains full → RSS contribution stable
- Slab B becomes completely empty → Recycled immediately → No RSS growth
- After 24 hours: RSS = 12 MB (0% growth)

**The RSS impact:**

| Allocator | 1 hour RSS | 24 hour RSS | 7 day RSS | RSS Growth |
|-----------|------------|-------------|-----------|------------|
| malloc (glibc) | 15 MB | 20 MB | 25 MB | +108% |
| tcmalloc | 14 MB | 18 MB | 22 MB | +83% |
| jemalloc | 13 MB | 16 MB | 19 MB | +58% |
| **temporal-slab** | **12 MB** | **12 MB** | **12 MB** | **0%** |

Baseline: 60,000 live objects × 200 bytes = 12 MB

temporal-slab's bounded RSS is not a tuning parameter—it is an architectural guarantee arising from lifetime grouping.

## Fragmentation as Entropy

In thermodynamics, entropy measures disorder. A low-entropy system is organized—energy is concentrated and useful. A high-entropy system is disordered—energy is dispersed and unusable. Without external work, entropy increases (second law of thermodynamics).

Memory fragmentation follows the same pattern. At startup, memory is organized: pages are either empty or full. As the program runs, allocations and frees interleave. Pages accumulate mixtures of live and dead objects. The system becomes disordered. Memory is dispersed across many pages, much of it unreclaimable. Fragmentation is entropy.

Traditional allocators treat fragmentation as an engineering problem—better data structures or heuristics can reduce it. This is true to a point. But if allocations and frees are uncorrelated with object lifetimes, fragmentation is inevitable. The allocator can delay it but cannot prevent it without reorganizing memory (compaction), which is itself expensive.

The insight is that the allocator should not fight entropy by constantly reordering memory. It should manage entropy by ensuring objects expire in an organized way—grouping objects with similar lifetimes so that when their lifetimes end, entire pages become empty and can be reclaimed as a unit.

## Working Set

The working set is the subset of a program's memory that is actively used during a period of time. It is the pages the program touches frequently enough that they should remain resident in physical RAM. The working set is distinct from total allocated memory (which may include rarely-accessed pages) and distinct from RSS (which includes all resident pages, even if not actively used).

Consider a database server with 10GB allocated: 1GB for the query engine, 8GB for an in-memory index, 1GB for connection buffers. During normal operation, the server processes queries that access only 2GB of the index (the hot data). The working set is ~3GB (query engine + hot index data + active connections). The remaining 7GB of index data is cold—allocated and resident but rarely touched.

**Why working set matters:**

If the working set exceeds physical RAM, the system begins swapping—evicting pages to disk and reloading them when needed. Swapping causes thrashing: the program spends more time waiting for disk I/O than executing. Performance collapses. A system with 8GB RAM can handle a 12GB working set if most of that is cold, but cannot handle an 8.1GB working set where all 8.1GB is actively used.

Allocators influence the working set through spatial locality: if related objects are placed near each other in memory, they share cache lines and pages, shrinking the working set. If unrelated objects are interleaved, the program touches more pages to access the same amount of useful data, expanding the working set.

**Working set vs RSS:**

RSS is a snapshot of current physical memory usage (right now, 5GB is resident). Working set is a behavioral characteristic (over the last minute, the program accessed 3GB). A program can have high RSS but low working set (many pages resident but rarely touched) or low RSS but high working set (actively uses all resident pages).

temporal-slab influences working set through temporal grouping: objects allocated together are placed in the same slab. If those objects have correlated access patterns (allocated together → accessed together), they share cache lines and pages, improving locality. If allocations are random, locality degrades. Lifetime affinity often aligns with access affinity—objects created for the same request are accessed while processing that request.

## Slab Allocator

A slab allocator is a specialized memory allocator designed for fixed-size allocations. Instead of managing arbitrary-sized blocks, a slab allocator divides memory into slabs—fixed-size containers for objects of a single size class. Each slab holds multiple objects of the same size, and allocation is finding a free slot within a slab.

The key insight is that fixed-size allocation eliminates the need for complex free list management. There is no search for a block large enough, no splitting of blocks, no coalescing of adjacent blocks. Slots are pre-sized and pre-allocated. Allocation becomes bitmap manipulation: find a free bit, set it to 1, return the corresponding slot.

**Slab allocator architecture:**

```
Size Class 128 bytes:
  Slab A: [allocated][free][allocated][free][free]...  (63 slots)
  Slab B: [allocated][allocated][free][free]...        (63 slots)
  Slab C: [free][free][free]...                        (63 slots)

Size Class 256 bytes:
  Slab D: [allocated][free][allocated]...              (31 slots)
  Slab E: [free][free][free]...                        (31 slots)
```

Each size class maintains a pool of slabs. To allocate a 100-byte object:
1. Round up to 128-byte size class
2. Find a slab with free slots (e.g., Slab A)
3. Allocate a slot from Slab A (bitmap operation)
4. Return pointer to the slot

To free:
1. Determine which slab the pointer belongs to (via address arithmetic or handle)
2. Mark the slot as free in the bitmap
3. Update slab state (if slab became empty or partial)

**Origins:**

The slab allocator was invented by Jeff Bonwick in 1994 for the Solaris kernel. The problem: the kernel allocates many small objects (process descriptors, file handles, network buffers) with the same sizes repeatedly. Traditional allocators (dlmalloc, BSD malloc) are designed for arbitrary sizes and suffer high metadata overhead for small, fixed-size allocations. Slab allocation reduced kernel memory allocator overhead by 80% and became the standard for kernel-level allocation (Linux SLAB, SLUB, SLOB).

temporal-slab adapts this for user-space, adding temporal grouping (lifetime affinity), lock-free fast paths, and conservative recycling (FULL-only reclamation). The core principle remains: fixed-size slots eliminate search overhead and enable O(1) allocation/free.

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

## External Fragmentation

External fragmentation is wasted space between allocations—memory that is free but unusable because it is split into fragments too small to satisfy requests. This is distinct from internal fragmentation (wasted space within allocations due to rounding up to size classes).

Consider a 4KB page managed by a traditional allocator. The program allocates 500 bytes, then 300 bytes, then 200 bytes, filling 1000 bytes. Later it frees the 300-byte allocation, leaving a 300-byte hole between the first and third allocations. If the next request is for 400 bytes, the allocator cannot satisfy it from this page despite 3000 bytes being free (3096 bytes unused + 300-byte hole), because no single contiguous region is 400 bytes.

External fragmentation accumulates as allocations and frees interleave. Memory becomes Swiss cheese—many small free regions, none large enough to be useful. The allocator must search many pages to find space, or resort to splitting (breaking a large block into smaller pieces) and coalescing (merging adjacent free blocks), both of which are expensive.

**Why slab allocators eliminate external fragmentation:**

In a slab allocator, every slot in a slab is the same size. There are no holes of varying sizes—slots are either allocated or free, and all free slots are equally usable. A 128-byte slab with 63 slots has either N allocated slots and (63 - N) free slots. Every free slot can satisfy a 128-byte request. There is no fragmentation at the slot level.

At the slab level, fragmentation is temporal, not spatial. A slab with 1 allocated slot and 62 free slots is "fragmented" in the sense that it is mostly unused, but all 62 free slots are immediately usable—no search, no splitting required. This is why slab allocators trade internal fragmentation (waste within slots) for elimination of external fragmentation (waste between slots).

temporal-slab goes further: slabs are filled sequentially, so objects allocated around the same time are adjacent in memory. This improves cache locality (objects accessed together are nearby) and enables whole-slab recycling (when lifetime-correlated objects die together, the entire slab becomes free).

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

## Bitmap Allocation

A bitmap is a compact data structure representing the allocation state of slots in a slab. Each slot has a corresponding bit: 1 means allocated, 0 means free. A 4KB slab with 64-byte slots has 63 slots (after metadata), requiring 63 bits—8 bytes—to track all slots.

**Bitmap structure for a 128-byte slab (63 slots):**

```
Bitmap: [uint64_t bitmap]
Bits:   0123456789...62
State:  1010110000...01

Bit 0 = 1: Slot 0 allocated
Bit 1 = 0: Slot 1 free
Bit 2 = 1: Slot 2 allocated
...
```

**Allocation algorithm:**

To allocate a slot:
1. Find the first 0 bit in the bitmap (first free slot)
2. Atomically set that bit to 1 using compare-and-swap
3. Calculate slot address: slab_base + (slot_index × slot_size)
4. Return pointer to the slot

Finding the first 0 bit is a single CPU instruction on most architectures: `__builtin_ctzll` (count trailing zeros) or equivalent. On x86-64, this compiles to the BSF (bit scan forward) instruction—one cycle, no branches.

**Free algorithm:**

To free a slot:
1. Calculate slot index: (ptr - slab_base) / slot_size
2. Atomically clear bit at slot_index (set to 0)
3. Update slab free count

**Why bitmaps are efficient:**

Bitmaps are cache-friendly: 64 slot states fit in 8 bytes (one cache line). Bitmap operations are CPU-native: CPUs have instructions to find first zero bit, count bits, set/clear bits atomically. Bitmaps have no memory overhead beyond the bits themselves—no linked list nodes, no per-slot headers.

Compare to free lists: a free list for 63 slots might store 63 pointers (504 bytes) plus metadata. The bitmap stores 8 bytes. That's 63× less metadata, all in one cache line.

temporal-slab uses 64-bit bitmaps stored in the slab header. Lock-free allocation uses atomic CAS loops on the bitmap to claim slots without mutexes. Bitmap operations are the foundation of the sub-100ns allocation fast path.

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

## Compare-and-Swap (CAS)

Compare-and-swap (CAS) is an atomic CPU instruction that enables lock-free programming. CAS takes three arguments: a memory location, an expected value, and a new value. It atomically performs:

```
if (*location == expected) {
    *location = new_value;
    return true;  // Success
} else {
    return false; // Failure—another thread modified *location
}
```

The key property is atomicity: the check and update happen as a single indivisible operation. No other thread can modify the memory location between the check and the update. This is implemented in hardware—on x86-64, CAS compiles to the CMPXCHG instruction with a LOCK prefix, which locks the memory bus for the duration of the operation.

**Example: Lock-free counter increment**

Naive (incorrect):
```c
int counter = 0;

void increment() {
    int old = counter;   // Thread A reads 0
                         // Thread B reads 0
    counter = old + 1;   // Thread A writes 1
                         // Thread B writes 1
}
// Lost update! Counter should be 2, but is 1.
```

Correct (CAS loop):
```c
void increment() {
    int old, new;
    do {
        old = counter;           // Read current value
        new = old + 1;           // Compute new value
    } while (!CAS(&counter, old, new));  // Try to update
    // If CAS fails, another thread changed counter—retry
}
```

**How CAS enables lock-free allocation:**

temporal-slab uses CAS to claim bits in the slab bitmap:

```c
uint64_t old_bitmap, new_bitmap;
do {
    old_bitmap = slab->bitmap;                // Read current bitmap
    int slot = find_first_zero_bit(old_bitmap); // Find free slot
    if (slot == -1) return NULL;              // Slab is full
    new_bitmap = old_bitmap | (1ULL << slot); // Set bit to 1
} while (!CAS(&slab->bitmap, old_bitmap, new_bitmap));
```

If another thread claims the same slot between reading the bitmap and the CAS, the CAS fails. The loop retries with the updated bitmap. This eliminates the need for mutexes—multiple threads can allocate from the same slab concurrently, with only the CAS instruction as synchronization.

**Cost of CAS:**

On x86-64, a successful CAS takes ~20-40 cycles (vs ~1-2 cycles for a normal load/store). The LOCK prefix forces cache coherence—other cores' caches are invalidated, ensuring all cores see the updated value. For contended memory (multiple threads accessing the same cache line), this can be slower (~100+ cycles) due to cache coherence traffic.

But CAS is still far cheaper than mutexes. A mutex acquire/release pair takes ~100-200 cycles minimum (syscall overhead in worst case), plus potential thread sleep/wake overhead if contended. CAS is 5-10× faster and has no risk of thread blocking.

## Atomic Operations

Atomic operations are CPU instructions that execute indivisibly—no other thread can observe a partially completed atomic operation. Atomics are the foundation of lock-free programming.

**Types of atomic operations:**

**1. Load/Store:**
```c
int value = atomic_load(&counter);      // Atomic read
atomic_store(&counter, 42);             // Atomic write
```

Ensures the load or store is not split into multiple operations. On most architectures, aligned loads/stores are naturally atomic (e.g., reading a 64-bit value on a 64-bit CPU). But unaligned accesses or multi-word operations may not be atomic without explicit instructions.

**2. Read-Modify-Write:**
```c
atomic_fetch_add(&counter, 1);          // Atomically increment
atomic_fetch_sub(&counter, 1);          // Atomically decrement
atomic_fetch_or(&bitmap, 0x1);          // Atomically set bits
atomic_fetch_and(&bitmap, ~0x1);        // Atomically clear bits
```

These combine read, modify, and write into one atomic operation. On x86-64, these compile to instructions like LOCK ADD, LOCK SUB, LOCK OR—hardware-level atomicity.

**3. Compare-and-Swap (covered above)**

**Memory ordering:**

Atomic operations can specify memory ordering constraints:
- **Relaxed:** No ordering guarantees. Fastest.
- **Acquire:** All subsequent reads/writes happen after this operation.
- **Release:** All prior reads/writes happen before this operation.
- **AcqRel:** Combination of acquire and release.
- **SeqCst:** Sequential consistency—total order across all threads.

temporal-slab uses acquire semantics for loading `current_partial` (ensures slab metadata is visible before accessing the slab) and release semantics for publishing a new `current_partial` (ensures slab initialization is complete before other threads see it).

**Why atomics matter for allocators:**

Lock-free allocation requires atomics for:
- Reading/writing `current_partial` pointer (other threads must see consistent state)
- Claiming bits in bitmaps (prevent multiple threads from claiming the same slot)
- Updating reference counts (track slab usage across threads)

Without atomics, race conditions cause double allocations, memory corruption, and crashes. With atomics, lock-free code is both safe and fast.

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

## Branch Prediction and Misprediction

Modern CPUs use pipelining to execute multiple instructions simultaneously. The pipeline stages instructions (fetch, decode, execute, write-back) so that while one instruction executes, the next is being decoded, and the next is being fetched. This achieves high throughput—multiple instructions complete per clock cycle.

Branches (if/else, loops, switches) break the pipeline. When the CPU encounters a branch, it doesn't know which path to take until the condition evaluates. To avoid stalling, CPUs predict which path will be taken and speculatively execute instructions down that path. If the prediction is correct, execution continues at full speed. If the prediction is wrong, the CPU flushes the pipeline (discards speculative work) and restarts from the correct path. This is a branch misprediction.

**Cost of misprediction:**

A branch misprediction costs 10-20 cycles on modern CPUs—the time to flush the pipeline and reload. For context, a single L1 cache access takes 4 cycles, an L2 access takes 12 cycles, a DRAM access takes 100 cycles. A misprediction is equivalent to 2-3 L2 cache misses—significant overhead.

**Branch prediction strategies:**

**1. Static prediction:** The CPU assumes branches follow a fixed pattern (e.g., backward branches—loop exits—are predicted taken, forward branches—error paths—are predicted not taken). Simple but inflexible.

**2. Dynamic prediction:** The CPU maintains a history of recent branch outcomes and predicts based on past behavior. A loop that runs 100 times will be predicted taken 99 times, not taken once. Accurate for predictable patterns (~95% hit rate).

**3. Unpredictable branches:** If a branch depends on input data (e.g., if (key % 2 == 0)), the pattern is random. The predictor fails ~50% of the time. Every other branch mispredicts—10-20 cycles wasted per branch.

**Why branch prediction matters for allocators:**

Size class selection via linear scan is branch-heavy:

```c
uint8_t select_size_class(size_t size) {
    if (size <= 64)  return 0;
    if (size <= 96)  return 1;
    if (size <= 128) return 2;
    if (size <= 192) return 3;
    if (size <= 256) return 4;
    if (size <= 384) return 5;
    if (size <= 512) return 6;
    return 7;
}
```

If allocation sizes are unpredictable (e.g., 72, 200, 80, 300), each if is a 50/50 coin flip. Expected mispredictions: 4 per call (half of 8 branches). Cost: 40-80 cycles (~10-25 nanoseconds at 3GHz). This is jitter—unpredictable latency that varies by allocation size.

temporal-slab eliminates branches via lookup table:

```c
static uint8_t k_class_lookup[768];  // Precomputed at initialization

uint8_t select_size_class(size_t size) {
    return k_class_lookup[size];  // One load, zero branches
}
```

This is deterministic: always 1 L1 cache access (~4 cycles). No branches, no mispredictions, no jitter. For HFT systems where every nanosecond matters, eliminating branch jitter is critical.

## Handle-Based API vs Malloc-Style API

temporal-slab provides two APIs:

**Handle-based:** `alloc_obj(alloc, size, &handle)` returns a pointer and an opaque handle. `free_obj(alloc, handle)` frees by handle. The handle encodes the slab pointer, slot index, and size class. No per-allocation metadata is stored (zero overhead). The application must track handles alongside pointers.

**Malloc-style:** `slab_malloc(alloc, size)` returns a pointer. The handle is stored in an 8-byte header before the pointer. `slab_free(alloc, ptr)` reads the header, extracts the handle, and calls `free_obj` internally. This trades 8 bytes overhead per allocation for API compatibility.

The handle-based API is suitable for performance-critical code where zero overhead is required and explicit handle management is acceptable. The malloc-style API is suitable for drop-in replacement where 8 bytes overhead is tolerable.

## How Design Choices Prevent RSS Growth

All of temporal-slab's design decisions converge on a single goal: **bounded RSS under sustained churn**. This section shows how the pieces fit together.

**The RSS growth problem in traditional allocators:**

Traditional allocators (malloc, jemalloc, tcmalloc) suffer unbounded RSS growth because they:
1. **Mix lifetimes on pages** - Place objects with different lifetimes adjacent to each other
2. **Cannot reclaim partially-used pages** - A single long-lived object pins an entire 4KB page
3. **Accumulate pinned pages over time** - More pages become partially-used as churn continues
4. **Lack temporal structure** - No mechanism to group objects by when they were created

Result: `RSS(t) = baseline + drift(t)` where `drift(t)` grows linearly with time.

**How temporal-slab prevents RSS growth:**

**1. Temporal grouping (Lifetime Affinity + Epochs):**
- Objects allocated in the same epoch share slabs
- Epoch advancement separates lifetime cohorts
- Short-lived objects die together → Entire slab becomes empty
- Long-lived objects remain grouped → No wasted space in their slabs

**2. Fixed-size slots (Size Classes + Bitmap Allocation):**
- No external fragmentation (all slots are same size, no holes)
- Bitmap tracks slot state in 8 bytes per slab (1.6% overhead)
- O(1) allocation/free (find first zero bit, atomic CAS)

**3. Conservative recycling (FULL-only + Slab Cache):**
- Empty slabs from FULL list are recycled (safe, no race conditions)
- Recycled slabs enter the cache (32 per size class)
- Cache hits reuse existing slabs → RSS doesn't grow
- Overflow list tracks excess empty slabs → Bounded growth

**4. Refusal to unmap:**
- Slabs remain mapped even when empty → RSS reflects high-water mark
- No mmap/munmap churn → Predictable RSS ceiling
- Tradeoff: Higher RSS floor for safety and predictability

**The complete RSS equation for temporal-slab:**

```
RSS(t) = max_live_set + slab_overhead + cache_size

Where:
- max_live_set = Peak number of live objects × object size (rounded to pages)
- slab_overhead = Metadata (64 bytes per slab = 1.6% of max_live_set)
- cache_size = 32 slabs × 4KB × num_size_classes ≈ 1-2 MB (constant)

Result: RSS is CONSTANT after reaching steady state
```

**Concrete example showing all components:**

Session store: 50,000 sessions at steady state, 10,000 alloc/free per second, 200-byte sessions

**Step 1: Temporal grouping separates lifetimes**
```
Epoch 0: Long-lived backbone (static configuration, ~1000 objects)
Epoch 1-15: Rotating sessions (each epoch covers ~5 seconds of allocations)

Slab A (Epoch 0): [Config 1][Config 2][Config 3]... (stays full forever)
Slab B (Epoch 1): [Session 1][Session 2]... (becomes empty after 60 seconds)
Slab C (Epoch 2): [Session 501][Session 502]... (becomes empty after 60 seconds)
```

**Step 2: Fixed-size slots eliminate external fragmentation**
```
200-byte objects → Round to 256-byte size class
256-byte slabs hold 15 objects each (4096 / 256)
Bitmap: 15 bits = 2 bytes per slab
No holes, no search, O(1) allocation
```

**Step 3: Conservative recycling reuses empty slabs**
```
At T=60 seconds:
- Epoch 1 sessions all freed
- Slab B moves from FULL → PARTIAL → EMPTY
- Slab B pushed to cache
- Next allocation at T=61 pops Slab B from cache (reused, no new mmap)
```

**Step 4: Refusal to unmap provides RSS ceiling**
```
Peak allocation: 50,000 sessions = 50,000 / 15 = 3,334 slabs = 13.3 MB
After 1 hour: 3,334 slabs in cache/use = 13.3 MB RSS
After 1 week: 3,334 slabs in cache/use = 13.3 MB RSS (0% growth)

If we unmapped empty slabs:
- Lower RSS ceiling (11-12 MB)
- But: mmap/munmap churn (~1000 syscalls/second)
- And: Cannot validate stale handles (segfaults instead of errors)
```

**RSS growth comparison over time:**

| Time | malloc RSS | jemalloc RSS | temporal-slab RSS |
|------|-----------|-------------|-------------------|
| 1 min | 13 MB | 13 MB | 13.3 MB |
| 1 hour | 15 MB (+15%) | 14 MB (+8%) | **13.3 MB (0%)** |
| 24 hours | 20 MB (+54%) | 16 MB (+23%) | **13.3 MB (0%)** |
| 7 days | 25 MB (+92%) | 19 MB (+46%) | **13.3 MB (0%)** |

**Why temporal-slab wins:**

Traditional allocators cannot prevent temporal fragmentation without compaction (expensive, unpredictable latency). temporal-slab prevents it architecturally by grouping lifetimes at allocation time.

**The tradeoff:**

temporal-slab achieves 0% RSS growth but has:
- Higher RSS floor (+37% vs malloc at startup due to slab alignment and cache)
- Fixed-size classes only (768 bytes max, 11.1% internal fragmentation)
- Requires lifetime-correlated allocation patterns (works for sessions, connections, requests; doesn't help random sizes)

For long-running systems where RSS drift is unacceptable (services that must run for weeks without restart), this is the right tradeoff.

## What You Should Understand Now

At this point, you understand the foundational concepts:

Memory allocation bridges the gap between OS-provided pages and application-needed objects. Allocators face two fragmentation problems: spatial (free space scattered into unusable holes) and temporal (pages pinned by mixtures of live and dead objects). Temporal fragmentation is entropy—inevitable without organizing allocations by lifetime.

Slab allocation solves this by dividing pages into fixed-size slots (size classes) and filling slabs sequentially. Objects allocated around the same time end up in the same slab. If they have correlated lifetimes (allocation-order affinity), they die together, and the slab can be recycled as a unit. This manages entropy by grouping objects to expire in an organized way.

temporal-slab extends this with lock-free fast-path allocation (sub-100ns latency), conservative recycling (only full slabs are recycled, eliminating use-after-free races), refusal to unmap (enabling safe handle validation and bounded RSS), and O(1) class selection (eliminating jitter).

The allocator does not predict lifetimes or require application hints. Lifetime alignment emerges naturally from allocation patterns. This is substrate, not policy—a foundation for higher-level systems to build on.

The implementation details—how bitmaps encode slot state, how atomic CAS loops allocate slots, how list membership is tracked—follow from these foundational concepts. Those details are in the source code. This document provides the foundation to understand why they exist.
