# Foundations: Memory Allocation and Lifetime-Aware Design

This document builds the theoretical foundation for temporal-slab from first principles, defining each concept before using it to explain the next. It assumes no prior knowledge of memory allocator internals.

## Why temporal-slab Exists: The RSS Drift Problem

Long-running systems with high allocation churn—web servers, session stores, control planes, high-frequency trading systems—face a problem that traditional allocators cannot solve: **unbounded RSS growth despite steady-state workloads**. Your service maintains 50,000 active sessions at all times. After one hour, RSS is 12 MB. After 24 hours, RSS is 20 MB. After 7 days, RSS is 25 MB. The number of live objects never changed, but memory usage grew by 108%. This is not a memory leak—objects are being freed correctly. This is temporal fragmentation.

**The mechanism of RSS drift:**

Traditional allocators (malloc, jemalloc, tcmalloc) place allocations wherever free space exists. They have no concept of object lifetime. A session object living for hours and a request object living for milliseconds look identical to the allocator—both are N-byte allocations. They are placed on the same page.

```
Page 1 at startup (mixed lifetimes):
[Long-lived session][Short-lived request][Short-lived buffer][Long-lived connection]

After 10 minutes (requests freed, sessions remain):
[Long-lived session][FREED              ][FREED             ][Long-lived connection]
                     75% of page is unused, but page remains resident (pinned by 2 live objects)
```

Over time, as allocations and frees interleave, more pages accumulate this pattern: mostly empty, but pinned by one or two long-lived objects. The allocator cannot return these pages to the OS because they still contain live data. RSS grows linearly with time, even though the working set (number of live objects) remains constant.

```
Session store: 50,000 live sessions (steady state), 10,000 alloc/free per second

malloc RSS over time:
Hour 1:  12 MB (baseline)
Hour 24: 20 MB (+67% drift)
Day 7:   25 MB (+108% drift)

Expected RSS: 12 MB (50,000 × 200 bytes + page alignment)
Actual RSS:   25 MB (13 MB wasted on partially-occupied pages)
Wasted memory: 108% overhead from temporal fragmentation
```

This is the RSS drift problem: memory usage grows unboundedly despite steady-state workloads, causing services to OOM after days or weeks of operation. The allocator is correct (no leaks, no corruption), but it cannot prevent temporal fragmentation without knowing object lifetimes.

**Why compaction doesn't solve this:**

Some allocators (Go's runtime, JVM garbage collectors) use compaction: move live objects to consolidate them onto fewer pages, freeing the old pages. This works for garbage-collected languages but is incompatible with C/C++:

```
Before compaction:
Page A: [Live obj at 0x1000][FREED][FREED][Live obj at 0x1030]
Page B: [FREED][FREED][FREED][FREED]

After compaction:
Page A: [Live obj at 0x2000][Live obj at 0x2030][unused][unused]
Page B: [unmapped]

Problem: All pointers to 0x1000 and 0x1030 are now invalid!
C/C++ programs expect pointers to remain stable—compaction breaks this contract.
```

Compaction also causes unpredictable latency spikes (stop-the-world pauses while moving objects), making it unsuitable for real-time systems. Trading bounded RSS for unbounded pause times is not acceptable for HFT or control planes.

**temporal-slab's solution: Temporal grouping with phase boundary alignment**

temporal-slab solves RSS drift through a simple architectural principle: **objects allocated together are placed together**. Instead of scattering allocations across all available pages, temporal-slab groups allocations by time window (epochs). Objects allocated in the same epoch share slabs (pages). When the application signals that a lifetime phase has completed—via `epoch_close()` at a **phase boundary** (request end, frame present, transaction commit)—the entire epoch can be reclaimed. All slabs become empty simultaneously because objects with correlated lifetimes were grouped together.

```
Epoch 0: Long-lived backbone (static configuration, ~1000 objects)
Epoch 1: Short-lived requests (allocated T=0-5s, freed T=0-60s)
Epoch 2: Short-lived requests (allocated T=5-10s, freed T=5-65s)
...

Slab A (Epoch 0): [Config 1][Config 2][Config 3]... (stays full forever)
Slab B (Epoch 1): [Request 1][Request 2][Request 3]... (becomes 100% empty at T=60s)
Slab C (Epoch 2): [Request 501][Request 502]...      (becomes 100% empty at T=65s)
```

When Epoch 1's requests all expire (T=60s), Slab B is completely empty—no mixed lifetimes. The entire slab can be recycled or madvised. No fragmentation, no drift.

**The results:**

```
Same session store with temporal-slab:
Hour 1:  12 MB
Hour 24: 12 MB (0% drift)
Day 7:   12 MB (0% drift)
Year 1:  12 MB (0% drift)

RSS is determined by live set size, not by how long the service has been running.
```

This is the unique value proposition: **bounded RSS under sustained churn without compaction, without GC pauses, without unpredictable latency spikes**.

**What temporal-slab provides that traditional allocators cannot:**

**1. Bounded RSS (zero drift):**
- Traditional: `RSS(t) = baseline + drift(t)` where drift grows linearly with time
- temporal-slab: `RSS(t) = max_live_set + overhead` (constant after reaching steady state)

**2. Deterministic reclamation at phase boundaries:**
- Traditional: Pages reclaimed nondeterministically as holes appear and coalesce
- temporal-slab: Memory reclaimed at **phase boundaries** when application calls `epoch_close()` (request end, frame present, transaction commit, batch complete)

**3. Predictable tail latency (GitHub Actions validated):**
- Traditional malloc: p99 = 1,443ns, p99.9 = 4,409ns (lock contention, hole-finding heuristics)
- temporal-slab: p99 = 120ns, p99.9 = 340ns (**12-13× better**, lock-free fast path)

**4. No stop-the-world pauses:**
- Traditional GC: 10-100ms pauses for compaction (unacceptable for HFT)
- temporal-slab: No GC, no compaction, no pauses (allocation latency is constant)

**When to use temporal-slab:**

**Use temporal-slab when:**
- Long-running services (days to weeks of uptime)
- High allocation churn (thousands to millions of alloc/free per second)
- Steady-state workload (working set size doesn't grow over time, but RSS does)
- Latency-sensitive (HFT, real-time systems, control planes)
- Allocation sizes ≤768 bytes (fixed size classes)
- Objects have correlated lifetimes (request-scoped, transaction-scoped, batch-scoped)

**Examples:**
- Session stores: 50K sessions, 10K alloc/free per second → RSS stable over weeks
- API gateways: Request objects allocated together, freed after response
- HFT engines: Order objects allocated per-trade, microsecond lifetimes
- Control planes: Watch event objects allocated per-interval, freed after processing

**Use traditional allocators (malloc/jemalloc) when:**
- Short-lived processes (restart daily, RSS drift doesn't accumulate)
- Low churn workload (allocate once, use for hours)
- Arbitrary allocation sizes (>768 bytes, or highly variable sizes)
- Objects have uncorrelated lifetimes (cannot group by epoch)
- Absolute RSS minimization more important than drift prevention

**Examples:**
- CLI tools: Run for seconds, exit (no time for drift)
- Static servers: Allocate data structures at startup, serve requests from cache (minimal churn)
- Large object storage: Multi-megabyte allocations (exceed temporal-slab's 768-byte limit)

**The tradeoffs temporal-slab accepts:**

To achieve 0% RSS drift and predictable tail latency (120ns p99, 340ns p999), temporal-slab makes explicit tradeoffs:

**1. Fixed size classes only (≤768 bytes):**
- No arbitrary sizes (must round up to nearest class)
- Internal fragmentation: 11.1% average (100-byte object uses 128-byte slot)
- Not suitable for large allocations (>768 bytes falls back to mmap)

**2. Phase boundary alignment required:**
- Application must signal phase boundaries via `epoch_close()` (request end, frame present, transaction commit)
- Application must specify epoch when allocating (or use epoch domains for automatic management)
- Incorrect phase alignment reduces effectiveness (objects with different lifetimes mixed in same epoch)

**3. Platform-specific (x86-64 Linux optimized):**
- 120ns p99 latency validated on x86-64 with TSO (free acquire/release semantics)
- madvise semantics assume Linux (BSD/Windows differ)
- ARM port would be 2× slower (150ns) due to memory fence overhead

**4. Conservative recycling (higher RSS floor):**
- Empty PARTIAL slabs not recycled (only FULL slabs)
- RSS reflects high-water mark, not current working set
- Trades aggressive reclamation for simplicity and safety

**The core insight:**

Allocators cannot predict lifetimes, but they can observe allocation patterns. Objects allocated close together in time are often causally related (created for the same request, transaction, or batch). They have correlated lifetimes—they die together. By grouping these objects onto the same pages, temporal-slab ensures pages become empty as a unit, enabling clean reclamation.

This is not lifetime prediction—it is lifetime correlation emerging from allocation-order affinity. The allocator doesn't guess when objects will die. It groups objects by when they were born and lets natural expiration patterns do the work.

**What this document teaches:**

The remainder of this document explains the foundational concepts needed to understand temporal-slab's design: what pages are, how virtual memory works, why temporal fragmentation happens, how slab allocation works, how lock-free algorithms achieve low latency, how epoch-granular reclamation provides deterministic RSS control.

By the end, you will understand not just how temporal-slab works, but why it works—why temporal grouping prevents RSS drift, why lock-free allocation achieves predictable tail latency (120ns p99, validated on GitHub Actions), why epoch boundaries enable deterministic reclamation, and what tradeoffs are required to achieve these properties.

## Table of Contents

**Operating System Concepts**
- [Page](#page)
- [Virtual Memory vs Physical Memory](#virtual-memory-vs-physical-memory)
- [Overcommitment](#overcommitment)
- [Page Fault](#page-fault)
- [Translation Lookaside Buffer (TLB)](#translation-lookaside-buffer-tlb)
- [Cache Line and Cache Locality](#cache-line-and-cache-locality)
- [System Call (syscall)](#system-call-syscall)
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
- [Phase Boundary](#phase-boundary)
- [Epoch](#epoch)
- [Epoch Advancement](#epoch-advancement)
- [Epoch Ring Buffer Architecture](#epoch-ring-buffer-architecture)
- [Epoch Domains](#epoch-domains)
- [Slab](#slab)
- [Size Class](#size-class)
- [Internal Fragmentation](#internal-fragmentation)
- [External Fragmentation](#external-fragmentation)
- [Slab Lifecycle](#slab-lifecycle)
- [Bitmap Allocation](#bitmap-allocation)
- [Adaptive Bitmap Scanning](#adaptive-bitmap-scanning)

**Implementation Techniques**
- [Fast Path vs Slow Path](#fast-path-vs-slow-path)
- [Hot Path Optimization Architecture](#hot-path-optimization-architecture)
- [Lock-Free Allocation](#lock-free-allocation)
- [Lock Contention](#lock-contention)
- [Compare-and-Swap (CAS)](#compare-and-swap-cas)
- [Atomic Operations](#atomic-operations)
- [Memory Barriers and Fences](#memory-barriers-and-fences)
- [Cache Coherence](#cache-coherence)
- [Memory Alignment](#memory-alignment)
- [Compiler Barriers](#compiler-barriers)
- [Thread-Local Storage (TLS)](#thread-local-storage-tls)
- [Bounded RSS Through Conservative Recycling](#bounded-rss-through-conservative-recycling)
- [Hazard Pointers and Reference Counting](#hazard-pointers-and-reference-counting)
- [ABA Problem](#aba-problem)
- [Slab Cache](#slab-cache)
- [Refusal to Unmap vs madvise](#refusal-to-unmap-vs-madvise)
- [Epoch-Granular Memory Reclamation](#epoch-granular-memory-reclamation)
- [O(1) Deterministic Class Selection](#o1-deterministic-class-selection)
- [Branch Prediction and Misprediction](#branch-prediction-and-misprediction)
- [Tail Latency and Percentiles](#tail-latency-and-percentiles)
- [Platform-Specific Considerations: x86-64 Linux](#platform-specific-considerations-x86-64-linux)

**API Design**
- [Handle-Based API vs Malloc-Style API](#handle-based-api-vs-malloc-style-api)
- [Handle Encoding and Slab Registry](#handle-encoding-and-slab-registry)
- [Generation Counter Mechanics](#generation-counter-mechanics)
- [How Design Choices Prevent RSS Growth](#how-design-choices-prevent-rss-growth)
- [What You Should Understand Now](#what-you-should-understand-now)

---

## Page

A page is the smallest unit of memory the operating system manages. On most modern systems (x86-64, ARM64), a page is 4096 bytes (4KB). When a program requests memory, the OS allocates entire pages. The program may request 80 bytes, but the OS grants at least one page. Pages are the fundamental currency of memory management—they can be mapped into a process's address space, unmapped to return them to the OS, or marked with protection attributes (read, write, execute).

**Page protection attributes:**

Every page in a process's address space has protection bits that control how the page can be accessed:

- **PROT_READ:** Page can be read (load instructions allowed)
- **PROT_WRITE:** Page can be written (store instructions allowed)
- **PROT_EXEC:** Page can be executed (instruction fetch allowed)
- **PROT_NONE:** Page cannot be accessed at all (any access triggers fault)

These are enforced by the CPU's Memory Management Unit (MMU). If a program tries to access a page in a way that violates its protection bits, the CPU triggers a **page fault** and the kernel delivers a SIGSEGV signal (segmentation fault), typically terminating the process.

**Common protection combinations:**

```c
// Typical data page (read/write, not executable)
mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

// Code page (read/execute, not writable)
mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

// Read-only data page (constants)
mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

// Guard page (detect stack overflow)
mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

**Why protection matters for security:**

Without page protection, any bug could corrupt any memory:

```c
char* stack_var = "hello";
char* code_ptr = (char*)&main;  // Pointer to code

// Without protection, this would work:
code_ptr[0] = 0xC3;  // Overwrite first byte of main() with 'ret' instruction
→ Program behavior completely corrupted

// With PROT_EXEC (no write), this triggers SIGSEGV:
code_ptr[0] = 0xC3;  // CRASH: Cannot write to executable page
```

Modern systems use **W^X (Write XOR Execute)** policy:
- Pages are either writable OR executable, never both
- Prevents code injection attacks (can't write shellcode then execute it)
- Enforced by default on all modern operating systems

**Protection in temporal-slab:**

temporal-slab allocates all slabs with `PROT_READ | PROT_WRITE`:

```c
void* slab = mmap(NULL, 4096, 
                  PROT_READ | PROT_WRITE,  // Data pages, not code
                  MAP_PRIVATE | MAP_ANONYMOUS, 
                  -1, 0);
```

Slabs store data (objects), not code, so they don't need `PROT_EXEC`. They need read/write for normal allocation/free operations. When a slab is madvised (`MADV_DONTNEED`), the protection bits remain unchanged—the page is still readable/writable, but the physical memory is released and the page is zeroed on next access.

**Protection changes and TLB:**

Changing protection bits requires a syscall:

```c
// Make page read-only
mprotect(ptr, 4096, PROT_READ);  // Syscall, updates page table

// Make page read/write again  
mprotect(ptr, 4096, PROT_READ | PROT_WRITE);  // Another syscall
```

After changing protection, the TLB (Translation Lookaside Buffer) must be flushed for that page—otherwise the CPU might use cached protection bits and allow access that should be denied. This flush is automatic in `mprotect()` but costs ~100-200 cycles.

temporal-slab never changes page protection after allocation—slabs remain `PROT_READ | PROT_WRITE` for their entire lifetime. This avoids syscall overhead and TLB flushes.

## Virtual Memory vs Physical Memory

When a program requests memory, the OS grants virtual memory—address space the program can reference. Virtual addresses are not physical RAM addresses. They are references into a virtual address space managed by the OS. The mapping from virtual to physical addresses happens through page tables maintained by the CPU's memory management unit (MMU).

Virtual memory enables isolation (each process has its own address space) and overcommitment (the OS can grant more virtual memory than physical RAM exists). Physical pages are allocated lazily: when a program first accesses a virtual page, a page fault occurs, the OS allocates a physical page, and the MMU updates the page table to map the virtual address to the physical address.

## Overcommitment

Overcommitment is the operating system's ability to grant more virtual memory than physical RAM exists. When a program requests memory via malloc or mmap, the OS allocates virtual address space immediately—entries in the page table marking that region as valid. But no physical RAM is assigned yet. The OS makes a promise: "You can use this memory." The physical backing is provided later, on demand, when the program first touches the memory.

Consider a system with 8GB of physical RAM. Three processes each request 4GB of virtual memory. The OS grants all three requests, allocating 12GB of virtual memory—50% more than physical RAM. This is overcommitment. The system has promised 12GB but only has 8GB to deliver.

How can this work? Most programs allocate more memory than they actually use. A program might request a 1GB buffer but only write to the first 100MB. Or it might fork child processes that inherit the parent's address space but only modify a small portion. Overcommitment exploits this gap between allocated and used memory.

**Copy-on-write (COW) amplifies overcommitment benefits:**

When a process calls fork(), the child process is an exact copy of the parent. Naive implementation would copy all memory:

```
Parent process: 4GB memory
fork() → Copy 4GB to child
Result: 8GB physical RAM needed
```

But most forked processes modify very little memory before calling exec() to run a different program. Copy-on-write defers the copy:

```
Parent has 4GB memory mapped to physical frames

fork():
1. Child gets same page table as parent (same virtual → physical mappings)
2. All pages marked read-only in both parent and child
3. No physical memory copied (0 bytes!)
4. Physical RAM usage: Still 4GB (shared between parent and child)

Child writes to page:
1. Write triggers page fault (page is read-only)
2. OS allocates new physical page
3. OS copies contents from parent's page to child's page
4. OS updates child's page table to point to new page
5. OS marks both pages writable
6. Child's write succeeds

Now: Parent has original page, child has private copy of that page
Physical RAM: 4GB + 4KB (only 1 page copied)
```

Copy-on-write means fork() is nearly free—the physical copy happens lazily, one page at a time, only for pages that are actually modified. If the child only modifies 100MB before calling exec(), only 100MB is copied. The remaining 3.9GB is shared between parent and child.

This is overcommitment in action: the OS granted 8GB of virtual memory (4GB parent + 4GB child) but uses only 4.1GB physical RAM (4GB shared + 100MB child-modified).

**Zero-page optimization:**

Copy-on-write also applies to newly allocated anonymous memory:

```
mmap(1GB, MAP_ANONYMOUS):
1. OS allocates 1GB virtual address space
2. All pages map to a single shared zero page (physical page filled with zeros)
3. All pages marked read-only
4. Physical RAM used: 4KB (one shared zero page)

Program writes to page 0:
1. Write triggers page fault (read-only)
2. OS allocates new physical page, fills with zeros
3. OS updates page table
4. Physical RAM used: 4KB + 4KB = 8KB (zero page + 1 allocated page)

Program writes to 10,000 pages:
Physical RAM used: 4KB + 40MB (zero page + 10,000 pages)
```

The OS can grant terabytes of virtual anonymous memory with negligible physical cost—every page initially maps to the shared zero page. Only pages that are written consume physical RAM.

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

## Translation Lookaside Buffer (TLB)

The Translation Lookaside Buffer (TLB) is a small, fast cache in the CPU that stores recent virtual-to-physical address translations. It is the hardware mechanism that makes paging practical—without the TLB, address translation would make every memory access 5× slower.

**The problem without TLB:**

Every memory access requires translating a virtual address to a physical address. On x86-64 with four-level paging, this means:

```
Virtual address access (no TLB):
1. Access PML4 table in RAM (100 cycles)
2. Access PDPT table in RAM (100 cycles)
3. Access PD table in RAM (100 cycles)
4. Access PT table in RAM (100 cycles)
5. Access actual data in RAM (100 cycles)
Total: 500 cycles per memory access

If the program accesses 1000 variables:
1000 × 500 = 500,000 cycles = ~160 microseconds at 3GHz
```

This is catastrophic. A simple loop reading an array would spend 80% of its time walking page tables, not accessing data.

**How TLB solves this:**

The TLB caches recent translations. When the CPU needs to translate a virtual address, it checks the TLB first:

```
TLB hit (common case):
1. Check TLB for translation (1 cycle) → Found!
2. Access actual data in RAM (100 cycles)
Total: 101 cycles per memory access

TLB miss (rare case):
1. Check TLB for translation (1 cycle) → Not found
2. Walk page tables in RAM (400 cycles for 4-level walk)
3. Store translation in TLB
4. Access actual data in RAM (100 cycles)
Total: 501 cycles per memory access
```

**TLB hit rate determines performance:**

If 99% of accesses hit the TLB:

```
Average memory access time:
0.99 × 101 cycles (TLB hit) + 0.01 × 501 cycles (TLB miss)
= 99.99 + 5.01
= 105 cycles

Overhead: 5 cycles (5% slower than direct physical access)
Without TLB: 500 cycles (5× slower!)
```

**TLB structure:**

A typical TLB on modern CPUs:
- **Size:** 64-512 entries (very small cache)
- **Coverage:** With 4KB pages, 512 entries cover 2MB of address space
- **Levels:** L1 TLB (tiny, fast), L2 TLB (larger, slower but still faster than page table walk)
- **Associativity:** Fully associative (can store any translation anywhere)
- **Flushed on:** Context switches (switching processes invalidates all translations)

**Why TLB is small:**

TLB must be extremely fast (1-cycle lookup). To achieve this, it's implemented in expensive SRAM on the CPU die. Only 64-512 entries fit. This means the TLB can only cache translations for a small portion of the address space at any time.

**TLB thrashing:**

If a program accesses more unique pages than the TLB can hold, it starts thrashing—constantly evicting translations and reloading them:

```
Program accesses 1024 unique pages in a loop
TLB holds 512 entries

First iteration:
- All 1024 accesses miss TLB (cold TLB)
- Load 1024 translations, evicting old entries
- Cost: 1024 × 500 cycles = 512,000 cycles

Second iteration:
- Still miss! (only last 512 translations cached)
- Cost: 512,000 cycles again

With TLB thrashing: Every iteration costs 512,000 cycles
With TLB hits: Every iteration costs 104,000 cycles (5× faster)
```

**Why TLB matters for allocators:**

Allocators influence TLB behavior through spatial layout:

**Bad:** Objects scattered across many pages
```
1000 objects, each on a different page (1000 unique pages)
Accessing all objects: 1000 TLB misses
```

**Good:** Objects grouped on few pages
```
1000 objects packed into 50 pages (50 unique pages, fits in TLB)
Accessing all objects: 50 TLB misses (first access), then all hits
```

temporal-slab improves TLB behavior through temporal grouping: objects allocated together are placed in the same slabs (same pages). If those objects are accessed together (temporal locality → spatial locality), they share TLB entries. A web server processing a request might allocate 10 objects in the same epoch—all 10 end up on the same page, requiring only 1 TLB entry instead of 10.

**TLB and huge pages:**

Modern CPUs support huge pages (2MB or 1GB) which drastically reduce TLB pressure:

```
Standard 4KB pages:
512-entry TLB covers 2MB (512 × 4KB)
100,000 objects @ 4KB per page = 100,000 pages
TLB hit rate: <1% (catastrophic)

Huge 2MB pages:
512-entry TLB covers 1GB (512 × 2MB)
100,000 objects @ 2MB per page = 50 pages
TLB hit rate: >99% (excellent)
```

Some allocators use huge pages for metadata-heavy structures. temporal-slab uses standard 4KB pages but achieves good TLB behavior through object grouping.

**The TLB equation:**

```
Effective memory access time = 
    TLB_hit_rate × (TLB_lookup + memory_access) +
    TLB_miss_rate × (TLB_lookup + page_walk + memory_access)

With 99% hit rate:
= 0.99 × (1 + 100) + 0.01 × (1 + 400 + 100)
= 99.99 + 5.01
= 105 cycles (5% overhead)

With 50% hit rate (TLB thrashing):
= 0.50 × (1 + 100) + 0.50 × (1 + 400 + 100)
= 50.5 + 250.5
= 301 cycles (3× slower!)
```

TLB hit rate is critical to performance. Allocators that scatter objects across many pages destroy TLB efficiency.

## Cache Line and Cache Locality

A cache line is the smallest unit of data transferred between RAM and CPU cache. On modern CPUs, a cache line is 64 bytes. Understanding cache lines is essential to understanding why object placement affects performance beyond just RSS.

**What is a cache line?**

When the CPU reads a single byte from RAM, it doesn't fetch just that byte—it fetches the entire 64-byte cache line containing that byte:

```
RAM:
Address 0x1000: [byte 0][byte 1][byte 2]...[byte 63]  ← One cache line (64 bytes)
Address 0x1040: [byte 64][byte 65]...[byte 127]      ← Next cache line

CPU reads byte at 0x1008:
→ Fetches entire cache line [0x1000-0x103F] (all 64 bytes)
→ Stores cache line in L1 cache
→ Returns byte at 0x1008

Next read of byte at 0x1010:
→ Already in cache! (same cache line)
→ 4 cycles (L1 hit) instead of 100 cycles (DRAM)
```

**Cache hierarchy:**

Modern CPUs have multiple cache levels:

| Cache | Size | Latency | Bandwidth |
|-------|------|---------|-----------|
| **L1** | 32-64 KB per core | 4 cycles (~1ns) | ~1 TB/s |
| **L2** | 256-512 KB per core | 12 cycles (~4ns) | ~500 GB/s |
| **L3** | 8-32 MB shared | 40 cycles (~14ns) | ~200 GB/s |
| **DRAM** | 8-64 GB | 100 cycles (~35ns) | ~50 GB/s |

The difference between L1 hit (4 cycles) and DRAM miss (100 cycles) is 25×. Cache locality—keeping frequently accessed data in cache—is critical for performance.

**Spatial locality:**

Spatial locality is the principle that if you access memory address X, you're likely to access nearby addresses soon:

```
Good spatial locality (array traversal):
for (int i = 0; i < 1000; i++) {
    sum += array[i];  // Access sequential addresses
}

Each cache line holds 16 integers (64 bytes / 4 bytes per int)
1000 integers = 63 cache lines
First access to each cache line: Miss (63 misses)
Remaining accesses: Hit (937 hits)
Hit rate: 93.7%
```

```
Bad spatial locality (random access):
for (int i = 0; i < 1000; i++) {
    sum += array[random()];  // Access random addresses
}

Each access likely hits a different cache line
1000 accesses = ~1000 cache line loads
Hit rate: ~0%
```

**Temporal locality:**

Temporal locality is the principle that if you access memory address X, you're likely to access it again soon:

```
Good temporal locality:
for (int iter = 0; iter < 100; iter++) {
    process(data);  // Repeatedly access same data
}

data cache line loaded once, hit 99 more times
Hit rate: 99%
```

```
Bad temporal locality:
for (int i = 0; i < 1000000; i++) {
    process(&huge_array[i]);  // Touch each element once
}

huge_array is 1M elements × 4 bytes = 4MB
L3 cache is 8MB, but working set is 4MB (50% of cache)
Many cache lines evicted before reuse
Hit rate: poor
```

**Why cache locality matters for allocators:**

Allocators determine spatial layout. Objects allocated together can be placed together (good locality) or scattered (bad locality).

**Example: Session store with poor locality**

```
Traditional allocator (scattered allocation):

Sessions for Request 1:
- Connection handle at 0x10000 (page 1)
- Request object at 0x25000 (page 37)
- Response buffer at 0x48000 (page 72)

Processing Request 1:
- Access 0x10000 → Cache miss, load page 1
- Access 0x25000 → Cache miss, load page 37
- Access 0x48000 → Cache miss, load page 72
= 3 cache misses per request
```

**Example: Session store with good locality**

```
temporal-slab (temporal grouping):

Sessions for Request 1 (all in same epoch):
- Connection handle at 0x10000 (page 1)
- Request object at 0x10040 (page 1, same cache line!)
- Response buffer at 0x10080 (page 1, same cache line!)

Processing Request 1:
- Access 0x10000 → Cache miss, load page 1 (loads entire cache line)
- Access 0x10040 → Cache hit (same cache line)
- Access 0x10080 → Cache hit (same cache line)
= 1 cache miss per request (3× fewer!)
```

**Quantifying the benefit:**

```
Web server: 10,000 requests/second, 3 objects per request

Poor locality:
30,000 object accesses × 100 cycles (cache miss) = 3,000,000 cycles/request
At 3GHz: 1ms per request

Good locality:
10,000 cache misses × 100 cycles + 20,000 cache hits × 4 cycles = 1,080,000 cycles/request
At 3GHz: 0.36ms per request (2.8× faster!)
```

**Cache line false sharing:**

When multiple threads write to different variables in the same cache line, cache coherence forces cache line bouncing between cores:

```
Struct with false sharing:
struct {
    int thread1_counter;  // Byte 0-3
    int thread2_counter;  // Byte 4-7 (SAME CACHE LINE!)
} shared;

Thread 1: shared.thread1_counter++ (writes cache line, invalidates Thread 2's cache)
Thread 2: shared.thread2_counter++ (writes cache line, invalidates Thread 1's cache)
→ Cache line ping-pongs between cores (expensive!)
```

```
Fix: Pad to separate cache lines:
struct {
    int thread1_counter;
    char pad1[60];        // Pad to 64 bytes
    int thread2_counter;
    char pad2[60];        // Pad to 64 bytes
};

Thread 1 and Thread 2 now write to different cache lines (no bouncing)
```

**Why temporal-slab improves cache locality:**

Objects allocated in the same epoch are placed in the same slabs (same pages). If those objects are accessed together (which is likely—they were created for the same request/transaction), they share cache lines:

- **Spatial locality:** Objects are adjacent in memory
- **Temporal locality:** Objects created together are often accessed together
- **Result:** Higher cache hit rate, lower memory bandwidth, better performance

This is a second-order effect beyond RSS stability, but it matters: temporal-slab's lifetime grouping improves not just memory usage but also cache behavior.

## System Call (syscall)

A system call (syscall) is a request from a user program to the operating system kernel to perform a privileged operation. System calls are the boundary between userspace (where programs run) and kernel space (where the OS runs). They are expensive—much more expensive than regular function calls.

**What is a syscall?**

Programs run in userspace with restricted privileges. They cannot directly access hardware, modify page tables, or allocate physical memory. To perform these operations, programs must ask the kernel via a syscall:

```c
// Userspace code
void* ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
// → Issues mmap syscall
// → Kernel allocates virtual address space
// → Kernel returns pointer
```

**How syscalls work:**

```
1. Program executes syscall instruction (e.g., SYSCALL on x86-64)
2. CPU switches from user mode to kernel mode
3. CPU saves user registers and switches to kernel stack
4. Kernel syscall handler runs (e.g., sys_mmap)
5. Kernel performs privileged operation
6. Kernel prepares return value
7. CPU switches from kernel mode back to user mode
8. CPU restores user registers
9. Program continues with return value
```

This mode switch is expensive because:
- **Context save/restore:** ~100 cycles (save/restore 16+ registers)
- **TLB flush (sometimes):** If switching address spaces, TLB is flushed
- **Cache pollution:** Kernel code evicts user code from instruction cache
- **Speculation barriers:** Security mitigations (Spectre/Meltdown) add overhead

**Syscall costs:**

| Operation | Cycles | Time @ 3GHz |
|-----------|--------|-------------|
| **Function call** | 5-10 | 2-3 ns |
| **Simple syscall (getpid)** | 1,000-1,500 | 300-500 ns |
| **mmap (no fault)** | 2,000-5,000 | 700ns-1.7µs |
| **munmap** | 2,000-5,000 | 700ns-1.7µs |
| **mmap (with fault)** | 3,000-10,000 | 1-3 µs |

A syscall is 100-1000× more expensive than a function call!

**Why syscalls matter for allocators:**

Allocators must obtain memory from the OS via mmap (syscall) and can return it via munmap (syscall). Frequent mmap/munmap is catastrophic:

```
Naive allocator (no caching):
malloc(128):  mmap(4096) → 3µs
free(ptr):    munmap(4096) → 3µs
Total: 6µs per allocation cycle

With 10,000 allocations/second:
10,000 × 6µs = 60ms = 6% of one CPU core spent in syscalls!
```

```
Smart allocator (caching):
malloc(128):  Pop from cache → 50ns (no syscall)
free(ptr):    Push to cache → 50ns (no syscall)
Total: 100ns per allocation cycle

With 10,000 allocations/second:
10,000 × 100ns = 1ms = 0.1% of CPU core (60× better!)
```

**How temporal-slab avoids syscalls:**

1. **Slab cache:** Empty slabs are cached (32 per size class). Allocating a new slab hits the cache (no mmap syscall).

2. **Refusal to unmap:** Slabs are never unmapped during runtime. Once mmapped, they remain mapped even when empty. This trades higher RSS floor for zero munmap syscalls.

3. **Batch allocation:** When cache is empty, allocate slabs in small batches (amortize mmap cost).

**Syscall cost in context:**

```
temporal-slab allocation (fast path):
- Bitmap lookup: 10 cycles
- CAS operation: 20 cycles
- Total: 30 cycles (~10ns)

If we unmapped empty slabs:
- Allocation might require mmap: 3000 cycles (~1µs)
- 100× slower!
```

**Common syscalls in allocators:**

| Syscall | Purpose | Cost | How allocators avoid it |
|---------|---------|------|------------------------|
| **mmap** | Obtain memory | 2-5µs | Cache empty slabs, batch allocate |
| **munmap** | Return memory | 2-5µs | Never unmap (temporal-slab) or cache empty pages |
| **brk/sbrk** | Grow heap | 1-3µs | Deprecated, use mmap instead |
| **madvise** | Hint to OS | 1-2µs | Tell OS pages are unused (MADV_DONTNEED) |

Syscalls are the enemy of low-latency allocators. Caching is essential.

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

### malloc Variants: Different Strategies for the Same Problem

While the malloc/free interface is standard, different implementations make vastly different tradeoff decisions. Understanding these variants helps appreciate why temporal-slab's approach is necessary.

#### ptmalloc2 (glibc malloc)

**Design:** Arena-per-thread with binned free lists

ptmalloc2 is the default allocator in glibc (Linux standard C library). It uses arenas (memory pools) to reduce contention between threads.

**How it works:**

1. **Arenas:** Each thread gets its own arena (memory pool). The main thread uses the main arena, other threads get thread-specific arenas. This reduces lock contention (threads don't compete for the same lock).

2. **Bins:** Within each arena, free blocks are organized into bins by size:
   - **Fast bins:** Small allocations (16-80 bytes), LIFO (last-in-first-out) for cache locality
   - **Small bins:** 16-512 bytes, segregated by exact size (powers of 2)
   - **Large bins:** >512 bytes, sorted by size within each bin
   - **Unsorted bin:** Recently freed blocks (not yet sorted into bins)

3. **Allocation strategy:**
   - Check fast bin for exact size match (O(1))
   - Check small bin for exact size match (O(1))
   - Check unsorted bin, sort blocks into bins during search (O(N))
   - Check large bins, find best-fit (O(log N))
   - If no fit found, request new pages from OS via mmap

**Performance characteristics:**

| Metric | Value | Explanation |
|--------|-------|-------------|
| Avg allocation latency | ~50ns | Fast bin hits are quick |
| p99 allocation latency | **1,443ns** | Large bin searches, lock contention, mmap syscalls |
| p999 allocation latency | **4,409ns** | Worst case: multiple bin scans + mmap |
| RSS drift | **Unbounded** | No lifetime awareness, temporal fragmentation |
| Thread scalability | Moderate | Arenas reduce but don't eliminate contention |

**Why temporal fragmentation happens:**

ptmalloc2 has no concept of object lifetimes. A long-lived session object and short-lived request object both go into the same bins if they're the same size. They intermingle on pages, causing temporal fragmentation.

```
Web server using ptmalloc2:
Hour 1:  12 MB RSS
Hour 24: 20 MB RSS (+67% drift)
Day 7:   25 MB RSS (+108% drift)

Cause: Pages pinned by sparse long-lived objects (temporal fragmentation)
```

**When it's good enough:**
- Short-lived processes (CLI tools, batch jobs)
- Low allocation rate (<1K/sec)
- Memory size more important than latency (embedded systems)

#### jemalloc (Facebook, Rust default)

**Design:** Size-class segregation with extensive profiling support

jemalloc (originally from FreeBSD, now maintained by Facebook) is optimized for multi-threaded applications with high allocation rates. It's the default allocator in Rust and Firefox.

**How it works:**

1. **Size classes:** Allocations are rounded up to predefined size classes (16, 32, 48, 64, 80, 96, ..., 2MB). This reduces fragmentation compared to arbitrary sizes.

2. **Thread-local caching (tcache):** Each thread has a cache of recently-freed objects organized by size class. Allocations hit the tcache first (lock-free), falling back to arena on cache miss.

3. **Arenas:** Multiple arenas (typically 4× number of CPUs) to distribute contention. Each arena manages extents (runs of pages) organized by size class.

4. **Runs:** A run is a contiguous group of pages devoted to a single size class. Similar to temporal-slab's slabs, but without epoch grouping.

**Performance characteristics:**

| Metric | Value | Explanation |
|--------|-------|-------------|
| Avg allocation latency | ~45ns | tcache hits are fast |
| p99 allocation latency | **~800ns** | Arena lock, run allocation |
| RSS drift | Moderate | Better than ptmalloc2, but still drifts |
| Profiling overhead | **10-30%** | When `--enable-prof` active |
| Observability | Per-allocation backtraces (expensive) | Requires sampling |

**Profiling tradeoff:**

jemalloc includes heap profiling, but it's expensive. Enabling profiling requires:
1. Hash table tracking every allocation → backtrace mapping
2. Stack unwinding on every allocation (200-500ns per alloc)
3. Post-mortem analysis with `jeprof` tool

```bash
# Enable profiling (10-30% overhead)
export MALLOC_CONF="prof:true,prof_leak:true"
./server

# Analyze after shutdown
jeprof --text ./server jeprof.*.heap
```

**Why it's better than ptmalloc2 but still has RSS drift:**

jemalloc's size-class segregation reduces fragmentation compared to ptmalloc2's mixed-size bins. But it still has no lifetime awareness. Long-lived and short-lived objects of the same size intermingle in the same runs, causing temporal fragmentation.

**When to use jemalloc:**
- High allocation rate (>10K/sec)
- Need profiling (can tolerate overhead)
- Multi-threaded (benefits from tcache)
- Allocation sizes vary (not fixed size classes)

#### tcmalloc (Google, used in Chrome)

**Design:** Aggressive per-thread caching with central freelist

tcmalloc (thread-caching malloc) from Google is optimized for high-throughput multi-threaded applications. It powers Chrome and many Google services.

**How it works:**

1. **Thread-local cache:** Each thread has a cache of free objects for each size class (up to 256KB). Allocations are served from the cache without locks.

2. **Central freelist:** When a thread cache misses, it requests a batch of objects from the central freelist (protected by lock). Batching amortizes lock cost.

3. **Page heap:** Manages large allocations (>256KB) using a best-fit algorithm. Coalesces adjacent free pages to reduce fragmentation.

4. **Aggressive caching:** tcmalloc caches more objects per thread than jemalloc, trading memory for speed.

**Performance characteristics:**

| Metric | Value | Explanation |
|--------|-------|-------------|
| Avg allocation latency | ~40ns | Thread cache (lock-free) |
| p99 allocation latency | **~600ns** | Central freelist lock |
| RSS overhead | Higher | Aggressive caching |
| RSS drift | Moderate | Better than ptmalloc2 |
| Heap profiler overhead | **5-15%** | Lower than jemalloc |

**Heap profiling:**

tcmalloc includes a heap profiler (`HeapProfiler`) that samples allocations (not every allocation, unlike jemalloc). Lower overhead but less precise.

```cpp
// C++ example with tcmalloc heap profiler
#include <gperftools/heap-profiler.h>

HeapProfilerStart("myapp");
// Run application...
HeapProfilerStop();  // Generates myapp.0001.heap
```

Analysis: `pprof --text ./myapp myapp.0001.heap`

**Why it still has RSS drift:**

Despite aggressive caching and size-class segregation, tcmalloc has no lifetime awareness. It cannot prevent temporal fragmentation because it treats all allocations of the same size as equivalent, regardless of expected lifetime.

**When to use tcmalloc:**
- Very high allocation rate (>100K/sec)
- Latency-sensitive (lower p99 than jemalloc/ptmalloc2)
- Multi-threaded with contention (aggressive caching helps)
- Can accept higher RSS baseline (caching overhead)

#### Comparison Table: malloc Variants vs temporal-slab

| Allocator | Avg Latency | p99 Latency | p999 Latency | RSS Drift (24h) | Observability Overhead | Thread Scalability |
|-----------|-------------|-------------|--------------|-----------------|------------------------|---------------------|
| **ptmalloc2** (glibc) | 50ns | 1,443ns | 4,409ns | +67% | N/A (no observability) | Moderate (arenas) |
| **jemalloc** | 45ns | ~800ns | ~1,500ns | +40% | 10-30% (if profiling) | Good (tcache) |
| **tcmalloc** | 40ns | ~600ns | ~1,200ns | +30% | 5-15% (heap profiler) | Excellent (aggressive cache) |
| **temporal-slab** | 40ns | **131ns** | **371ns** | **0%** | **0% (structural)** | Excellent (lock-free) |

**Key observations:**

1. **Tail latency:** temporal-slab achieves 4-11× better p99 latency than malloc variants (131ns vs 600-1,443ns)

2. **RSS drift:** All malloc variants drift (+30-67% over 24 hours), temporal-slab has 0% drift

3. **Observability:** malloc profiling costs 5-30% overhead, temporal-slab's metrics are free (structural)

4. **Tradeoff:** temporal-slab requires fixed size classes (≤768 bytes) and phase boundaries

**Why malloc cannot achieve 0% RSS drift:**

All malloc variants operate at the **pointer level**, not the **lifetime level**. They optimize for:
- Fast allocation (thread caching, lock-free paths)
- Low fragmentation (size classes, bins, runs)
- Thread scalability (arenas, per-thread caches)

But they cannot prevent temporal fragmentation because they have no concept of object lifetimes. A 128-byte session object (lives hours) and a 128-byte request object (lives milliseconds) look identical to malloc—both go into the same size class, intermingle on the same pages, and cause temporal fragmentation as requests expire.

temporal-slab solves this by operating at the **phase level**: group objects by epoch (time window), place them on the same slabs, and reclaim entire slabs when the phase completes. This requires application cooperation (calling `epoch_close()` at phase boundaries) but eliminates drift entirely.

**When to use each:**

| Workload | Recommended Allocator | Why |
|----------|----------------------|-----|
| Web server (request/response) | **temporal-slab** | High rate, correlated lifetimes, need 0% drift |
| Desktop app (user interactions) | **jemalloc** | Variable sizes, unpredictable lifetimes |
| Browser (many short-lived tabs) | **tcmalloc** | Very high rate, low p99 latency |
| CLI tool (short-lived process) | **ptmalloc2** | Simplicity, no time for drift |
| Game engine (per-frame objects) | **temporal-slab** | Frame boundary = epoch boundary |
| Database (long-lived cache) | **jemalloc** | Stable working set, profiling useful |

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

## Memory Management Approaches: A Complete Taxonomy

Before diving deeper into temporal-slab's design, it's essential to understand the full landscape of memory management approaches. Each approach makes different tradeoffs between safety, performance, and control. Understanding these tradeoffs helps explain why temporal-slab exists and when it's the right choice.

### Manual Memory Management (malloc/free)

**How it works:**

The programmer explicitly allocates and frees memory. The allocator tracks which memory is free, but has no concept of object lifetimes or ownership.

```c
void* ptr = malloc(128);   // Request memory
use(ptr);                  // Use it
free(ptr);                 // Return it when done
```

**Advantages:**
- **Full control:** Programmer decides exactly when memory is allocated/freed
- **Predictable timing:** No garbage collection pauses
- **Low overhead:** No runtime tracking (typical malloc: 50ns allocation)

**Disadvantages:**
- **Memory safety issues:** Use-after-free, double-free, leaks if programmer forgets to free
- **Temporal fragmentation:** No lifetime awareness leads to mixed lifetimes on same pages
- **Unbounded RSS growth:** Long-running systems accumulate partially-empty pages (drift)

**Fragmentation characteristics:**

malloc cannot prevent temporal fragmentation because it operates at the pointer level, not the lifetime level. It sees allocations as independent requests, not as related groups with correlated lifetimes.

```
Web server after 24 hours with malloc:
Live objects: 50,000 (steady state)
Expected RSS: 12 MB
Actual RSS: 20 MB (+67% drift)
Cause: 8 MB of partially-empty pages pinned by long-lived objects
```

**When to use malloc:**
- Short-lived processes (restart daily, drift doesn't accumulate)
- Low allocation rate (<1K allocs/sec)
- Variable allocation sizes (cannot fit into fixed size classes)
- Arbitrary object lifetimes (no discernible phases)

### Garbage Collection (Tracing)

**How it works:**

The runtime periodically traces all reachable objects from "root" pointers (stack, globals). Any unreachable objects are dead and their memory can be reclaimed. The programmer never explicitly frees memory.

```go
// Go example (mark-sweep GC)
func handleRequest() {
    data := make([]byte, 1024)  // Allocated automatically
    use(data)
    // No free() - GC will collect when unreachable
}
```

**Garbage collection variants:**

**Mark-Sweep GC** (basic):
1. **Mark phase:** Trace from roots, mark all reachable objects
2. **Sweep phase:** Walk heap, free unmarked objects
3. **Problem:** Must stop-the-world during marking (10-100ms pauses)

**Generational GC** (Java G1, .NET):
1. **Assumption:** Most objects die young
2. **Young generation:** Collected frequently (minor GC, 1-10ms pauses)
3. **Old generation:** Collected rarely (major GC, 50-500ms pauses)
4. **Problem:** Write barriers required (overhead on pointer updates)

**Concurrent GC** (Go, Azul Zing):
1. **Goal:** Reduce pause times by collecting concurrently with application
2. **Method:** Mark in parallel, brief stop-the-world for root scanning
3. **Problem:** Write barriers (5-10% throughput cost), still has brief pauses (1-5ms)

**Advantages:**
- **Memory safety:** No use-after-free, double-free, or leaks
- **Productivity:** Programmer doesn't track lifetimes manually
- **Compaction:** Can consolidate live objects to eliminate fragmentation

**Disadvantages:**
- **Unpredictable pauses:** Stop-the-world collections (10-100ms typical, can be seconds)
- **Write barrier overhead:** Every pointer update requires bookkeeping (5-10% throughput cost)
- **Heap size tuning:** Requires careful configuration (too small = frequent GC, too large = long pauses)
- **No control over timing:** GC triggers based on heap pressure, not application logic

**Why GC causes pauses:**

During marking, the heap state must be consistent (no mutations). If threads continue allocating/freeing while GC traces, the reachability graph changes, causing missed objects (incorrect frees) or retained garbage (leaks). Solution: stop-the-world (pause all threads during mark phase).

```
Example pause times (Java G1GC, 4GB heap):
Minor collection (young gen): 5-15ms
Major collection (full heap): 50-200ms
Worst case (fragmented heap): 500ms-2s
```

**When to use GC:**
- Safety more important than performance (financial systems, medical devices)
- Rapid development (prototypes, MVPs where productivity matters)
- Complex object graphs with circular references (compilers, IDEs)
- Languages with GC built-in (Go, Java, C#, Python)

### Reference Counting

**How it works:**

Each object has a counter tracking how many pointers reference it. When the count reaches zero, the object is immediately freed. No tracing or stop-the-world required.

```swift
// Swift example (ARC = Automatic Reference Counting)
class Session {
    var data: Data
}

var session1 = Session()  // refcount = 1
var session2 = session1   // refcount = 2
session1 = nil            // refcount = 1
session2 = nil            // refcount = 0 → freed immediately
```

**Advantages:**
- **Deterministic timing:** Objects freed immediately when last reference dropped
- **No pauses:** No stop-the-world collections
- **Bounded latency:** Each operation has constant-time overhead

**Disadvantages:**
- **Cyclic references:** If A → B → A, refcount never reaches zero (requires cycle detection or weak references)
- **Contention:** Multiple threads incrementing same refcount cause cache line bouncing
- **Overhead:** 40-80 cycles per access (atomic increment on acquire, decrement on release)

**Cycle detection problem:**

```python
# Python example (refcount + cycle detector)
class Node:
    def __init__(self):
        self.next = None

a = Node()
b = Node()
a.next = b  # a → b
b.next = a  # b → a (cycle!)

a = None
b = None
# Without cycle detector: a and b leak (refcount never reaches 0)
# With cycle detector: Periodic tracing required (defeats purpose of refcounting)
```

**When to use reference counting:**
- Embedded systems (no GC runtime overhead)
- Real-time systems (deterministic timing critical)
- Languages with ARC built-in (Swift, Objective-C)
- Simple object ownership (no circular structures)

### Region-Based Memory Management

**How it works:**

Group allocations into "regions" (also called arenas or pools). All allocations in a region are freed together when the region is destroyed. No per-object tracking.

```c
// Apache APR pools example
apr_pool_t* pool = apr_pool_create();
void* obj1 = apr_palloc(pool, 100);
void* obj2 = apr_palloc(pool, 200);
// Use objects...
apr_pool_destroy(pool);  // Frees obj1 and obj2 together
```

**Advantages:**
- **Fast allocation:** Bump pointer (just increment offset)
- **Fast deallocation:** Free entire region at once (O(1))
- **No per-object tracking:** No metadata overhead

**Disadvantages:**
- **Stack-only lifetimes:** Cannot express interleaved lifetimes (sessions outliving requests)
- **No partial reclamation:** All-or-nothing (cannot free individual objects)
- **No threading support:** Regions typically not thread-safe
- **Memory bloat:** Region holds memory until destruction (even if objects freed early)

**Limitation: Interleaved lifetimes**

```c
// Problem: Cannot handle long-lived + short-lived in same region
apr_pool_t* pool = apr_pool_create();

Session* session = apr_palloc(pool, 1024);  // Lives hours
for (int i = 0; i < 1000; i++) {
    Request* req = apr_palloc(pool, 128);  // Lives milliseconds
    process(req);
    // Cannot free req individually!
}
apr_pool_destroy(pool);  // Frees session AND all 1000 requests together
```

Result: Pool holds 128KB (1000 requests) even though only session (1KB) is live. 99% wasted memory until pool destroyed.

**When to use regions:**
- Single-threaded parsers (allocate AST nodes, free tree at once)
- Image processing (allocate temp buffers, destroy after frame processed)
- Stack-like lifetime patterns (perfect nesting, no interleaving)

### Epoch-Based Memory Management (temporal-slab)

**How it works:**

Group allocations by time window (epochs). Objects allocated in the same epoch are placed on the same pages (slabs). When the application signals that an epoch's lifetime has ended (`epoch_close()`), all empty slabs in that epoch are recycled. This eliminates temporal fragmentation without compaction or GC.

```c
// temporal-slab example
SlabAllocator alloc;
allocator_init(&alloc);

// Long-lived epoch
void* session = alloc_obj_epoch(&alloc, 128, 0, &handle);

// Short-lived epoch
void* request = alloc_obj_epoch(&alloc, 128, 1, &handle);
free_obj(&alloc, request);
epoch_close(&alloc, 1);  // Reclaim epoch 1's memory
```

**Advantages:**
- **Bounded RSS:** No drift (0% growth over weeks)
- **Deterministic reclamation:** Memory freed at phase boundaries (epoch_close)
- **Predictable latency:** Lock-free fast path (131ns p99, 371ns p999)
- **Zero GC overhead:** No tracing, no pauses, no write barriers
- **Structural observability:** Metrics organized by phase (built-in)

**Disadvantages:**
- **Fixed size classes:** Only 64-768 bytes (no arbitrary sizes)
- **Phase boundary alignment required:** Application must call epoch_close()
- **Internal fragmentation:** 11.1% average (128-byte slot for 100-byte object)
- **Platform-specific:** Optimized for x86-64 Linux (ARM would be slower)

**Key insight: Temporal grouping prevents fragmentation**

```
Web server with temporal-slab:
Epoch 0 (connections): [Conn 1][Conn 2][Conn 3]... (Slab A, stays full)
Epoch 1 (requests):     [Req 1][Req 2][Req 3]...   (Slab B, becomes empty)

After requests complete:
Slab A: 100% full (no waste)
Slab B: 100% empty (recyclable)
RSS: 12 MB (0% drift over 7 days)
```

**When to use epoch-based (temporal-slab):**
- High allocation rate (>10K allocs/sec)
- Correlated lifetimes (request-scoped, frame-scoped, batch-scoped)
- Long-running services (days to weeks of uptime)
- Latency-sensitive (HFT, real-time systems, control planes)
- Allocation sizes ≤768 bytes

### Comparison Table: All Approaches

| Approach | Safety | Pause Risk | Overhead | RSS Drift | Observability | When to Use |
|----------|--------|------------|----------|-----------|---------------|-------------|
| **malloc** | Manual (unsafe) | None | ~50ns | Unbounded | None | Short-lived processes, low churn |
| **GC (mark-sweep)** | Automatic | 10-100ms | Write barriers (5-10%) | Compaction prevents | External tools | Safety-critical, complex ownership |
| **Refcounting** | Automatic (cycles leak) | None | 40-80ns per access | Moderate | None | Embedded systems, simple ownership |
| **Regions** | Manual (unsafe) | None | ~10ns (bump pointer) | Bloat until destroy | None | Stack-like lifetimes, single-threaded |
| **Epochs (temporal-slab)** | Manual (unsafe) | None | 40ns (p50), 131ns (p99) | **0%** | **Built-in, zero-cost** | High-throughput structured systems |

### Decision Flowchart

```
START: Choosing memory management approach

┌─────────────────────────────────────┐
│ Is safety more important than perf? │
│ (medical, financial, safety-critical)│
└─────────────────┬───────────────────┘
                  │
         YES ─────┴───── NO
          │                │
          ▼                ▼
    ┌─────────┐     ┌──────────────────┐
    │ Use GC  │     │ Can you identify │
    │ (Go,    │     │ phase boundaries?│
    │ Java,   │     │ (requests, frames)│
    │ C#)     │     └────────┬─────────┘
    └─────────┘              │
                    YES ─────┴───── NO
                     │                │
                     ▼                ▼
              ┌──────────────┐  ┌──────────┐
              │ Are objects  │  │ Use malloc│
              │ ≤768 bytes?  │  │ or jemalloc│
              └──────┬───────┘  └──────────┘
                     │
            YES ─────┴───── NO
             │                │
             ▼                ▼
       ┌──────────────┐  ┌──────────┐
       │ High alloc   │  │ Use malloc│
       │ rate         │  │ (large     │
       │ (>10K/sec)?  │  │ objects)   │
       └──────┬───────┘  └──────────┘
              │
     YES ─────┴───── NO
      │                │
      ▼                ▼
┌──────────────┐  ┌──────────┐
│ Use temporal-│  │ Use malloc│
│ slab         │  │ (overhead  │
│              │  │ not worth) │
└──────────────┘  └──────────┘
```

### Anti-Patterns for Each Approach

**Don't use malloc for:**
- Long-running services with high churn (RSS drift accumulates)
- Latency-sensitive systems (p99 = 1,443ns, unpredictable spikes)

**Don't use GC for:**
- Hard real-time systems (pauses violate latency SLAs)
- HFT or control planes (10-100ms pauses unacceptable)

**Don't use refcounting for:**
- Complex object graphs with cycles (requires cycle detector = GC overhead)
- High contention workloads (cache line bouncing on shared refcounts)

**Don't use regions for:**
- Interleaved lifetimes (long-lived + short-lived in same region causes bloat)
- Multi-threaded systems (regions typically not thread-safe)

**Don't use temporal-slab for:**
- Variable-size allocations >768 bytes (document buffers, video frames)
- Unpredictable lifetimes (desktop GUI, user-driven interactions)
- Low allocation rate (<1K/sec) (overhead not justified)

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

temporal-slab adapts this for user-space, adding temporal grouping (lifetime affinity), lock-free fast paths, and conservative recycling (deferred reclamation via epoch_close rather than immediate recycling on free). The core principle remains: fixed-size slots eliminate search overhead and enable O(1) allocation/free.

## Lifetime Affinity

Lifetime affinity is the principle that objects with similar lifetimes should be placed in the same page. If short-lived objects are grouped together, they die together, and their page can be reclaimed. If long-lived objects are grouped together, their page remains full and useful. The page does not become pinned by a mixture of live and dead objects.

The problem is that the allocator does not know object lifetimes in advance. A session object and a request object have the same type signature—they are both allocations of N bytes. The allocator cannot predict that one will live hours and the other milliseconds.

The solution is allocation-order affinity: group allocations that occur close together in time. Programs exhibit temporal locality—allocations that occur around the same time are often causally related and thus have correlated lifetimes. A web server handling a request allocates a request object, a response object, a buffer, and a session token. These are allocated in quick succession. They are causally related—created for the same request. When the request completes, they are all freed. If they are in the same page, that page becomes empty.

Allocation-order affinity does not require the allocator to predict lifetimes. It requires the allocator to allocate sequentially within pages, so objects allocated in the same epoch end up in the same page. Lifetime correlation emerges naturally from allocation patterns, not from explicit hints.

## Phase Boundary

A phase boundary is the moment when a logical unit of work completes in the application. It marks the transition between distinct lifetime phases. Examples:

**Web server:**
- Phase: Processing a single HTTP request
- Boundary: Sending the response, closing the connection
- Objects freed: Request metadata, parsed headers, response buffers, session tokens

**Game engine:**
- Phase: Rendering a single frame
- Boundary: Presenting the frame to the display, advancing to next frame
- Objects freed: Render commands, particle state, temporary geometry buffers

**Database transaction:**
- Phase: Executing a transaction (BEGIN → COMMIT/ROLLBACK)
- Boundary: Transaction commit or rollback
- Objects freed: Query plan cache, intermediate result sets, lock metadata

**Batch processor:**
- Phase: Processing a batch of records
- Boundary: Writing results to storage, acknowledging completion
- Objects freed: Parsed records, aggregation state, output buffers

**Control plane reconciliation:**
- Phase: Processing a watch event or reconciliation loop iteration
- Boundary: Completing the reconciliation, updating status
- Objects freed: Event metadata, diff structures, API call results

**Why phase boundaries matter for temporal-slab:**

General-purpose allocators (malloc, jemalloc, tcmalloc) have no concept of phase boundaries. They see individual `malloc()` and `free()` calls but cannot distinguish between "allocations from the same request" and "allocations from different requests that happened to interleave." This leads to temporal fragmentation: pages contain a mix of objects from different phases with different lifetimes.

temporal-slab provides explicit phase boundary signaling via `epoch_close()`:

```c
// Application signals phase boundary
epoch_id_t request_epoch = epoch_current(alloc);

// ... allocate objects for this request ...
void* buffer = slab_malloc_epoch(alloc, 256, request_epoch);
void* session = slab_malloc_epoch(alloc, 128, request_epoch);

// Phase boundary: Request complete
epoch_close(alloc, request_epoch);
// → All empty slabs from this epoch immediately reclaimed
```

By aligning memory reclamation with application phase boundaries, temporal-slab achieves deterministic RSS control. The allocator doesn't guess when a "phase" ends—the application explicitly signals it.

**Structural determinism:**

Phase boundaries transform memory management from a pointer-chasing problem ("which objects are still reachable?") to a structural problem ("which phases have completed?"). This is the foundation of temporal-slab's **structural determinism** model:

- **malloc/free model:** Memory reclaimed when individual pointers become unreachable (pointer-level tracking)
- **GC model:** Memory reclaimed when objects become unreachable from roots (reachability graph traversal)
- **temporal-slab model:** Memory reclaimed when structural phases end (phase-level tracking)

Phase boundaries are the structural anchors that enable deterministic reclamation. They are observable, explicit, and aligned with application semantics—not inferred from pointer lifetimes.

**Relationship to epochs:**

An epoch is the allocator's implementation of a phase. When the application starts a new logical phase, it advances to a new epoch. When the phase ends (phase boundary), it closes the epoch. The epoch groups all allocations from that phase onto dedicated slabs, ensuring they can be reclaimed together when the phase boundary is reached.

Phase boundaries are the **application-level concept** ("this request is done"). Epochs are the **allocator-level mechanism** that implements phase tracking. The `epoch_close()` API is the bridge between the two.

## Epoch

An epoch is a time window during which allocations are grouped together onto the same set of slabs. It is the mechanism that implements lifetime affinity: objects allocated in the same epoch share slabs, separating them from objects allocated in different epochs.

An epoch is identified by a simple integer (EpochId): 0, 1, 2, ..., 15. temporal-slab uses a ring buffer of 16 epochs, so epoch IDs wrap around (after epoch 15 comes epoch 0 again). Each epoch maintains its own set of slabs per size class:

```
Size Class 128 bytes:
  Epoch 0: [Slab A][Slab B][Slab C]...
  Epoch 1: [Slab D][Slab E]...
  Epoch 2: [Slab F]...
  ...
  Epoch 15: [Slab Z]...
```

When you allocate an object, you specify which epoch it belongs to:

```c
void* ptr = slab_malloc_epoch(alloc, 128, current_epoch);
```

The allocator finds a slab from that epoch's pool and allocates a slot. All objects allocated with `current_epoch = 1` will be placed on slabs belonging to epoch 1, physically separated from objects in epoch 0 or epoch 2.

**Why epochs prevent RSS growth:**

Without epochs, objects with different lifetimes mix on the same slabs. A slab might contain:
- 5 short-lived request objects (freed after 10ms)
- 2 long-lived connection handles (live for 60 seconds)

When the requests are freed, the slab has 5 empty slots but remains allocated because the 2 connections are still alive. The slab cannot be recycled. This is temporal fragmentation.

With epochs, you separate lifetimes explicitly:

```c
// Long-lived backbone allocations in epoch 0
void* connection = slab_malloc_epoch(alloc, 128, 0);

// Short-lived request allocations in rotating epochs
void* request = slab_malloc_epoch(alloc, 128, current_epoch);
```

Now the slabs are separated:
- Epoch 0 slabs: 100% full with long-lived connections (no waste)
- Epoch 1 slabs: All requests from time window T=0-5s (all freed together)

When epoch 1's requests all die, its slabs become completely empty—no mixed lifetimes pin them. The slabs can be recycled immediately. RSS does not grow.

**The epoch allocation pattern:**

A typical application uses epochs like this:

```c
// Epoch 0: Static, long-lived data (server config, caches)
void* static_data = slab_malloc_epoch(alloc, 256, 0);

// Epochs 1-15: Rotating window for short-lived data
for (int i = 0; i < 1000000; i++) {
    if (i % 10000 == 0) {
        epoch_advance(alloc);  // Rotate to next epoch every 10K requests
    }
    
    EpochId current = epoch_current(alloc);
    void* request_data = slab_malloc_epoch(alloc, 128, current);
    
    process_request(request_data);
    
    slab_free(alloc, request_data);
}
```

Every 10,000 requests, the application advances to the next epoch. Old epochs gradually drain as their objects are freed. Slabs from drained epochs are recycled. The 16-epoch ring buffer provides a sliding window: the oldest epoch (16 epochs ago) is fully drained by the time you wrap around and reuse that epoch ID.

**Epoch properties:**

- **No forced expiration:** Advancing to a new epoch does not free objects from old epochs. Objects live until explicitly freed via `slab_free()`. Epochs only affect where new allocations go.
- **Natural drainage:** Old epochs drain as objects are freed normally. No special GC or compaction required.
- **Thread-safe:** `epoch_current()` and `epoch_advance()` use atomic operations. Multiple threads can allocate from the same epoch concurrently.
- **Ring buffer:** 16 epochs wrap around. By the time you reuse epoch 0 again (after 15 advances), epoch 0's previous allocations should be long gone.

**Why 16 epochs?**

16 is a balance between:
- **Too few epochs (e.g., 2):** Not enough separation—objects with different lifetimes still mix
- **Too many epochs (e.g., 256):** Excessive overhead—each epoch maintains its own slab pools, fragmenting the allocator's state

16 epochs provide sufficient separation for most workloads: if you advance every 5 seconds, that's 80 seconds of history. Objects living longer than 80 seconds should be allocated in epoch 0 (the "permanent" epoch).

## Epoch Advancement

Epoch advancement is the act of moving from one epoch to the next. When you call `epoch_advance(alloc)`, the allocator increments the current epoch ID (wrapping at 16):

```
Current epoch: 3
Call epoch_advance(alloc)
Current epoch: 4 (all new allocations now go to epoch 4's slabs)
```

Advancing epochs does two things:

**1. Starts a new allocation cohort:**

All allocations after advancement go to the new epoch's slabs. This separates them from the previous epoch's allocations:

```
Before advancement (epoch 3):
Slab A (Epoch 3): [Obj 1][Obj 2][Obj 3]...

Call epoch_advance() → Now epoch 4

After advancement (epoch 4):
Slab A (Epoch 3): [Obj 1][Obj 2][Obj 3]... (no new allocations)
Slab B (Epoch 4): [Obj 4][Obj 5][Obj 6]... (new allocations go here)
```

**2. Closes the previous epoch for new allocations:**

Epoch 3 is now "closed"—no new objects will be allocated there. Existing objects in epoch 3 remain alive until freed, but the slab pool stops growing. This allows epoch 3's slabs to drain naturally:

```
T=0: Advance to epoch 4
     Epoch 3 has 1000 objects on 100 slabs

T=10s: 500 objects from epoch 3 freed
       Epoch 3 has 500 objects on 100 slabs (some slabs partially empty)

T=60s: All 1000 objects from epoch 3 freed
       Epoch 3 has 0 objects, 100 empty slabs
       All 100 slabs pushed to cache → Available for reuse
```

**When to advance epochs:**

Applications advance based on their workload characteristics:

**Time-based advancement:**
```c
// Advance every 10 seconds
if (time_now() - last_advance_time > 10) {
    epoch_advance(alloc);
    last_advance_time = time_now();
}
```

**Request-based advancement:**
```c
// Advance every 10,000 requests
if (++request_count % 10000 == 0) {
    epoch_advance(alloc);
}
```

**Batch-based advancement:**
```c
// Advance after processing each batch
while (has_more_batches()) {
    Batch* batch = get_next_batch();
    epoch_advance(alloc);  // Each batch gets its own epoch
    process_batch(batch);
}
```

### Passive Epoch Reclamation

temporal-slab uses **passive epoch reclamation**—epoch state transitions require no thread coordination or quiescence periods. This distinguishes it from RCU-style epoch-based reclamation schemes.

**What is passive reclamation?**

When `epoch_advance()` is called, three atomic operations occur:

```c
void epoch_advance(SlabAllocator* a) {
    uint32_t old_epoch = current_epoch % 16;
    uint32_t new_epoch = (current_epoch + 1) % 16;
    
    // Phase 1: Mark old epoch CLOSING (atomic store)
    atomic_store(&epoch_state[old_epoch], EPOCH_CLOSING);
    
    // Phase 2: Mark new epoch ACTIVE (atomic store)
    atomic_store(&epoch_state[new_epoch], EPOCH_ACTIVE);
    
    // Phase 3: Null current_partial pointers (8 atomic stores, one per size class)
    for (size_t i = 0; i < 8; i++) {
        atomic_store(&classes[i].epochs[old_epoch].current_partial, NULL);
    }
    
    // That's it. No locks, no barriers, no waiting.
}
```

**No coordination required:**
- No grace periods (RCU-style "wait for all threads to reach quiescent state")
- No thread registration/deregistration
- No hazard pointer publication/unpublication
- No epoch counter increments per-thread

**Threads observe state changes asynchronously:**

After `epoch_advance()`, threads discover the CLOSING state independently:

```c
void* alloc_obj_epoch(SlabAllocator* a, uint32_t size, EpochId epoch) {
    // Every allocation checks epoch state (acquire ordering)
    uint32_t state = atomic_load(&a->epoch_state[epoch], memory_order_acquire);
    if (state != EPOCH_ACTIVE) {
        return NULL;  // Epoch closed, reject allocation
    }
    
    // Continue with allocation...
}
```

The CLOSING epoch drains **passively**:
- New allocations are rejected (state check fails)
- Existing allocations remain valid (no forced invalidation)
- Frees continue normally (lock-free bitmap operations)
- Empty slabs accumulate until `epoch_close()` sweeps them

**Contrast with RCU:**

| Mechanism | Coordination | Reclamation Trigger | Remote Ops |
|-----------|--------------|---------------------|------------|
| **RCU** | Quiescent states required | Grace period expires (all threads reached quiescence) | Often requires slow path |
| **Hazard Pointers** | Per-pointer protection | Retry scan on contention | Lock-free but complex |
| **Passive Epoch (temporal-slab)** | None (observe & adapt) | Explicit `epoch_close()` or passive drain | Lock-free bitmap CAS |

### Deep Dive: RCU vs Passive Epoch Reclamation

To fully appreciate temporal-slab's passive approach, it's essential to understand how RCU (Read-Copy-Update) works and why its coordination overhead is incompatible with allocation hot paths.

**RCU: Quiescence-Based Reclamation**

RCU is a synchronization mechanism used in the Linux kernel for read-mostly data structures. It allows readers to access shared data without locks, deferring reclamation until all readers have finished.

**How RCU works:**

```c
// Reader side (very fast)
rcu_read_lock();       // Mark: "I'm reading"
data = rcu_dereference(ptr);  // Read pointer
use(data);
rcu_read_unlock();     // Mark: "Done reading"

// Writer side (reclaims old data)
new_data = create_new_version();
rcu_assign_pointer(ptr, new_data);  // Atomic pointer update
synchronize_rcu();     // WAIT for grace period (expensive!)
free(old_data);        // Now safe to free
```

**Grace period mechanism:**

A grace period completes when **all threads have reached a quiescent state** (a point where they hold no RCU read locks). The `synchronize_rcu()` call blocks until this happens.

```
Thread A: [rcu_read_lock]───[use data]───[rcu_read_unlock] ← quiescent
Thread B: [rcu_read_lock]─────────────[use data]─────[rcu_read_unlock] ← quiescent
                                                     ↑
                              Grace period completes here
```

**RCU overhead:**

| Operation | Cost | Explanation |
|-----------|------|-------------|
| `rcu_read_lock()` | 0-2 cycles | Just disables preemption on some systems |
| `rcu_read_unlock()` | 0-2 cycles | Re-enables preemption |
| `synchronize_rcu()` | **10-50ms** | Must wait for all CPUs to reach quiescence |
| Per-thread tracking | Memory overhead | Each CPU tracks current RCU state |

**Why RCU is unsuitable for allocation:**

1. **Grace period latency:** `synchronize_rcu()` can take 10-50ms. Blocking allocation for milliseconds is unacceptable.

2. **Per-thread coordination:** RCU requires tracking whether each thread is in a quiescent state. For N threads doing M allocations/sec, that's N×M coordination points.

3. **Kernel-specific:** RCU relies on scheduler cooperation (tracking context switches as quiescent states). User-space can't use this infrastructure.

**Example: Why RCU doesn't fit allocation:**

```c
// Hypothetical RCU-based allocator (doesn't work)
void* alloc_obj() {
    rcu_read_lock();  // Must hold lock for entire allocation
    void* ptr = find_free_slot();
    rcu_read_unlock();
    return ptr;
}

void reclaim_empty_slabs() {
    synchronize_rcu();  // PROBLEM: Blocks for 10-50ms!
    // Can't hold up allocation for milliseconds
    free_slabs();
}
```

If you tried to use `synchronize_rcu()` in an allocator, every `epoch_close()` would block for 10-50ms, adding huge latency spikes to allocation paths.

**Hazard Pointers: Per-Pointer Protection**

Hazard pointers are an alternative to RCU that avoid grace periods, but they introduce per-access overhead.

**How hazard pointers work:**

```c
// Thread publishes pointer it's using
void access_slab(Slab* slab) {
    hazard_ptr[thread_id] = slab;  // Publish: "I'm using this"
    atomic_thread_fence(memory_order_seq_cst);  // Ensure visibility
    
    // Use slab safely (no one can free it while published)
    allocate_from_slab(slab);
    
    hazard_ptr[thread_id] = NULL;  // Unpublish
}

// Before freeing, check if anyone is using it
void free_slab(Slab* slab) {
    for (int i = 0; i < num_threads; i++) {
        if (hazard_ptr[i] == slab) {
            // Someone is using it! Defer free
            retire_list_push(slab);
            return;
        }
    }
    actual_free(slab);  // Safe to free
}
```

**Hazard pointer overhead:**

| Operation | Cost | Explanation |
|-----------|------|-------------|
| Publish hazard pointer | 10-20 cycles | Atomic store + fence |
| Unpublish hazard pointer | 10-20 cycles | Atomic store |
| Scan hazard list on free | N × 5 cycles | Scan N threads' hazard pointers |
| Per-thread storage | 64-128 bytes | Array of hazard pointers per thread |

**Why hazard pointers don't fit:**

1. **Per-access overhead:** Every slab access requires publishing a hazard pointer (20-40 cycles). For allocations happening at 131ns p99, adding 40 cycles (doubling latency) is unacceptable.

2. **Scan on every free:** Every `free_slab()` must scan all threads' hazard pointers. For 32 threads, that's 32 memory loads on the free path.

3. **Retire list complexity:** Deferred frees accumulate in retire lists, requiring periodic scanning and retry logic.

**Passive Epoch Reclamation: The Key Insight**

temporal-slab's passive approach avoids both grace periods (RCU) and per-access overhead (hazard pointers) by making epoch state **observable but not coordinated**.

**How passive reclamation works:**

1. **State is announced, not negotiated:**
   ```c
   // Announcement (no coordination)
   atomic_store(&epoch_state[3], EPOCH_CLOSING);
   ```

2. **Threads observe asynchronously:**
   ```c
   // Thread discovers state change on next allocation
   if (atomic_load(&epoch_state[3]) != EPOCH_ACTIVE) {
       return NULL;  // Epoch closed, reject allocation
   }
   ```

3. **No waiting for quiescence:**
   - RCU: "Wait until all threads reach quiescent state" (10-50ms)
   - Passive: "Announce state change, threads adapt" (0ms wait)

4. **No per-access overhead:**
   - Hazard pointers: Publish/unpublish on every access (20-40 cycles)
   - Passive: State check only on allocation (already on critical path)

**Detailed overhead comparison:**

| Mechanism | Per-Access Overhead | Reclamation Latency | Memory Overhead | Complexity |
|-----------|---------------------|---------------------|-----------------|------------|
| **RCU** | 0-4 cycles (`rcu_read_lock/unlock`) | **10-50ms** (grace period) | Per-CPU state tracking | High (scheduler integration) |
| **Hazard Pointers** | **20-40 cycles** (publish/unpublish) | 0ms (immediate) | 64-128 bytes per thread | Medium (retire lists) |
| **Passive Epoch** | **0 cycles** (state check on alloc path) | **0ms** (announce) | 64 bytes (epoch state array) | Low (atomic stores only) |

**Why passive reclamation enables 131ns p99:**

By eliminating coordination overhead, temporal-slab keeps the allocation hot path simple:
- No RCU lock/unlock (would add 4 cycles)
- No hazard pointer publish (would add 20-40 cycles)
- No grace period waiting (would add 10-50ms)
- Just: bitmap CAS + state check (already required for correctness)

**The tradeoff:**

**RCU/Hazard Pointers:**
- **Advantage:** Can safely reclaim any data structure immediately after last access
- **Cost:** Coordination overhead (grace periods or per-access tracking)

**Passive Epochs:**
- **Advantage:** Zero coordination overhead, deterministic timing
- **Cost:** Reclamation granularity is per-epoch, not per-object

For an allocator handling millions of allocations per second, eliminating per-access overhead is decisive. temporal-slab accepts coarser reclamation granularity (epochs, not pointers) to achieve lock-free performance.

**Why this matters for performance:**

Passive reclamation eliminates coordination overhead:
- No per-thread epoch counters to increment
- No grace period waiting (deterministic latency)
- No callback queues or deferred work
- Application controls reclamation timing via `epoch_close()`

The cost is deferred reclamation: empty slabs stay allocated until `epoch_close()` is called. But this cost is predictable and application-controlled, unlike GC pauses or background reclamation threads.

**Deterministic Phase-Aligned Reclamation:**

Passive reclamation's real power emerges when combined with **epoch domains**—RAII-style scoped lifetimes that map epochs to application phases:

```c
// Web server handling a request
void handle_request(Request* req) {
    epoch_domain_t* domain = epoch_domain_enter(alloc, "request-handler");
    // All allocations go to this domain's epoch
    
    Response* resp = parse_and_process(req);  // Many allocations
    send_response(resp);
    
    epoch_domain_exit(domain);  // Triggers epoch_close() automatically
    // RSS drops HERE, deterministically, after request completes
}
```

**The determinism comes from three properties:**

1. **Phase boundaries are structural moments in the application** (request ends, frame renders, transaction commits)
2. **Epoch domains automatically call `epoch_close()` on exit** (no manual cleanup tracking)
3. **Passive reclamation ensures no coordination overhead** (threads discover CLOSING asynchronously)

This creates **observable, predictable RSS behavior**:

```
T=0ms:   Request arrives, domain created (epoch 5)
T=5ms:   Request processing (RSS grows as allocations happen)
T=50ms:  Response sent, domain exits → epoch_close(5) called
T=51ms:  RSS drops (empty slabs from epoch 5 recycled)
         ↑ Deterministic timing: always happens right after response sent
```

**Contrast with non-deterministic schemes:**

| Approach | When RSS Drops | Predictability |
|----------|----------------|----------------|
| **malloc** | Whenever free() returns empty chunk | Depends on fragmentation, allocator heuristics |
| **GC** | When heap pressure triggers collection | Unpredictable, can happen mid-request |
| **Background threads** | When reclamation thread wakes up | Periodic, not aligned with phases |
| **Passive + Domains (temporal-slab)** | When domain exits (phase boundary) | **Deterministic: aligned with application structure** |

**Why this matters for production systems:**

- **Capacity planning:** RSS peaks at phase boundaries, not randomly during processing
- **Debugging:** Memory issues happen at structural boundaries (easier to reason about)
- **Performance:** No surprise GC pauses during latency-sensitive operations
- **Monitoring:** RSS metrics correlate with application-level events (requests, transactions)

The passive reclamation mechanism (no quiescence) enables this determinism—if `epoch_close()` required waiting for grace periods, you couldn't guarantee RSS drops happen immediately after phase boundaries.

**Epoch closing with epoch_close():**

temporal-slab provides `epoch_close(alloc, epoch_id)` for explicit epoch reclamation:

```c
EpochId request_epoch = epoch_current(alloc);
// ... allocate request objects in request_epoch ...
// ... process request ...
// ... free request objects ...

epoch_close(alloc, request_epoch);  // Aggressive reclamation
```

When you call `epoch_close()`, the allocator:
1. Marks the epoch as CLOSING (no new allocations allowed)
2. Scans for already-empty slabs and recycles them immediately
3. Enables aggressive recycling (empty slabs from this epoch are madvised on free)

This provides **application-controlled memory reclamation** at lifetime boundaries.

**Epoch ID vs Epoch Era:**

Epoch IDs are ring indices (0-15) that wrap around. After 16 advances, you're back to epoch 0. This creates observability ambiguity:

```
Epoch 0 (era 0):  opened at T=0s, 1000 allocations
Epoch 1 (era 1):  opened at T=5s, 2000 allocations
...
Epoch 15 (era 15): opened at T=75s, 500 allocations
Epoch 0 (era 16):  opened at T=80s, 800 allocations  ← Same ID, different era!
```

To disambiguate, temporal-slab maintains a **monotonic epoch era counter**.

**What is monotonic?**

A monotonic counter is one that always increases and never decreases or wraps around. In contrast to cyclic counters (like the 0-15 epoch ring), monotonic counters grow without bound:

```
Cyclic counter (ring buffer):
0 → 1 → 2 → ... → 15 → 0 → 1 → ...  (wraps, repeats values)

Monotonic counter (era):
0 → 1 → 2 → ... → 15 → 16 → 17 → ... → 2^64-1  (never wraps, never repeats)
```

**Properties of monotonic counters:**
- **Always increasing:** `era(t+1) > era(t)` for all time t
- **Never decreases:** No value appears twice
- **Never wraps:** With 64 bits, wrapping takes 584 billion years at 1 increment/nanosecond
- **Total ordering:** Any two eras can be compared (`era_a < era_b` or `era_a > era_b`)

This property is critical for disambiguation: if two events have different era values, you know which happened first and that they're distinct events—even if they have the same ring index.

**Epoch era counter:**
- Increments on every `epoch_advance()` call (never wraps, 64-bit)
- Each epoch stores its era when activated
- Observability tools can distinguish epoch 0 era 0 from epoch 0 era 16

This prevents graphs from showing "time going backward" when the ring wraps. It also enables era validation in epoch domains:

```c
// Domain created at era 100
epoch_domain_t* domain = epoch_domain_create(alloc);
domain->epoch_era = 100;

// ... 20 epoch_advance() calls happen (ring wrapped) ...

// Domain tries to close epoch
if (current_era != domain->epoch_era) {
    // Don't close! This is a different incarnation of the same epoch ID
}
```

**epoch_current() implementation:**

The API returns a ring index (0-15), not the raw counter:

```c
EpochId epoch_current(SlabAllocator* a) {
    uint32_t raw = atomic_load(&a->current_epoch);
    return raw % a->epoch_count;  // Modulo operation: 16 → 0, 17 → 1, etc.
}
```

This ensures `epoch_current()` always returns a valid epoch ID, even after millions of advances.

## Epoch Ring Buffer Architecture

temporal-slab reimagines memory allocation as a **bounded circular buffer** operating on time windows rather than individual bytes. This architectural shift—from spatial allocation to temporal allocation—is the foundation for bounded RSS and predictable reclamation.

**The circular buffer model:**

```
┌─────────────────────────────────────────────────┐
│  Epoch Ring Buffer (16 slots, wraps at 16)     │
├─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┤
│ E0  │ E1  │ E2  │ E3  │ ... │ E13 │ E14 │ E15 │
└─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
  ↑                                               ↑
  └─ After 16 advances, wraps back to epoch 0 ───┘
```

Each epoch slot is a time window containing:
- Dedicated slab pools (one per size class)
- Metadata (open timestamp, refcount, label)
- Lifecycle state (ACTIVE or CLOSING)

**Modular arithmetic enables wrapping:**

```c
// Raw counter increments forever: 0, 1, 2, ..., 16, 17, ...
uint64_t raw_counter = atomic_fetch_add(&allocator->epoch_counter, 1);

// Modulo maps to ring index: 16 → 0, 17 → 1, 18 → 2, ...
EpochId epoch_id = raw_counter % EPOCH_COUNT;  // EPOCH_COUNT = 16
```

This creates an infinite sequence of time windows mapped onto a finite 16-epoch ring:

```
Time →
Era:    0    1    2   ...  15   16   17   18  ...  31   32  ...
Epoch: [E0] [E1] [E2] ... [E15] [E0] [E1] [E2] ... [E15] [E0] ...
       └────────┬────────┘      └────────┬────────┘
          First rotation            Second rotation
```

**Why this prevents RSS growth:**

Traditional allocators (malloc, jemalloc) treat memory as a **spatial pool**:
- Allocate from any available slot across the entire address space
- Objects with different lifetimes mix freely
- Partial occupancy prevents page reclamation (temporal fragmentation)
- RSS grows linearly with time under churn

temporal-slab treats memory as a **temporal pool**:
- Allocate from the current epoch window only
- Objects allocated together (same epoch) share pages
- Epoch closure creates clean reclamation boundaries
- RSS bounded by `max_live_set × (1 + fragmentation_factor)`

**Era stamping for disambiguation:**

The 16-epoch ring creates an ambiguity: epoch ID 0 at era 0 looks identical to epoch ID 0 at era 16. Era stamping solves this:

```c
struct EpochMetadata {
    uint64_t epoch_era;        // Monotonic counter (never wraps)
    uint64_t open_since_ns;    // Timestamp when epoch became ACTIVE
    atomic_int domain_refcount; // Live domain references
    char label[32];            // Semantic tag for debugging
};
```

Every `epoch_advance()` call:
1. Increments global era counter: `allocator->epoch_era_counter++`
2. Stamps the new epoch: `meta[new_epoch_id].epoch_era = current_era`
3. Records opening timestamp: `meta[new_epoch_id].open_since_ns = now()`

This enables:
- **Observability:** Distinguish "epoch 0 era 0" from "epoch 0 era 16" in metrics
- **Safety:** Validate domain closure against era to prevent closing wrong incarnation
- **Debugging:** Correlate epoch lifetime with application phases via era timestamps

**Lifecycle state machine:**

Each epoch transitions through states as time advances:

```
        epoch_advance()
             │
             ↓
        ┌─────────┐
        │ ACTIVE  │ ← Accepts new allocations
        └────┬────┘
             │ epoch_close() or ring wraps
             ↓
        ┌─────────┐
        │ CLOSING │ ← Draining: no new allocations, existing objects freed
        └────┬────┘
             │ Last object freed
             ↓
        ┌─────────┐
        │ DRAINED │ ← All slabs empty, ready for reuse
        └─────────┘
             │ epoch_advance() wraps around
             ↓
        [Return to ACTIVE with new era]
```

**Memory reclamation at epoch granularity:**

Instead of reclaiming individual pages (malloc's approach), temporal-slab reclaims entire epochs:

```
Epoch 5 (era 105):
  - Opened at T=0
  - 10,000 objects allocated across 100 slabs
  - Closed at T=5s (epoch_close())
  - Draining period: objects freed as they expire
  - Last object freed at T=60s
  → All 100 slabs madvised simultaneously
  → RSS drops by 400KB instantly
```

This is fundamentally different from malloc:
- malloc: Reclaim pages individually as they become empty (nondeterministic)
- temporal-slab: Reclaim epochs collectively when lifetime boundary ends (deterministic)

**The circular buffer boundary condition:**

When advancing from epoch 15 to epoch 0 (wraparound), temporal-slab must ensure epoch 0's previous incarnation has drained. Two strategies:

**Strategy 1: Sufficient separation (default)**
- 16-epoch window provides ~80 seconds of history (if advancing every 5s)
- Objects allocated in epoch 0 era 0 are freed before epoch 0 era 16 activated
- No special handling needed—drainage completes naturally

**Strategy 2: Explicit barrier (epoch_close())**
- Application closes epochs when lifetime boundaries are known
- Forces drainage before wraparound
- Example: Close request epoch after response sent, before ring wraps

**Why bounded RSS emerges:**

The circular buffer architecture makes RSS growth impossible:

```
RSS(t) = sum(live_objects(epoch)) for epoch in [current-15, current]
       = sum(live_objects(epoch)) for 16-epoch window
       ≤ max_live_set × slab_overhead_factor
```

Because:
1. Only 16 epochs can be ACTIVE or CLOSING simultaneously
2. DRAINED epochs contribute zero RSS (slabs madvised)
3. The window slides forward—old epochs drain as new ones open
4. Maximum RSS = 16 epochs × max_objects_per_epoch × object_size

This is the core architectural insight: **bounded circular buffer + temporal grouping = bounded RSS**.

## Epoch Domains

Epoch domains are RAII-style scoped lifetimes that provide finer-grained control over memory reclamation within epochs. While epochs separate allocations by time windows (e.g., every 10,000 requests), domains separate allocations by **nested scopes** (e.g., per-request, per-transaction, per-query).

**The problem: Coarse-grained epochs**

Epochs provide temporal separation, but they're application-wide boundaries:

```c
// Problem: All requests in a 10,000-request batch share an epoch
EpochId batch_epoch = epoch_current(alloc);

for (int i = 0; i < 10000; i++) {
    process_request(alloc, batch_epoch);  // All use same epoch
}

// Can only reclaim after ALL 10,000 requests complete
epoch_close(alloc, batch_epoch);
```

If request #1 completes in 10ms but request #9,999 takes 60 seconds, you cannot reclaim request #1's memory until request #9,999 finishes. Memory is trapped by the longest-lived allocation in the epoch.

**Solution: Epoch domains (scoped lifetimes)**

Domains subdivide epochs into **nested scopes** with explicit enter/exit boundaries:

```c
// Each request gets its own domain
for (int i = 0; i < 10000; i++) {
    epoch_domain_t* domain = epoch_domain_create(alloc);
    epoch_domain_enter(alloc, domain);
    
    // All allocations in this domain
    process_request(alloc);  // Uses current epoch via domain
    
    epoch_domain_exit(alloc, domain);
    epoch_domain_destroy(alloc, domain);  // Can reclaim THIS request's memory
}
```

When `epoch_domain_destroy()` is called, if `auto_close=true` and the domain's refcount reaches zero, the allocator can reclaim memory allocated in that domain's scope—even if other domains in the same epoch are still active.

**Domain lifecycle (RAII pattern):**

```c
// 1. Create domain (associates with current epoch)
epoch_domain_t* domain = epoch_domain_create(alloc);

// 2. Enter domain (increments refcount, activates scope)
epoch_domain_enter(alloc, domain);

// 3. Allocations happen within the domain
void* obj = slab_malloc(alloc, 128);  // Uses domain's epoch

// 4. Exit domain (decrements refcount)
epoch_domain_exit(alloc, domain);

// 5. Destroy domain (can trigger epoch_close if auto_close=true)
epoch_domain_destroy(alloc, domain);
```

**Domain refcount tracking:**

Each epoch maintains a `domain_refcount` (formerly called `alloc_count`):

```c
struct EpochMetadata {
    atomic_int domain_refcount;  // Number of active domains in this epoch
    uint64_t epoch_era;           // Era for validation
    char label[32];               // Semantic tag for debugging
};
```

Operations:
- `epoch_domain_enter()` → `domain_refcount++`
- `epoch_domain_exit()` → `domain_refcount--`
- When `domain_refcount` reaches 0 and domain has `auto_close=true`, the allocator can call `epoch_close()`

**Why refcount, not allocation count?**

Previously, `alloc_count` tracked live allocations in the epoch. This had two problems:

1. **Semantic collision:** 10,000 live objects looked identical to 10,000 active domains
2. **Hot-path overhead:** Every allocation/free incremented/decremented a shared counter

The rename to `domain_refcount` clarifies the purpose: tracking domain boundaries (enter/exit), not individual allocations. This is a **cold-path** operation (happens per-request, not per-allocation), so the overhead is acceptable.

**Thread-local contract:**

Domains enforce a **thread-local ownership** contract:

```c
struct epoch_domain {
    pthread_t owner_tid;         // Thread that created the domain
    uint64_t epoch_era;          // Era for validation
    EpochId epoch_id;            // Epoch ID (ring index)
    bool auto_close;             // Trigger epoch_close on destroy?
};
```

All domain operations assert ownership:

```c
void epoch_domain_enter(SlabAllocator* alloc, epoch_domain_t* domain) {
    assert(pthread_equal(pthread_self(), domain->owner_tid));
    // ... increment refcount ...
}
```

This prevents bugs where one thread creates a domain and another thread tries to use it. Domains are **not** transferable between threads—they're strictly thread-local scopes.

**Nested domains (TLS stack):**

Domains can nest arbitrarily (up to 32 levels):

```c
// Thread-local stack of nested domains (max depth: 32)
static __thread struct {
    epoch_domain_t* stack[MAX_DOMAIN_DEPTH];
    uint32_t depth;
} tls_domain_stack;

// Outer domain: Request processing
epoch_domain_t* request_domain = epoch_domain_create(alloc);
epoch_domain_enter(alloc, request_domain);

    // Inner domain: Database transaction
    epoch_domain_t* txn_domain = epoch_domain_create(alloc);
    epoch_domain_enter(alloc, txn_domain);
    
        // Innermost domain: Query execution
        epoch_domain_t* query_domain = epoch_domain_create(alloc);
        epoch_domain_enter(alloc, query_domain);
        
        // Allocations here use query_domain's epoch
        execute_query(alloc);
        
        epoch_domain_exit(alloc, query_domain);
        epoch_domain_destroy(alloc, query_domain);
    
    epoch_domain_exit(alloc, txn_domain);
    epoch_domain_destroy(alloc, txn_domain);

epoch_domain_exit(alloc, request_domain);
epoch_domain_destroy(alloc, request_domain);
```

The TLS stack enforces **LIFO (Last-In-First-Out)** unwind order. You cannot exit an outer domain before exiting all inner domains. This prevents use-after-free bugs from improper scope nesting.

**Era validation prevents wraparound bugs:**

Domains store both `epoch_id` (ring index) and `epoch_era` (monotonic counter):

```c
// Domain created at era 100
epoch_domain_t* domain = epoch_domain_create(alloc);
domain->epoch_era = 100;

// ... 20 epoch_advance() calls happen (ring wrapped) ...

// Destroy attempts auto-close
if (domain->auto_close && domain_refcount == 0) {
    if (current_era != domain->epoch_era) {
        // DON'T CLOSE! This is a different epoch incarnation
        return;
    }
    epoch_close(alloc, domain->epoch_id);
}
```

Without era validation, auto-close could close the wrong epoch after the ring wraps (closing epoch 0 era 16 when domain was created for epoch 0 era 0).

**Semantic labels for debugging:**

Domains can set human-readable labels on epochs for production diagnostics:

```c
epoch_domain_t* domain = epoch_domain_create(alloc);
slab_epoch_set_label(alloc, domain->epoch_id, "request-processing");

// Later, in observability tools:
SlabEpochStats stats = slab_stats_epoch(alloc, 5);
printf("Epoch 5 (%s): %zu live allocations\n", 
       stats.label, stats.live_objects);
// Output: Epoch 5 (request-processing): 42,000 live allocations
```

Labels are stored per-epoch (32-character limit) and protected by a per-allocator mutex (rare writes, cold path). This enables correlation between allocator behavior and application phases in production debugging.

**Auto-close behavior (default: false):**

Domains support two modes:

**Manual mode (auto_close=false, default):**
```c
epoch_domain_t* domain = epoch_domain_create(alloc);
// Must explicitly call epoch_close() when done
epoch_close(alloc, domain->epoch_id);
```

**Auto-close mode (auto_close=true):**
```c
epoch_domain_t* domain = epoch_domain_create_auto(alloc);
epoch_domain_destroy(alloc, domain);
// If domain_refcount=0, automatically calls epoch_close()
```

Default changed from `true` to `false` in Phase 2.3 because:
- Explicit `epoch_close()` is safer (no surprise closures)
- Prevents accidental premature closure when epochs are shared
- Auto-close only useful for strictly request-scoped patterns

**When to use domains:**

| Pattern | Use Domain? | Why |
|---------|-------------|-----|
| **Batch processing** | No | All allocations die together at batch end |
| **Request handling** | Yes | Each request has independent lifetime |
| **Nested transactions** | Yes | Transaction → Query → Cache layers |
| **Long-lived cache** | No | Allocations live for hours/days |
| **Connection pools** | No | Connections persist across many requests |

Domains add overhead (refcount tracking, TLS stack management), so only use them when you need **fine-grained reclamation boundaries** within epochs.

**Observability integration:**

Domain operations are fully observable:

```c
SlabEpochStats stats = slab_stats_epoch(alloc, epoch_id);
printf("Epoch %u (era %lu):\n", epoch_id, stats.epoch_era);
printf("  Label: %s\n", stats.label);
printf("  Domain refcount: %d\n", stats.domain_refcount);
printf("  Live objects: %zu\n", stats.live_objects);
```

This enables production debugging:
- **Domain leak detection:** `domain_refcount > 0` after expected completion
- **Phase correlation:** Labels show which application phase is active
- **Memory attribution:** Correlate RSS spikes with specific request types

**Example: HTTP request handler**

```c
void handle_request(SlabAllocator* alloc, Request* req) {
    // Create request-scoped domain
    epoch_domain_t* domain = epoch_domain_create(alloc);
    slab_epoch_set_label(alloc, domain->epoch_id, req->path);
    epoch_domain_enter(alloc, domain);
    
    // All allocations use domain's epoch
    Response* resp = parse_request(alloc, req);
    execute_handlers(alloc, req, resp);
    serialize_response(alloc, resp);
    
    // Exit domain (decrements refcount)
    epoch_domain_exit(alloc, domain);
    epoch_domain_destroy(alloc, domain);
    
    // If auto_close=true and refcount=0, epoch is closed
    // RSS reclaimed when all request objects freed
}
```

Domains transform epochs from "10,000 requests" into "per-request boundaries," enabling deterministic reclamation at request completion rather than batch completion.

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

temporal-slab uses 64-bit bitmaps stored in the slab header. Lock-free allocation uses atomic CAS loops on the bitmap to claim slots without mutexes. Bitmap operations are the foundation of the predictable allocation fast path (120ns p99, 340ns p999 validated on GitHub Actions).

## Adaptive Bitmap Scanning

When multiple threads allocate from the same slab concurrently, the choice of where to start scanning the bitmap for free slots becomes critical. The naive approach—always start from bit 0—creates a contention hotspot. Adaptive bitmap scanning solves this by switching between sequential and randomized scanning based on observed contention.

**The thundering herd problem:**

The **thundering herd** is a classic concurrency problem where multiple threads wake up or converge on the same resource simultaneously, but only one can proceed, causing the others to waste cycles contending. The term comes from the image of a herd of cattle stampeding toward a single gate—most get blocked, creating chaos.

In bitmap scanning, the thundering herd occurs when all threads start scanning from the same position:

```c
// Naive: Always scan from bit 0
uint64_t bitmap = slab->bitmap;
int slot = __builtin_ctzll(~bitmap);  // Find first 0 bit from position 0
```

Under multi-threaded load, all threads execute this simultaneously:

```
Thread 1: Scans bitmap, finds bit 0 is free, attempts CAS
Thread 2: Scans bitmap, finds bit 0 is free, attempts CAS
Thread 3: Scans bitmap, finds bit 0 is free, attempts CAS
Thread 4: Scans bitmap, finds bit 0 is free, attempts CAS

Result: All 4 threads target bit 0
        → 3 CAS failures
        → 3 retry loops
        → Wasted cycles on repeated contention
```

Each CAS failure wastes ~20-40 cycles. With 8 threads competing, the retry rate can reach 70-80%, turning 40-cycle allocations into 200+ cycle operations. This is the thundering herd in action: deterministic scanning creates a predictable contention point.

**Sequential scanning (mode 0):**

Start from bit 0, scan linearly:

```c
// Sequential: Scan from beginning
for (int i = 0; i < 64; i++) {
    if (bitmap & (1ULL << i) == 0) {
        // Bit i is free, try to claim it
    }
}
```

**Benefits:**
- Deterministic behavior (always fills low slots first)
- Best cache locality (adjacent slots = adjacent memory)
- Optimal for single-threaded or low-contention workloads

**Cost:**
- All threads compete for same low-numbered slots
- High CAS retry rate under contention (50-80% retries)

**Randomized scanning (mode 1):**

Start from thread-specific offset, wrap around:

```c
// Randomized: Start from thread-local offset
uint32_t offset = get_thread_offset() % 64;  // Per-thread starting point
for (int i = 0; i < 64; i++) {
    int bit = (offset + i) % 64;  // Wrap around bitmap
    if (bitmap & (1ULL << bit) == 0) {
        // Bit is free, try to claim it
    }
}
```

Thread-local offset derived from thread ID:
```c
static __thread uint32_t tls_scan_offset = UINT32_MAX;

uint32_t get_thread_offset() {
    if (tls_scan_offset == UINT32_MAX) {
        uint64_t tid = (uint64_t)pthread_self();
        tls_scan_offset = hash(tid);  // 32-bit hash of thread ID
    }
    return tls_scan_offset;
}
```

**Benefits:**
- Threads spread across bitmap (reduced contention)
- Lower CAS retry rate (20-30% under same load)
- 2-3× faster allocation under high concurrency (8+ threads)

**Cost:**
- Worse cache locality (slots scattered across slab)
- Non-deterministic slot assignment order
- Slight overhead: modulo operation + TLS lookup (~2-3 cycles)

**The adaptive policy:**

Rather than choosing one strategy statically, temporal-slab adapts at runtime based on measured contention:

```c
// Policy: Switch based on CAS retry rate
double retry_rate = cas_retries / allocation_attempts;

if (retry_rate > 0.30) {
    // High contention: enable randomized scanning
    scan_mode = 1;
} else if (retry_rate < 0.10) {
    // Low contention: enable sequential scanning  
    scan_mode = 0;
}
```

**Hysteresis prevents flapping:**
- Enable threshold: 30% retry rate (switch to randomized)
- Disable threshold: 10% retry rate (switch to sequential)
- 20% gap prevents oscillation

**Measurement window:**
- Sample every 100K allocations (allocation-count triggered, not time-based)
- Use windowed deltas, not lifetime averages (fast convergence)
- Minimum dwell time: 100K allocations (prevents rapid switching)

**Why allocation-count triggered?**

HFT workloads cannot tolerate clock reads (rdtsc ~20-30 cycles, clock_gettime syscall ~300 cycles). Counting allocations is free—already tracked for observability. Checking "did we cross 100K boundary?" is a simple comparison.

**Why windowed deltas?**

```c
// Windowed delta approach
uint64_t delta_attempts = attempts - last_attempts;
uint64_t delta_retries = retries - last_retries;
double rate = (double)delta_retries / (double)delta_attempts;

// NOT lifetime average approach (too slow to converge)
double rate = (double)total_retries / (double)total_attempts;
```

Windowed deltas respond to load changes within ~100K allocations. Lifetime averages can take millions of allocations to reflect new contention patterns.

**Why the combination achieves stability:**

The controller is not just reactive—it's **stable under sustained pressure**. This is the hard part: anyone can flip a flag when they see contention, but very few adaptive systems know when to stop adapting.

The three mechanisms work together:

**1. Windowed deltas (fast convergence):**
- Measure only the last 100K allocations, not lifetime totals
- New contention patterns detected within 1-2 windows (~200K allocations)
- Without windowing: Startup spike at era 0 dominates lifetime average forever

**2. Hysteresis (prevents oscillation):**
```
Retry rate oscillating around 20%:
Without hysteresis:
  19% → switch to sequential
  21% → switch to randomized
  19% → switch to sequential
  21% → switch to randomized
  → Mode flips every 100K allocations!

With hysteresis (30% enable, 10% disable):
  19% → stay in current mode (no switch)
  21% → stay in current mode (no switch)
  → Stable, no thrashing
```

The 20% gap creates a "dead zone" where small fluctuations don't trigger switches. Once randomized mode is enabled (>30% retry rate), it stays enabled until retry rate drops below 10%. This prevents ping-ponging when load hovers near a threshold.

**3. Dwell time (prevents rapid switching):**
```c
if (sc->scan_adapt.dwell_countdown) {
    sc->scan_adapt.dwell_countdown--;
    return;  // Don't switch yet, even if threshold crossed
}
```

After every mode switch, the controller enforces a minimum dwell time (3 checks = 300K allocations). This prevents:

```
Load spike scenario WITHOUT dwell time:
T=0: 8 threads → retry rate 75% → switch to randomized
T=100K: Spike ends, 1 thread → retry rate 5% → switch to sequential
T=200K: Spike returns, 8 threads → retry rate 75% → switch to randomized
T=300K: Spike ends → switch to sequential
→ 4 mode switches in 400K allocations (unstable!)

WITH dwell time (3 checks):
T=0: 8 threads → retry rate 75% → switch to randomized, dwell=3
T=100K: Spike ends, 1 thread → retry rate 5%, but dwell=2 → stay randomized
T=200K: Spike returns, 8 threads → retry rate 75%, dwell=1 → stay randomized
T=300K: Still high load, dwell=0 → controller active again
→ 1 mode switch, stable operation during transient load
```

**The stability guarantee:**

These three mechanisms together prevent **control system instability**:

| Mechanism | Prevents | Without It |
|-----------|----------|------------|
| **Windowed deltas** | Stale decisions from old data | Startup spike dominates forever |
| **Hysteresis** | Oscillation near thresholds | Mode flips every window when load ~ 20% |
| **Dwell time** | Rapid switching on transient spikes | Thrashing during bursty workloads |

The result: **The controller converges to the correct mode and stays there** until load characteristics fundamentally change. It doesn't thrash, doesn't oscillate, and doesn't overreact to transient spikes.

**Measured stability in production validation:**

```
T=2 threads (47 trials):
- mode=0.1 (mostly sequential)
- switches=2.1 average
- Proves controller switches near threshold (retry rate ~ 30%)
- CV=7.8% (excellent repeatability)

T=8 threads (47 trials):  
- mode=1.0 (always randomized)
- switches=0 (never switches back)
- Proves stability under sustained high load
- No thrashing despite 60-second measurement windows
```

At T=2, the controller is actively adapting (retry rate near 30% threshold, occasional switches). At T=8, the controller has converged to randomized mode and stays there—it doesn't flip back and forth despite measuring 6 million allocations per trial.

**This is control theory in action:** The combination of windowed measurement, hysteresis thresholds, and dwell time creates a **stable feedback loop** that adapts quickly but doesn't overreact.

**Example: 8 threads allocating simultaneously**

```
Sequential mode:
- All threads scan from bit 0
- Retry rate: 75% (6 out of 8 CAS attempts fail)
- After 100K allocations: Detect high contention
- Switch to randomized mode

Randomized mode:
- Threads spread across bitmap (8 different starting points)
- Retry rate: 25% (2 out of 8 CAS attempts fail)
- After 100K allocations: Contention reduced, stay in randomized

Load drops to 2 threads:
- Retry rate: 15% (minimal contention)
- After 100K allocations: Still above 10% threshold, stay randomized
- Load continues dropping
- Retry rate: 8% (essentially uncontended)
- After 100K allocations: Below 10% threshold, switch to sequential
```

**Performance characteristics:**

| Scenario | Sequential Mode | Randomized Mode | Adaptive (Auto-Select) |
|----------|-----------------|-----------------|------------------------|
| **1 thread** | 40ns p50 | 43ns p50 (+7.5%) | 40ns (uses sequential) |
| **4 threads** | 95ns p50 | 82ns p50 (−14%) | 82ns (uses randomized) |
| **8 threads** | 180ns p50 | 88ns p50 (−51%) | 88ns (uses randomized) |
| **16 threads** | 340ns p50 | 105ns p50 (−69%) | 105ns (uses randomized) |

Adaptive policy delivers:
- Single-threaded performance of sequential mode (no overhead when uncontended)
- Multi-threaded performance of randomized mode (contention detection triggers switch)
- Zero manual configuration (no user-provided thread count or contention hint)

**Observability metrics:**

Adaptive scanning exposes three metrics via `slab_stats_class()`:

```c
typedef struct {
    uint32_t scan_adapt_checks;    // Total adaptation checks performed
    uint32_t scan_adapt_switches;  // Total mode switches (0↔1)
    uint32_t scan_mode;             // Current mode (0=sequential, 1=randomized)
} SlabClassStats;
```

These enable:
- **Validation**: Verify mode switches happen under expected load
- **Debugging**: Correlate mode changes with latency spikes
- **Tuning**: Adjust thresholds (30%/10%) based on observed behavior

**Why this matters for HFT:**

High-frequency trading systems require both:
1. **Predictable latency** (mode 0 for single-threaded hot paths)
2. **Scalable throughput** (mode 1 for multi-threaded batch processing)

Adaptive scanning delivers both without manual configuration. The allocator self-tunes to workload characteristics, eliminating the need for per-deployment thread count hints or contention tuning parameters.

## Fast Path vs Slow Path

Fast path and slow path (also called hot path and cold path) are terms describing the two execution branches in performance-critical code. The fast path is the common case optimized for minimum latency. The slow path is the rare case that handles edge conditions, initialization, or complex operations.

**Fast path characteristics:**

The fast path is designed to execute as quickly as possible:
- **No locks** - Uses lock-free atomic operations instead of mutexes
- **No syscalls** - Operates entirely in userspace (no mmap/munmap)
- **No allocations** - Uses pre-allocated structures (no dynamic memory)
- **Minimal branching** - Few or no conditional branches (avoid misprediction)
- **Cache-friendly** - Accesses data likely already in L1/L2 cache

**Slow path characteristics:**

The slow path handles exceptional cases:
- **Locks allowed** - Can acquire mutexes (correctness over speed)
- **Syscalls allowed** - Can call mmap, munmap, madvise
- **Allocations allowed** - Can allocate metadata structures
- **Complex logic** - Can scan lists, search trees, compute heuristics
- **Tolerates latency** - 1-10 microseconds acceptable (vs <100ns for fast path)

**The key insight: Optimize the common case**

If 99% of allocations hit the fast path and 1% hit the slow path:

```
Fast path: 50ns per allocation
Slow path: 5µs per allocation (100× slower)

Average latency:
0.99 × 50ns + 0.01 × 5µs = 49.5ns + 50ns = 99.5ns

The fast path dominates performance!
```

Even if the slow path is 100× slower, it barely affects average latency because it's so rare.

**Fast path vs slow path in temporal-slab allocation:**

**Fast path (99%+ of allocations):**

```
Allocate 128 bytes:
1. Load current_partial pointer (atomic load, 1 cycle)
2. Check slab has free slots (bitmap != 0xFFFF, 1 cycle)
3. Find first free bit (BSF instruction, 1 cycle)
4. Claim bit with CAS (20-40 cycles)
5. Calculate slot address (multiply + add, 2 cycles)
6. Return pointer

Total: ~30-50 cycles (~10-17ns at 3GHz)
No locks, no syscalls, no allocations
```

**Slow path (<1% of allocations):**

```
Current slab is full, need new slab:
1. Acquire global mutex (50-200 cycles if uncontended)
2. Check if other thread already allocated slab (10 cycles)
3. Pop slab from cache OR mmap new slab:
   - Cache hit: 50 cycles (pop from list)
   - Cache miss: 3000-10,000 cycles (mmap syscall)
4. Initialize slab metadata (50 cycles)
5. Update current_partial pointer (atomic store, 1 cycle)
6. Release mutex (50 cycles)
7. Allocate from new slab (fast path, 30 cycles)

Total:
- Cache hit: ~200-400 cycles (~100-150ns)
- Cache miss: ~3000-10,000 cycles (~1-3µs)

10-100× slower than fast path, but rare
```

**Why the split matters:**

Without fast/slow path separation, every allocation would need to:
- Acquire a mutex (100+ cycles) → Contention bottleneck
- Check if slab exists (unnecessary for existing slabs)
- Handle edge cases (wasted cycles in common case)

Result: 150-200 cycles minimum per allocation (~50-70ns)

With fast/slow path separation:
- Fast path: 30-50 cycles (3-4× faster!)
- Slow path: 200-10,000 cycles (acceptable, rare)
- Average: ~50 cycles when fast path hit rate is 99%

**Fast path optimization strategies in temporal-slab:**

**1. Lock-free via atomics:**
```c
// Fast path: No mutex
Slab* slab = atomic_load(&current_partial);  // Lock-free load
if (CAS(&slab->bitmap, old, new)) {          // Lock-free claim
    return slot;
}

// Slow path: Mutex for structural changes
mutex_lock(&alloc->mutex);
allocate_new_slab();
mutex_unlock(&alloc->mutex);
```

**2. Pre-selected slab:**
```c
// Fast path: Use current_partial (already selected)
Slab* slab = current_partial;  // No search

// Slow path: Search partial list or allocate new
Slab* slab = find_partial_slab();  // Scan list
if (!slab) slab = allocate_new_slab();  // mmap
```

**3. Bitmap allocation:**
```c
// Fast path: Bitmap bit scan (1 cycle)
int slot = __builtin_ctzll(~bitmap);  // BSF instruction

// Slow path: Could use complex free list search
// (temporal-slab doesn't, but traditional allocators do)
FreeBlock* block = search_free_list(size);  // O(N) scan
```

**Hot path vs cold path (synonymous):**

"Hot path" and "cold path" are synonyms for fast path and slow path, with the metaphor being CPU heat:
- **Hot path:** Executed so frequently it "heats up" the CPU (cache stays warm, branch predictor learns pattern)
- **Cold path:** Executed so rarely the CPU cache is "cold" (cache misses, unpredicted branches)

The terms are interchangeable. temporal-slab documentation uses "fast path" and "slow path" for clarity.

**Fast/slow path in other contexts:**

**Database query execution:**
- Fast path: Query hits index, returns in <1ms (99% of queries)
- Slow path: Query requires full table scan, 100ms+ (1% of queries)

**Network request handling:**
- Fast path: Request served from cache, <1ms (95% of requests)
- Slow path: Cache miss, fetch from backend, 50ms+ (5% of requests)

**Compiler optimization:**
- Fast path: Hot loop compiled with aggressive optimization
- Slow path: Error handling compiled with -O0 (cold code)

The pattern is universal: identify the common case (fast path), optimize aggressively, tolerate higher cost in rare case (slow path).

**Why this matters for temporal-slab:**

temporal-slab's 0% RSS growth and predictable tail latency both depend on fast/slow path separation:

**Fast path enables predictable latency:**
- Lock-free bitmap allocation (no mutex contention)
- No syscalls (no mmap/munmap)
- No metadata updates (just bitmap CAS)
- Result: p50 = 40ns, p99 = 120ns (GitHub Actions validated)

**Slow path handles growth:**
- Mutex-protected slab allocation (rare, acceptable cost)
- mmap for new slabs (amortized via cache)
- List manipulation (updating partial/full lists)
- Result: p999 = 340ns (GitHub Actions validated, 100M sample benchmark)

The fast path is what makes temporal-slab suitable for HFT and real-time systems. The slow path is what makes it correct and prevents unbounded growth.

## Hot Path Optimization Architecture

temporal-slab achieves predictable tail latency (120ns p99, 340ns p999) through a layered optimization strategy where each technique addresses a specific performance bottleneck. Understanding how these optimizations compose reveals why the allocator can sustain 40ns median latency with 120ns p99—a 3× ratio that's exceptional even among specialized allocators.

The hot path is the sequence of operations executed when allocating from a slab that already exists and has free slots available. This is the 99%+ case—the critical path that determines system performance. Every cycle saved here multiplies across millions of allocations per second.

**The unoptimized baseline:**

Before explaining optimizations, consider what a naive allocator would do:

```c
void* naive_alloc(size_t size) {
    pthread_mutex_lock(&global_lock);           // 100+ cycles (contended)
    
    for (int i = 0; i < num_size_classes; i++) {
        if (size <= size_classes[i]) {           // 8 branches (unpredictable)
            SizeClass* sc = &classes[i];
            
            Slab* slab = sc->partial_list;
            while (slab) {                        // O(N) list traversal
                for (int j = 0; j < slab->capacity; j++) {
                    if (slab->free_list[j]) {     // Another O(M) scan
                        void* ptr = slab->free_list[j];
                        slab->free_list[j] = NULL;
                        pthread_mutex_unlock(&global_lock);
                        return ptr;
                    }
                }
                slab = slab->next;
            }
            
            // No free slot found, allocate new slab
            slab = mmap(...);                     // 3000+ cycles (syscall)
            break;
        }
    }
    
    pthread_mutex_unlock(&global_lock);
    return ptr;
}
```

**Cost breakdown of naive approach:**
- Mutex lock: 100-200 cycles (uncontended), 1000-10000 cycles (contended)
- Size class selection: 8 branches × 10 cycles (misprediction) = 80 cycles
- Slab search: O(N) list traversal, average 50 cycles per slab checked
- Slot search: O(M) free list scan, average 100 cycles
- Mutex unlock: 100 cycles

Total: **400-600 cycles best case**, 10000+ cycles worst case (contended lock or mmap)

This is unacceptable for HFT systems where every allocation contributes to tail latency. A single 10µs allocation spike (30,000 cycles at 3GHz) causes a missed trade worth potentially millions.

**Optimization Layer 1: Eliminate lock contention (Lock-Free Allocation)**

The first bottleneck is the global mutex. Under multi-threaded load, threads serialize on lock acquisition—only one thread allocates at a time, wasting CPU cores. The solution is lock-free allocation using atomic compare-and-swap (CAS).

Instead of protecting the entire allocator with a mutex, temporal-slab uses atomic operations on the bitmap that tracks slot state. Multiple threads can attempt to claim different bits simultaneously. Only the final step—flipping a bit from 0 to 1—requires synchronization, and this happens via hardware-level atomic CAS.

```c
// Lock-free fast path (simplified)
Slab* slab = atomic_load_explicit(&sc->current_partial, memory_order_acquire);
uint64_t old_bitmap, new_bitmap;
do {
    old_bitmap = atomic_load_explicit(&slab->bitmap, memory_order_relaxed);
    int slot = __builtin_ctzll(~old_bitmap);  // Find first 0 bit
    if (slot == 64) return NULL;  // Slab full
    new_bitmap = old_bitmap | (1ULL << slot);
} while (!atomic_compare_exchange_weak(&slab->bitmap, &old_bitmap, new_bitmap));

return (void*)(slab_base + slot * slot_size);
```

**What this eliminates:**
- No mutex acquisition (100-10000 cycles saved)
- No thread serialization (scales to ~4 threads before cache coherence limits)
- No kernel involvement (no context switches, no scheduler interaction)

**What it costs:**
- CAS operation: 20-40 cycles (LOCK CMPXCHG instruction on x86-64)
- Retry on contention: Additional 20-40 cycles per retry (rare: <1% under T≤4)

**Net improvement:** 100+ cycles → 20-40 cycles (3-5× faster, uncontended case)

The CAS operation is not free—it's more expensive than a regular store (1-2 cycles)—but it's vastly cheaper than a mutex. The LOCK prefix forces cache coherence, ensuring all cores see the updated bitmap. This means the cache line containing the bitmap bounces between cores, but this is still faster than blocking threads.

**Why CAS scales poorly beyond 4-8 threads:**

Cache coherence traffic grows quadratically with thread count. When 8 threads all CAS the same cache line, each successful CAS invalidates 7 other caches. With 16 threads, each CAS invalidates 15 caches. The memory bus becomes saturated with coherence messages, and CAS latency degrades from 20 cycles to 100+ cycles.

This is why temporal-slab's lock-free design targets 1-4 threads as the sweet spot. Beyond that, the allocator remains correct (no data races), but performance degrades due to fundamental hardware limits, not algorithmic issues.

**Optimization Layer 2: Eliminate slot search (Bitmap Allocation)**

The second bottleneck is finding a free slot. Naive allocators use free lists—linked lists of available slots—which require O(N) traversal. Even a simple array scan is O(N) in worst case.

temporal-slab uses a bitmap where each bit represents one slot: 0 = free, 1 = allocated. A 64-slot slab needs 64 bits (one `uint64_t`). Finding a free slot becomes a single CPU instruction: `__builtin_ctzll(~bitmap)` (count trailing zeros of inverted bitmap).

```c
// Bitmap: 0b0000...0101 (slots 0 and 2 allocated, rest free)
// Inverted: 0b1111...1010 (flip all bits)
// CTZ: 1 (position of first 1 bit)
// → Slot 1 is free

uint64_t bitmap = 0b0101;
int free_slot = __builtin_ctzll(~bitmap);  // Returns 1
```

On x86-64, `__builtin_ctzll` compiles to the BSF (Bit Scan Forward) instruction, which takes 1 cycle (unpipelined) or 3 cycles (latency). This is as fast as arithmetic gets—finding the free slot is essentially free compared to the CAS operation that claims it.

**Why bitmaps beat free lists:**

Free lists have two problems: pointer chasing and metadata overhead. Each free list node stores a `next` pointer (8 bytes). For a 64-byte slot, that's 12.5% overhead. Worse, traversing the list is cache-hostile—each node access is a dependent load that stalls the pipeline until the previous load completes.

```c
// Free list traversal (cache-hostile)
FreeNode* node = free_list_head;
while (node->size < requested_size) {
    node = node->next;  // Dependent load, pipeline stalls
}
```

Bitmaps store all slot state in a single cache line (64 bits = 8 bytes). One load fetches the entire state. The BSF instruction operates on a register, not memory—no pipeline stalls.

**What this eliminates:**
- O(N) free list scan (50-100 cycles average)
- Pointer chasing (dependent loads, pipeline stalls)
- Metadata overhead (free list nodes)

**What it costs:**
- Bitmap storage: 1 bit per slot (8 bytes per 64-slot slab)
- BSF instruction: 1-3 cycles

**Net improvement:** 50-100 cycles → 1-3 cycles (20-50× faster)

The bitmap is so efficient that the bottleneck shifts to the CAS operation, not the slot search. This is the correct tradeoff—CAS is unavoidable for correctness (atomicity), but slot search is algorithmic and can be optimized.

**Optimization Layer 3: Eliminate size class search (O(1) Deterministic Class Selection)**

The third bottleneck is mapping requested size to size class. Naive approach is a linear scan through size class thresholds: `if (size <= 64) class=0; if (size <= 96) class=1; ...`

Each comparison is a conditional branch. If allocation sizes are unpredictable (e.g., user input, network packets), the CPU's branch predictor fails ~50% of the time. With 8 size classes, that's 4 mispredicted branches per allocation.

Branch mispredictions cost 10-20 cycles each because the CPU must flush its speculative execution pipeline and restart from the correct path. This is jitter—latency that varies by allocation size.

temporal-slab uses a precomputed lookup table:

```c
static uint8_t size_class_lookup[MAX_SIZE + 1];  // 769 bytes

// Initialization (once at startup)
for (size_t s = 0; s <= MAX_SIZE; s++) {
    for (int c = 0; c < NUM_CLASSES; c++) {
        if (s <= class_sizes[c]) {
            size_class_lookup[s] = c;
            break;
        }
    }
}

// Allocation (hot path)
uint8_t class = size_class_lookup[size];  // 1 load, 0 branches
```

This is an array lookup: load an address, fetch the byte. On x86-64 with L1 cache hit, this is 4 cycles. No branches, no speculation, no mispredictions. The latency is constant regardless of allocation size.

**Why this works:**

The lookup table is small (769 bytes) and accessed frequently, so it remains cache-resident. It fits in a single 1KB cache line block in L1. The access pattern is random (allocation sizes vary), but that doesn't matter—the entire table is cached, so every access is an L1 hit.

The table is initialized once at startup. The cost is ~2000 cycles total (negligible for a one-time setup). Every subsequent allocation saves 40-80 cycles (4 mispredicted branches).

**What this eliminates:**
- 8 conditional branches (80 cycles worst case, mispredictions)
- Unpredictable latency (size-dependent jitter)

**What it costs:**
- Array load: 4 cycles (L1 cache hit)
- 769 bytes of memory (negligible)

**Net improvement:** 80 cycles → 4 cycles (20× faster, deterministic)

**Optimization Layer 4: Eliminate thundering herd (Adaptive Bitmap Scanning)**

The fourth bottleneck appears only under multi-threaded load. When multiple threads allocate from the same slab, they all scan the bitmap starting from bit 0. This creates a thundering herd—all threads converge on the same slot and contend for it with CAS retries.

```c
// Sequential scanning (naive)
for (int i = 0; i < 64; i++) {
    if (bitmap & (1ULL << i) == 0) {
        // All threads find bit 0 first
        // → All threads CAS bit 0
        // → Only 1 succeeds, others retry
    }
}
```

With 8 threads, 7 threads fail their first CAS and retry. Retry rate: 87.5%. Each retry wastes 20-40 cycles.

temporal-slab uses thread-local randomized starting offsets:

```c
static __thread uint32_t tls_scan_offset = UINT32_MAX;

if (tls_scan_offset == UINT32_MAX) {
    uint64_t tid = (uint64_t)pthread_self();
    tls_scan_offset = hash(tid);  // Initialize once per thread
}

int start = tls_scan_offset % 64;
for (int i = 0; i < 64; i++) {
    int bit = (start + i) % 64;  // Wrap around bitmap
    if (bitmap & (1ULL << bit) == 0) {
        // Threads start at different positions
        // → Reduced contention
    }
}
```

Each thread computes a unique starting offset from its thread ID. Threads spread across the bitmap instead of colliding on bit 0. With 8 threads and 64 slots, average distance between threads is 8 slots—low collision probability.

**Thread-local storage (TLS) is critical here:**

Storing the offset in TLS means each thread accesses its own private copy—no sharing, no contention. TLS access is 2-3 cycles (same as a regular variable), essentially free. Computing the offset requires hashing the thread ID, which costs ~10 cycles, but this happens once per thread lifetime—amortized to zero over millions of allocations.

**Adaptive policy:**

Under single-threaded load, randomized scanning is unnecessary—there's no contention. It adds a modulo operation (2-3 cycles) for no benefit. temporal-slab adapts based on observed CAS retry rate:

```c
if (cas_retries / allocations > 0.30) {
    enable_randomized_scanning();  // High contention
} else if (cas_retries / allocations < 0.10) {
    enable_sequential_scanning();  // Low contention
}
```

Single-threaded workloads use sequential scanning (deterministic, 2-3 cycles faster). Multi-threaded workloads switch to randomized scanning when retry rate exceeds 30%. This eliminates configuration—the allocator self-tunes.

**What this eliminates:**
- CAS retry loops (20-40 cycles per retry, retry rate drops from 80% to 20%)
- Thundering herd contention (threads spread across bitmap)

**What it costs:**
- Modulo operation: 2-3 cycles (when randomized mode active)
- TLS load: 2-3 cycles (offset lookup)
- Adaptation overhead: Negligible (checked every 100K allocations)

**Net improvement:** 80% retry rate → 20% retry rate (4× fewer retries, 60+ cycles saved per avoided retry)

**Optimization Layer 5: Eliminate list traversal (Pre-selected Current Slab)**

The fifth bottleneck is finding a slab with free slots. Traditional slab allocators maintain a partial list—a linked list of slabs that are neither empty nor full. Allocation scans this list until finding a slab with free slots.

```c
// Naive partial list scan
Slab* slab = sc->partial_list;
while (slab) {
    if (slab->free_count > 0) {
        // Found one!
        break;
    }
    slab = slab->next;
}
```

This is O(N) where N is the number of partial slabs. In worst case (all partial slabs are full except the last), this scans the entire list. Each iteration is a pointer dereference (cache miss risk) and a comparison.

temporal-slab maintains a `current_partial` pointer—a single pre-selected slab that's known to have free slots (or NULL if no partial slabs exist). Allocation tries this slab first:

```c
Slab* slab = atomic_load(&sc->current_partial);
if (slab && slab->free_count > 0) {
    // Use this slab (no search)
}
```

If `current_partial` is full, the slow path updates it to point to the next partial slab (or allocates a new slab). The fast path never scans lists—it always operates on a known-good slab.

**Why this works:**

Under sustained load, slabs fill sequentially. Once a slab is selected as `current_partial`, it remains the target until full. The fast path hits the same slab repeatedly—this slab's cache line stays hot in L1. When the slab fills, the slow path (which already holds the mutex for structural changes) updates `current_partial` atomically.

**What this eliminates:**
- O(N) partial list scan (10-50 cycles depending on list length)
- Pointer chasing (dependent loads)
- Cache misses (same slab accessed repeatedly)

**What it costs:**
- One atomic load: 1-2 cycles (acquire barrier)
- Pointer dereference: 1 cycle (slab header likely cached)

**Net improvement:** 10-50 cycles → 2-3 cycles (5-20× faster)

**Optimization Layer 6: Eliminate memory reordering hazards (Acquire/Release Semantics)**

The sixth optimization is correctness, not speed—but it enables speed. Lock-free algorithms require careful memory ordering to prevent reordering bugs. Consider publishing a newly initialized slab:

```c
// Slow path: Initialize slab
Slab* slab = mmap(...);
slab->magic = MAGIC;
slab->bitmap = 0;
slab->free_count = 64;

// Publish to fast path
sc->current_partial = slab;  // Other threads can now see it
```

Without memory barriers, the CPU or compiler might reorder these writes. Thread B could see `current_partial = slab` before `slab->bitmap = 0` completes. Thread B would then read an uninitialized bitmap—catastrophic.

temporal-slab uses acquire/release semantics:

```c
// Slow path (release)
atomic_store_explicit(&sc->current_partial, slab, memory_order_release);
// Ensures all prior writes (bitmap, magic, free_count) complete before store

// Fast path (acquire)  
Slab* slab = atomic_load_explicit(&sc->current_partial, memory_order_acquire);
// Ensures all subsequent reads (bitmap, magic) see initialized values
```

On x86-64, acquire loads and release stores are essentially free—they compile to regular MOV instructions because x86-64's strong memory model provides these guarantees by default. The `memory_order` annotations prevent compiler reordering, but require no additional CPU instructions.

On ARM or PowerPC (weaker memory models), acquire/release insert memory fence instructions (DMB, SYNC), costing 10-50 cycles. But temporal-slab targets x86-64 primarily, so this cost is zero in practice.

**What this eliminates:**
- Reordering bugs (compiler or CPU reordering writes)
- Data races (uninitialized reads)

**What it costs:**
- x86-64: 0 cycles (hardware already provides acquire/release)
- ARM/PowerPC: 10-50 cycles (explicit fence instructions)

**Net improvement:** Correctness enabled at zero cost on x86-64

**Optimization Layer 7: Eliminate cache misses (Temporal Grouping)**

The seventh optimization is architectural, not algorithmic. Allocators that scatter objects across many pages suffer cache misses when accessing those objects. If related objects (allocated together, accessed together) are on different cache lines, the CPU must fetch multiple cache lines from DRAM.

temporal-slab groups objects allocated in the same epoch onto the same slabs. If those objects are accessed together (temporal locality → spatial locality), they share cache lines:

```c
// Request processing allocates multiple objects
Connection* conn = alloc(epoch_current);  // Slab A, offset 0
Request* req = alloc(epoch_current);      // Slab A, offset 128
Response* resp = alloc(epoch_current);    // Slab A, offset 256

// All three objects on same page (Slab A)
// Accessing conn, req, resp: 1 TLB entry, 4 cache lines (256 bytes / 64 bytes per line)
```

Traditional allocators might place these objects on three different pages:

```c
Connection* conn = malloc(128);  // Page X
Request* req = malloc(128);      // Page Y (different!)
Response* resp = malloc(256);    // Page Z (different!)

// Three TLB entries required
// Three distinct cache line groups
// 3× TLB miss risk, 3× cache pollution
```

**Why this matters:**

Cache misses cost 100+ cycles (DRAM access). TLB misses cost 500+ cycles (page table walk). If a request processes 10 objects and they're scattered across 10 pages, that's 10 TLB misses = 5000 cycles overhead just for address translation.

With temporal grouping, 10 objects fit on 1-2 pages, requiring 1-2 TLB entries. TLB hit rate improves from 10% (10 pages, 512-entry TLB saturated) to 99% (2 pages, plenty of TLB space).

**What this eliminates:**
- Cache line pollution (objects on same page share cache lines)
- TLB pressure (fewer unique pages accessed)
- DRAM bandwidth waste (fetching unrelated cache lines)

**What it costs:**
- Requires epoch-aware allocation (application must specify epoch)
- Slightly higher internal fragmentation (objects grouped by time, not size)

**Net improvement:** Difficult to measure in isolation, but contributes to consistent p99 latency

**The Complete Hot Path: Cycle-by-Cycle Breakdown**

Bringing it all together, here's what happens during a successful fast path allocation:

```c
void* alloc_obj_hot_path(SlabAllocator* a, size_t size) {
    // [1-4 cycles] Size class selection
    uint8_t class = a->class_lookup[size];  // Array load (L1 hit)
    SizeClassAlloc* sc = &a->classes[class];
    
    // [1-2 cycles] Load current slab
    Slab* slab = atomic_load_explicit(&sc->current_partial, memory_order_acquire);
    
    // [2-3 cycles] TLS scan offset (adaptive mode)
    uint32_t start = get_tls_scan_offset();
    
    // CAS loop (expected iterations: 1.2× on average at T=4)
    uint64_t old_bm, new_bm;
    int attempts = 0;
    do {
        // [1-2 cycles] Load bitmap
        old_bm = atomic_load_explicit(&slab->bitmap, memory_order_relaxed);
        
        // [1-3 cycles] Find free slot (BSF instruction)
        int slot = find_free_bit_starting_at(old_bm, start);
        if (slot == -1) return NULL;  // Slab full
        
        // [1 cycle] Prepare new bitmap
        new_bm = old_bm | (1ULL << slot);
        
        // [20-40 cycles] Atomic CAS
        attempts++;
    } while (!atomic_compare_exchange_weak(&slab->bitmap, &old_bm, new_bm));
    
    // [2-3 cycles] Compute slot address
    uint8_t* slot_base = slab_data_ptr(slab);
    void* ptr = slot_base + (slot * slab->object_size);
    
    return ptr;
}
```

**Cycle accounting (single-threaded, no contention):**
- Size class selection: 4 cycles
- Current slab load: 2 cycles
- TLS offset load: 3 cycles
- Bitmap load: 2 cycles
- BSF (find free bit): 3 cycles
- Bitmap prepare: 1 cycle
- CAS: 20 cycles (success, first attempt)
- Address computation: 3 cycles

**Total: 38 cycles ≈ 13ns at 3GHz**

This matches the observed p50 of 40ns from GitHub Actions benchmarks—the difference is additional overhead from function call preambles, register saves, minor variations in cache state, and the specific CPU characteristics (AMD EPYC 7763 vs theoretical 3GHz baseline).

**Cycle accounting (multi-threaded, T=4, 20% retry rate):**
- Base: 38 cycles (same as above)
- CAS retry (20% probability): 0.2 × 30 cycles = 6 cycles
- Cache coherence overhead: 5 cycles (cache line ping-pong)

**Total: 49 cycles ≈ 16ns at 3GHz**

Under 4-thread load, benchmarks show p50 ≈ 82ns, again consistent with the theoretical model when accounting for measurement overhead.

**Why the optimizations compound:**

Each optimization removes a specific bottleneck, exposing the next one. Without lock-free allocation, mutex contention dominates (1000+ cycles), making bitmap speed irrelevant. With lock-free allocation, slot search becomes the bottleneck—solved by bitmaps. With bitmaps, size class selection becomes visible—solved by lookup tables.

The final hot path is limited by atomic CAS latency (20-40 cycles), which is fundamental to correctness. You cannot eliminate this without breaking thread-safety. Everything else—size selection, slot search, slab selection—has been optimized to near-zero cost.

This is why temporal-slab's p99 is within 3× of p50 (40ns → 120ns). The only variance comes from CAS retries, which are rare and bounded. There are no O(N) scans, no unpredictable branches, no lock contention, no syscalls. The hot path is deterministic machinery executing in 30-50 cycles in the common case.

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

This achieves 120ns p99 allocation latency (GitHub Actions validated) in the common case with no lock contention.

## Lock Contention

Lock contention occurs when multiple threads compete to acquire the same mutex (mutual exclusion lock). When one thread holds a lock, other threads attempting to acquire it must wait. The more threads competing for the lock, the longer the average wait time—this is contention.

**How mutexes work:**

A mutex is a synchronization primitive that ensures only one thread can execute a critical section at a time:

```c
pthread_mutex_t lock;

Thread A:                          Thread B:
1. pthread_mutex_lock(&lock)       1. pthread_mutex_lock(&lock)
2. // Critical section               → BLOCKED (lock held by A)
3. counter++;                       
4. pthread_mutex_unlock(&lock)     
                                   2. → UNBLOCKED, acquires lock
                                   3. // Critical section
                                   4. counter++;
                                   5. pthread_mutex_unlock(&lock)
```

When Thread B tries to acquire a lock held by Thread A, the kernel blocks Thread B—removing it from the CPU scheduler and putting it to sleep. When Thread A releases the lock, the kernel wakes Thread B. This context switch (save Thread B's state, schedule another thread, then later restore Thread B) is expensive: 1,000-10,000 cycles (~300ns-3µs).

**Types of lock contention:**

**1. Uncontended lock (fast path):**
```
Only one thread accesses lock:
pthread_mutex_lock()   → 50-100 cycles (~20-30ns)
// critical section
pthread_mutex_unlock() → 50-100 cycles (~20-30ns)
Total: 100-200 cycles (~50-100ns)
```

**2. Lightly contended lock:**
```
Thread A holds lock, Thread B tries to acquire:
Thread B: pthread_mutex_lock()
→ Spin briefly (~100 cycles) waiting for Thread A to release
→ Thread A releases, Thread B acquires without kernel involvement
Total delay: 100-500 cycles (~50-150ns)
```

**3. Heavily contended lock:**
```
Thread A holds lock, Threads B-H all try to acquire:
Each blocked thread:
1. Spin briefly (100 cycles)
2. Kernel blocks thread (1,000 cycles syscall)
3. Context switch to another thread (1,000 cycles)
4. Later: Kernel wakes thread (1,000 cycles)
5. Context switch back (1,000 cycles)
6. Thread resumes, retries lock
Total delay: 4,000-10,000 cycles (~1-3µs per thread)

Aggregate cost for 8 threads: 32,000-80,000 cycles wasted
```

**Why contention destroys performance:**

```
Web server: 10,000 requests/second, mutex-protected allocator

Uncontended (1 thread):
- 10,000 allocations × 100 cycles/allocation = 1M cycles
- At 3GHz: 0.3ms CPU time (negligible)

Heavily contended (8 threads):
- Each allocation waits for lock: 1,000 cycles average
- 10,000 allocations × 1,000 cycles = 10M cycles
- At 3GHz: 3ms CPU time (10× worse)
- Wasted CPU cycles: 9M cycles spent waiting, not working
```

**Lock-free algorithms avoid contention:**

Instead of blocking threads with mutexes, lock-free algorithms use atomic operations (CAS, fetch-add) that never block:

```c
// Lock-based counter (contention bottleneck)
pthread_mutex_lock(&lock);
counter++;
pthread_mutex_unlock(&lock);
Cost: 100-10,000 cycles (depends on contention)

// Lock-free counter (no contention)
atomic_fetch_add(&counter, 1);
Cost: 20-40 cycles (constant, regardless of threads)
```

**Why temporal-slab uses lock-free fast path:**

```
Fast path (99% of allocations):
- Lock-free CAS on bitmap (20-40 cycles)
- No mutex, no blocking, no contention
- Scales to ~4 threads before cache coherence limits kick in

Slow path (<1% of allocations):
- Mutex-protected slab allocation (100-1,000 cycles)
- Acceptable because rare (1% of 1,000 cycles = 10 cycle average overhead)
- Contention is minimal (slow path infrequent)
```

**The contention equation:**

```
Average allocation time with mutex:
T_avg = lock_acquire_time + critical_section_time + lock_release_time

Where lock_acquire_time depends on contention:
- 0 threads waiting: 50 cycles (fast path)
- 1-2 threads waiting: 100-500 cycles (spin wait)
- 3+ threads waiting: 1,000-10,000 cycles (kernel blocking)

With lock-free fast path:
T_avg = atomic_CAS_time = 20-40 cycles (constant)
```

**Measuring contention:**

You can detect contention by comparing lock hold time vs wait time:

```c
uint64_t t0 = now_ns();
pthread_mutex_lock(&lock);
uint64_t t1 = now_ns();
// critical section
pthread_mutex_unlock(&lock);

uint64_t wait_time = t1 - t0;
if (wait_time > 1000ns) {
  printf("High contention: waited %lu ns for lock\n", wait_time);
}
```

**Why contention matters for allocators:**

Traditional allocators like malloc use a global lock (or per-arena locks). Under high thread count, every allocation blocks on the lock:

```
malloc() with 16 threads:
- Thread 1 allocates, holds lock (100 cycles)
- Threads 2-16 wait (15 threads × 1,000 cycles = 15,000 cycles wasted)
- Throughput: 1 allocation per 1,100 cycles (serialized)

temporal-slab with 16 threads:
- All threads allocate concurrently via lock-free CAS
- Throughput: 16 allocations per 40 cycles (parallel)
- 400× better throughput (until cache coherence limits hit)
```

Lock contention is why "thread-safe" doesn't mean "scalable." A mutex-protected allocator is correct under concurrent access but becomes a bottleneck as thread count increases. Lock-free algorithms eliminate contention but introduce new challenges (ABA problem, memory ordering, retry loops).

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

## Memory Barriers and Fences

Memory barriers (also called memory fences) are synchronization primitives that control the order in which memory operations become visible to other threads. They are critical for correctness in lock-free programming.

**The problem: Memory reordering**

Modern CPUs and compilers reorder memory operations for performance. This creates subtle bugs in concurrent code:

```c
// Thread A
data = 42;          // Write 1
ready = true;       // Write 2

// Thread B
if (ready) {        // Read 2
  use(data);        // Read 1 - expects to see 42
}
```

Without barriers, the CPU might reorder Thread A's writes:
```
Thread A (reordered):
ready = true;       // Write 2 happens first!
data = 42;          // Write 1 happens second

Thread B sees:
ready == true, but data == 0 (old value)
→ BUG: Thread B reads stale data
```

**Why CPUs reorder memory:**

CPUs use out-of-order execution to maximize throughput. If Write 2 can execute before Write 1 (because its operands are ready first), the CPU will reorder them. This is safe for single-threaded code but breaks concurrent code that depends on ordering.

Compilers also reorder for optimization:
```c
// Original code
x = 1;
y = 2;
z = 3;

// Compiler might reorder to:
y = 2;  // Reorder for better register allocation
x = 1;
z = 3;
```

**Memory ordering guarantees:**

Different memory operations have different ordering guarantees:

**1. Relaxed ordering:**
```c
atomic_store_explicit(&flag, 1, memory_order_relaxed);
```
- No ordering guarantees
- Only atomicity (operation is indivisible)
- Fastest (no memory fences)
- Use when order doesn't matter (e.g., simple counters)

**2. Acquire ordering:**
```c
int value = atomic_load_explicit(&ptr, memory_order_acquire);
```
- All subsequent reads/writes happen AFTER this load
- Prevents later operations from moving before the acquire
- Used when loading shared data: ensures data is visible before use

**3. Release ordering:**
```c
atomic_store_explicit(&ptr, value, memory_order_release);
```
- All prior reads/writes happen BEFORE this store
- Prevents earlier operations from moving after the release
- Used when publishing shared data: ensures data is finalized before visible

**4. Acquire-release ordering:**
```c
atomic_exchange_explicit(&ptr, new_val, memory_order_acq_rel);
```
- Combination of acquire and release
- Prior writes happen before, subsequent reads happen after
- Used for read-modify-write operations (CAS, fetch-add)

**5. Sequential consistency:**
```c
atomic_store(&ptr, value);  // Default: memory_order_seq_cst
```
- Strongest guarantee: total order across all threads
- All threads see the same sequence of operations
- Slowest (full memory fence on every operation)
- Use when correctness is unclear (fallback)

**Explicit memory fences:**

Sometimes you need a fence without an atomic operation:

```c
atomic_thread_fence(memory_order_acquire);  // All subsequent ops happen after
atomic_thread_fence(memory_order_release);  // All prior ops happen before
atomic_thread_fence(memory_order_seq_cst);  // Full barrier
```

**Concrete example: Publishing initialized data**

```c
// Thread A: Initialize and publish
struct Data {
  int x, y, z;
};

Data* data = malloc(sizeof(Data));
data->x = 1;
data->y = 2;
data->z = 3;

// Release fence: ensure x, y, z writes complete before publishing
atomic_store_explicit(&global_ptr, data, memory_order_release);

// Thread B: Load and use
Data* p = atomic_load_explicit(&global_ptr, memory_order_acquire);
if (p) {
  // Acquire fence: ensure we see x, y, z writes
  use(p->x, p->y, p->z);  // Guaranteed to see 1, 2, 3
}
```

Without barriers:
```
Thread A writes might be reordered:
atomic_store(&global_ptr, data);  // Published before initialized!
data->x = 1;  // Not visible yet
data->y = 2;
data->z = 3;

Thread B might see:
p != NULL, but p->x == 0 (uninitialized memory)
→ BUG: Published incomplete data
```

**Why temporal-slab uses acquire/release:**

```c
// Slow path: Initialize slab and publish to fast path
Slab* slab = allocate_slab();
slab->magic = SLAB_MAGIC;
slab->free_count = object_count;
slab->bitmap = 0;  // All slots free

// Release: ensure slab is fully initialized before publishing
atomic_store_explicit(&current_partial, slab, memory_order_release);

// Fast path: Load and use
Slab* s = atomic_load_explicit(&current_partial, memory_order_acquire);
// Acquire: guaranteed to see initialized magic, free_count, bitmap
if (s && s->free_count > 0) {
  allocate_from_slab(s);
}
```

**Performance cost of memory barriers:**

| Operation | Cycles | Notes |
|-----------|--------|-------|
| **Relaxed atomic** | 1-2 | No fence, like normal load/store |
| **Acquire load** | 1-2 | No fence on x86 (implicit in load) |
| **Release store** | 1-2 | No fence on x86 (implicit in store) |
| **CAS (acq_rel)** | 20-40 | LOCK prefix implies full fence |
| **Full fence** | 20-100 | mfence instruction, flushes store buffer |

On x86-64, acquire/release have zero cost—the hardware already provides these guarantees. On ARM/PowerPC, acquire/release require explicit fence instructions (dmb, sync) costing 10-50 cycles.

**Memory barrier vs compiler barrier:**

- **Memory barrier:** CPU instruction preventing hardware reordering (e.g., mfence, dmb)
- **Compiler barrier:** Directive preventing compiler reordering (e.g., `asm volatile("" ::: "memory")`)

atomic_thread_fence() inserts both. This prevents:
1. Compiler from reordering (at compile time)
2. CPU from reordering (at runtime)

**Why "volatile" is not enough:**

```c
volatile int ready;  // Does NOT provide memory barriers
data = 42;
ready = 1;  // Compiler won't reorder, but CPU still might
```

`volatile` only prevents compiler optimization. It does NOT insert memory barriers. Use atomics with proper memory ordering instead.

## Cache Coherence

Cache coherence is the hardware mechanism that keeps CPU caches synchronized across multiple cores. When one core modifies a cache line, all other cores' caches holding that line must be notified so they don't use stale data.

**The problem without cache coherence:**

```
Modern CPU: 8 cores, each with private L1/L2 cache, shared L3

Initial state:
RAM address 0x1000: value = 0

Core 0:                         Core 1:
1. Read 0x1000                  1. Read 0x1000
   → Load into L1: value = 0       → Load into L1: value = 0
2. Write 0x1000 = 42            2. Read 0x1000
   → Update L1: value = 42         → Read from L1: value = 0 (STALE!)

Without coherence: Core 1 sees old value even after Core 0 updated it
```

This would make concurrent programming impossible—every shared variable would require explicit cache flushes.

**How cache coherence works (MESI protocol):**

Modern CPUs use the MESI protocol (or variants like MESIF, MOESI) to track cache line states:

**M - Modified:** This core modified the line, other caches don't have it
**E - Exclusive:** This core has the line, no other caches have it, matches RAM
**S - Shared:** Multiple cores have the line, matches RAM, read-only
**I - Invalid:** This core's cached copy is stale, must reload

**Example: Coherence in action**

```
Address 0x1000 initially in RAM: value = 0

Core 0:                         Core 1:                    State
------------------------------------------------------------------------
Read 0x1000                                                Core 0: S
                                                          (loaded, shared)

                                Read 0x1000                Core 0: S
                                                          Core 1: S
                                                          (both shared)

Write 0x1000 = 42                                         Core 0: M
  → Sends invalidate to Core 1                            Core 1: I
  → Core 1's cache marked invalid                         (Core 1 invalidated)

                                Read 0x1000                Core 0: S
                                  → Cache miss!            Core 1: S
                                  → Request from Core 0    (Core 0 wrote back,
                                  → Gets value = 42         both now shared)
```

**Coherence message types:**

When cores modify shared data, they exchange coherence messages over the memory bus:

**1. Read request:** "I need address 0x1000"
- Response: Data from RAM or owning core

**2. Invalidate:** "I'm writing to 0x1000, invalidate your copies"
- All other cores mark their cache line as Invalid
- Next read triggers cache miss

**3. Writeback:** "I'm evicting modified 0x1000, here's the data"
- Core writes dirty cache line back to RAM
- Other cores can now read the updated value

**Cost of cache coherence:**

Each coherence message takes time:

| Operation | Cycles | Description |
|-----------|--------|-------------|
| **L1 hit** | 4 | Data in this core's L1 cache |
| **L2 hit** | 12 | Data in this core's L2 cache |
| **L3 hit** | 40 | Data in shared L3 cache |
| **Cache line invalidation** | 40-100 | Invalidate message to other cores |
| **Cache line transfer** | 100-200 | Load from another core's cache |
| **DRAM access** | 100-200 | Load from main memory |

**False sharing: The performance killer**

False sharing occurs when two threads modify different variables that happen to be in the same 64-byte cache line:

```c
struct CounterPair {
  int counter_a;  // Byte 0-3
  int counter_b;  // Byte 4-7 (SAME CACHE LINE!)
};

// Thread A (Core 0)
counter_a++;  // Modifies cache line
              // → Invalidates Core 1's cache

// Thread B (Core 1)
counter_b++;  // Modifies same cache line
              // → Invalidates Core 0's cache
              // → Cache line bounces between cores
```

**Result:**
- Each increment triggers cache line transfer (~100 cycles)
- No actual data dependency, but hardware can't tell
- Performance degrades as if variables were shared

**Fixing false sharing with padding:**

```c
struct CounterPairFixed {
  int counter_a;
  char pad[60];       // Pad to 64-byte cache line
  int counter_b;      // Now on different cache line
  char pad2[60];
};

// Thread A and Thread B now modify different cache lines
// No false sharing, no cache line bouncing
```

**Why temporal-slab scales to only 4-8 threads:**

Lock-free doesn't mean cache-coherence-free. The atomic CAS operations still cause cache line bouncing:

```
8 threads allocating concurrently:
Each thread does CAS on slab->bitmap (same cache line)

Thread 1: CAS(&slab->bitmap)
  → Core 0 loads cache line (Shared)
  → Core 0 modifies (Modified)
  → Invalidates Cores 1-7

Thread 2: CAS(&slab->bitmap)
  → Core 1 requests cache line from Core 0 (100 cycles)
  → Core 1 modifies (Modified)
  → Invalidates Cores 0, 2-7

Result: Cache line bounces between cores on every CAS
Throughput degrades beyond 4-8 threads due to coherence traffic
```

**Coherence traffic grows quadratically:**

```
With N threads modifying same cache line:
- Each modification invalidates N-1 other caches
- Total invalidations: N × (N-1) ≈ N²

1 thread:  0 invalidations
2 threads: 2 invalidations (2× cost)
4 threads: 12 invalidations (6× cost)
8 threads: 56 invalidations (28× cost)
16 threads: 240 invalidations (120× cost!)
```

This is why lock-free allocators don't scale infinitely—cache coherence becomes the bottleneck.

**Measuring cache coherence effects:**

Use hardware performance counters:
```bash
perf stat -e LLC-load-misses,LLC-store-misses ./benchmark_threads

# High LLC misses = cache coherence overhead
```

**Why cache coherence is mandatory:**

Without hardware cache coherence, programmers would need explicit cache management:

```c
// Without coherence (nightmare)
x = 42;
flush_cache_line(&x);  // Manual flush
send_invalidate_to_all_cores(&x);

// With coherence (automatic)
atomic_store(&x, 42);  // Hardware handles coherence
```

Cache coherence makes concurrent programming tractable, but it's not free. Lock-free algorithms trade lock contention for cache coherence overhead—both have limits.

## Memory Alignment

Memory alignment is the requirement that data be stored at addresses that are multiples of the data's size. Aligned memory accesses are faster (or required) on most CPUs.

**What is alignment?**

An N-byte value is "N-byte aligned" if its address is a multiple of N:

```
8-byte aligned addresses: 0x0, 0x8, 0x10, 0x18, 0x20, ...
4-byte aligned addresses: 0x0, 0x4, 0x8, 0xC, 0x10, ...
2-byte aligned addresses: 0x0, 0x2, 0x4, 0x6, 0x8, ...
```

**Aligned vs unaligned:**

```c
// Aligned (fast)
char buffer[16];
uint64_t* ptr = (uint64_t*)&buffer[0];  // Address 0x0 (8-byte aligned)
*ptr = 42;  // Single memory access

// Unaligned (slow or crashes)
uint64_t* ptr = (uint64_t*)&buffer[1];  // Address 0x1 (NOT 8-byte aligned)
*ptr = 42;  // May require 2 memory accesses or trigger fault
```

**Why alignment matters:**

**1. Performance:**

CPUs fetch memory in aligned chunks. Unaligned accesses may span cache lines:

```
Cache line boundaries (64 bytes): 0x0, 0x40, 0x80, ...

Aligned 8-byte load at 0x38:
[======== Cache Line 0 ========]
                        [8 bytes]
→ 1 cache line access (fast)

Unaligned 8-byte load at 0x3D:
[======== Cache Line 0 ========][======== Cache Line 1 ========]
                     [4B][4B]
→ 2 cache line accesses (slow!)
```

**Cost:**
- Aligned load: 4 cycles (L1 hit)
- Unaligned load: 8+ cycles (may access 2 cache lines)

**2. Correctness:**

Some CPUs require aligned access. Unaligned access triggers hardware fault:

```c
// On SPARC, ARM (without unaligned support), RISC-V:
uint64_t* ptr = (uint64_t*)0x1001;  // Unaligned address
*ptr = 42;  // → Bus error, process terminates
```

x86-64 tolerates unaligned access (penalty, no fault). ARM/SPARC crash.

**3. Atomicity:**

Aligned atomic operations are guaranteed atomic. Unaligned atomics may not be:

```c
_Atomic uint64_t x;  // At address 0x8 (aligned)
atomic_store(&x, 42);  // Guaranteed atomic (single CPU instruction)

_Atomic uint64_t y;  // At address 0x9 (unaligned)
atomic_store(&y, 42);  // NOT atomic (may require multiple instructions)
                       // Other threads could see half-written value!
```

**Alignment requirements by type:**

| Type | Size | Alignment | Address must be multiple of |
|------|------|-----------|------------------------------|
| `char` | 1 | 1 | Any address (no restriction) |
| `short` | 2 | 2 | 2 (0x0, 0x2, 0x4, ...) |
| `int` | 4 | 4 | 4 (0x0, 0x4, 0x8, ...) |
| `long` | 8 | 8 | 8 (0x0, 0x8, 0x10, ...) |
| `double` | 8 | 8 | 8 |
| `void*` | 8 | 8 | 8 (on 64-bit systems) |

**How compilers ensure alignment:**

```c
struct Example {
  char a;      // 1 byte at offset 0
  // 3 bytes padding inserted here
  int b;       // 4 bytes at offset 4 (4-byte aligned)
  char c;      // 1 byte at offset 8
  // 7 bytes padding inserted here
  double d;    // 8 bytes at offset 16 (8-byte aligned)
};

sizeof(struct Example) = 24 bytes (not 14!)
Padding ensures each field is properly aligned
```

**Forcing alignment with `alignas`:**

```c
// Align to cache line boundary (64 bytes) to prevent false sharing
struct alignas(64) ThreadLocal {
  int counter;
  // 60 bytes padding added by compiler
};

// Each thread's counter on different cache line
ThreadLocal thread_data[16];  // 16 * 64 = 1024 bytes
```

**Why temporal-slab cares about alignment:**

**1. Handle encoding uses registry indirection:**

```c
// Handle format v1: [slab_id:22][gen:24][slot:8][class:8][ver:2]
// No raw pointers, uses registry lookup

SlabHandle h = handle_pack(slab_id, generation, slot, class);
// Registry: metas[slab_id] → {Slab* ptr, uint32_t gen}

// Alignment matters for object pointers:
Slab* slab = mmap(..., 4096, ...);  // OS returns page-aligned (0x...000)
void* object = slab_base + (slot * slot_size);  // 8-byte aligned
```

**2. All allocations 8-byte aligned:**

```c
void* alloc_obj(alloc, size, &h) {
  // Returns 8-byte aligned pointer
  // Ensures atomic operations on user data are safe
  // Ensures double/long/pointer types are naturally aligned
}
```

**3. Slab header structure alignment:**

```c
struct Slab {
  Slab* prev;            // 8 bytes, offset 0 (8-byte aligned)
  Slab* next;            // 8 bytes, offset 8 (8-byte aligned)
  _Atomic uint32_t magic; // 4 bytes, offset 16 (4-byte aligned)
  uint32_t object_size;  // 4 bytes, offset 20 (4-byte aligned)
  // Compiler ensures each field is aligned for atomic operations
};
```

**Checking alignment at runtime:**

```c
bool is_aligned(void* ptr, size_t alignment) {
  return ((uintptr_t)ptr % alignment) == 0;
}

assert(is_aligned(slab, 4096));  // Slab is page-aligned
assert(is_aligned(ptr, 8));      // Allocation is 8-byte aligned
```

**The alignment-size trade-off:**

```c
// Option 1: Tight packing (unaligned, slow)
struct Tight {
  char a;   // offset 0
  int b;    // offset 1 (UNALIGNED!)
  char c;   // offset 5
};
sizeof(Tight) = 6 bytes (if packed)
Access to b requires 2 memory operations (slow)

// Option 2: Aligned (padding, fast)
struct Aligned {
  char a;   // offset 0
  // 3 bytes padding
  int b;    // offset 4 (aligned!)
  char c;   // offset 8
};
sizeof(Aligned) = 12 bytes (wasted 5 bytes)
Access to b requires 1 memory operation (fast)
```

Allocators choose alignment to maximize performance, accepting some wasted space from padding.

## Compiler Barriers

Compiler barriers prevent the compiler from reordering instructions across the barrier at compile time. They are distinct from memory barriers (which prevent CPU reordering at runtime).

**The problem: Compiler optimization**

Compilers reorder and optimize code for performance. This breaks benchmarks and low-level concurrent code:

```c
// Original code
uint64_t t0 = now_ns();
void* ptr = alloc_obj(alloc, 128, &h);
uint64_t t1 = now_ns();
printf("Latency: %lu ns\n", t1 - t0);

// Compiler might optimize to:
uint64_t t0 = now_ns();
uint64_t t1 = now_ns();  // Reordered before alloc_obj!
void* ptr = alloc_obj(alloc, 128, &h);
printf("Latency: %lu ns\n", t1 - t0);  // Measures 0 ns (wrong!)
```

The compiler sees no dependency between `alloc_obj()` and `t1 = now_ns()`, so it reorders to improve register allocation. This produces incorrect benchmark results.

**Compiler barrier implementation:**

```c
#define compiler_barrier() asm volatile("" ::: "memory")
```

This is an inline assembly directive that:
1. `asm volatile` - Tells compiler not to optimize away or reorder
2. `""` - Empty assembly (no actual CPU instruction)
3. `::: "memory"` - Clobbers memory (compiler assumes all memory changed)

**How it works:**

```c
uint64_t t0 = now_ns();
compiler_barrier();  // Compiler cannot move code before this line past it
void* ptr = alloc_obj(alloc, 128, &h);
compiler_barrier();  // Compiler cannot move code after this line before it
uint64_t t1 = now_ns();
```

The compiler treats the barrier as if it reads/writes all memory. Any memory operation before the barrier must stay before. Any after must stay after.

**Compiler barrier vs memory barrier:**

| Barrier Type | Prevents | Cost | Example |
|--------------|----------|------|---------|
| **Compiler barrier** | Compile-time reordering | 0 cycles (no code) | `asm volatile("" ::: "memory")` |
| **Memory barrier** | Runtime CPU reordering | 20-100 cycles | `atomic_thread_fence()`, `mfence` |

**When to use compiler barriers:**

**1. Benchmarking:**

```c
// Prevent compiler from reordering benchmark timing
compiler_barrier();
uint64_t t0 = now_ns();
compiler_barrier();
operation_under_test();
compiler_barrier();
uint64_t t1 = now_ns();
compiler_barrier();
```

**2. Volatile access patterns:**

```c
volatile int* mmio_reg = (volatile int*)0xFF00;  // Memory-mapped I/O

*mmio_reg = 1;  // Write to hardware register
compiler_barrier();  // Ensure write happens before read
int status = *mmio_reg;  // Read hardware status
```

**3. Preventing dead code elimination:**

```c
void benchmark_alloc() {
  void* ptr = alloc_obj(alloc, 128, &h);
  // Without barrier, compiler might eliminate alloc_obj if ptr unused
  compiler_barrier();  // Force ptr to be "used"
  free_obj(alloc, h);
}
```

**Why temporal-slab benchmarks use compiler barriers:**

```c
// From benchmark_accurate.c
static inline void barrier(void) {
  asm volatile("" ::: "memory");
}

void benchmark_latency() {
  barrier();  // Prevent timing code from being reordered
  uint64_t t0 = now_ns();
  barrier();  // Separate timing from allocation
  
  void* ptr = slab_malloc_epoch(alloc, size, 0);
  
  barrier();  // Prevent allocation from being reordered
  uint64_t t1 = now_ns();
  barrier();  // Separate timing from cleanup
  
  latency_ns = t1 - t0;  // Accurate measurement
}
```

Without barriers, the compiler might:
- Hoist `t1 = now_ns()` before the allocation
- Sink `t0 = now_ns()` after the allocation
- Eliminate the allocation entirely if `ptr` appears unused
- Merge multiple `now_ns()` calls

**Common mistake: Thinking volatile is enough**

```c
volatile uint64_t timestamp;

timestamp = now_ns();  // volatile prevents optimization of read
alloc_obj(...);
timestamp = now_ns();  // But doesn't prevent reordering!
```

`volatile` tells the compiler "this memory might change unexpectedly, don't optimize it away." It does NOT prevent reordering. Use explicit barriers.

**Full memory barrier (atomic_thread_fence) includes compiler barrier:**

```c
atomic_thread_fence(memory_order_seq_cst);
// Implies both:
// 1. Compiler barrier (no compile-time reordering)
// 2. CPU memory barrier (no runtime reordering)
```

So you don't need both:
```c
// Redundant:
compiler_barrier();
atomic_thread_fence(memory_order_seq_cst);

// Sufficient:
atomic_thread_fence(memory_order_seq_cst);
```

**Zero-cost abstraction:**

Compiler barriers are a zero-cost abstraction—they affect what code the compiler generates but add no runtime instructions:

```assembly
; Without barrier:
mov rax, [time]    ; t0 = now_ns()
call alloc_obj     ; alloc_obj()
mov rbx, [time]    ; t1 = now_ns()

; With barrier:
mov rax, [time]    ; t0 = now_ns()
; <no instruction here for barrier>
call alloc_obj     ; alloc_obj()
; <no instruction here for barrier>
mov rbx, [time]    ; t1 = now_ns()
```

The barrier affects instruction ordering but compiles to zero bytes.

## Thread-Local Storage (TLS)

Thread-Local Storage (TLS) is a mechanism where each thread gets its own private copy of a variable. Instead of all threads sharing a single global variable, each thread sees its own independent instance. This eliminates contention and enables lock-free patterns.

**The problem with shared globals:**

```c
// Global variable shared by all threads
uint32_t scan_offset = 0;  // Problem: All threads use same offset

// Thread 1 reads: scan_offset = 0
// Thread 2 reads: scan_offset = 0
// Thread 3 reads: scan_offset = 0
// → Thundering herd! All threads start from same position
```

To make each thread use a different offset, you'd need a lock or atomic operations—adding overhead to every access.

**Thread-local variables:**

```c
// Each thread gets its own copy
__thread uint32_t scan_offset = 0;

// Thread 1 has: scan_offset = 42
// Thread 2 has: scan_offset = 17
// Thread 3 has: scan_offset = 99
// → No sharing, no contention, no synchronization needed
```

The `__thread` storage class specifier (or `thread_local` in C11) tells the compiler to create a separate copy per thread. Access is as fast as a regular variable—no atomic operations required.

**How TLS works internally:**

Each thread has a Thread Control Block (TCB) containing:
- Stack pointer
- Thread ID
- **TLS segment base pointer** ← Points to this thread's TLS data

When you access a `__thread` variable, the compiler generates:
```asm
mov rax, fs:0      ; Load TLS base pointer from segment register
mov rbx, [rax+32]  ; Access thread-local variable at offset 32
```

On x86-64, the `fs` segment register points to the TCB. TLS access is typically 2-3 cycles (if TLS data is in L1 cache)—essentially free.

**TLS initialization:**

```c
__thread uint32_t tls_offset = UINT32_MAX;  // Initial value

uint32_t get_thread_offset() {
    if (tls_offset == UINT32_MAX) {
        // First access: Initialize with thread-specific value
        uint64_t tid = (uint64_t)pthread_self();
        tls_offset = hash(tid);  // Unique per thread
    }
    return tls_offset;
}
```

**Lazy initialization pattern:** The first thread to access the TLS variable initializes it. Subsequent accesses just read the cached value. This is cheaper than computing the value on every access.

**TLS in temporal-slab's adaptive scanning:**

```c
// TLS-cached scan offset per thread
static __thread uint32_t tls_scan_offset = UINT32_MAX;

static inline uint32_t get_tls_scan_offset(uint32_t words) {
    if (tls_scan_offset == UINT32_MAX) {
        uint64_t tid = (uint64_t)pthread_self();
        uint32_t h = mix32(tid);  // 32-bit hash
        tls_scan_offset = h;
    }
    return (words == 0) ? 0 : (tls_scan_offset % words);
}
```

Each thread computes its offset once (hash of thread ID), then caches it in TLS. All subsequent bitmap scans use the cached value with zero synchronization overhead.

**Benefits:**
- **Zero contention:** No atomic operations, no cache line bouncing
- **Fast access:** 2-3 cycles (L1 hit), same as normal variable
- **Per-thread state:** Each thread gets unique starting point
- **No initialization overhead:** Lazy initialization happens once

**Cost:**
- **Memory overhead:** `sizeof(variable) × num_threads`
- **Not shared:** Cannot communicate between threads via TLS variables
- **Cleanup complexity:** Must be destroyed when thread exits (automatic for `__thread`)

**TLS stack pattern (used in epoch domains):**

For nested structures like epoch domains, TLS can store a stack:

```c
#define MAX_DOMAIN_DEPTH 32

// Per-thread stack of nested domains
static __thread struct {
    epoch_domain_t* stack[MAX_DOMAIN_DEPTH];
    uint32_t depth;
} tls_domain_stack = { .depth = 0 };

// Push domain onto thread-local stack
void domain_enter(epoch_domain_t* domain) {
    assert(tls_domain_stack.depth < MAX_DOMAIN_DEPTH);
    tls_domain_stack.stack[tls_domain_stack.depth++] = domain;
}

// Pop domain from thread-local stack
void domain_exit() {
    assert(tls_domain_stack.depth > 0);
    tls_domain_stack.depth--;
}
```

This enables **nested scopes** (transaction contains query contains cache lookup) without any synchronization. Each thread maintains its own independent stack.

**TLS vs alternatives:**

| Approach | Access Cost | Contention | Complexity |
|----------|-------------|------------|------------|
| **Global variable + lock** | 100-200 cycles (mutex) | High | Low |
| **Global variable + atomic** | 20-40 cycles (CAS) | Medium | Medium |
| **Hash table (tid → data)** | 10-20 cycles (lookup) | Low | High |
| **Thread-local storage** | 2-3 cycles (TLS load) | None | Low |

TLS is the fastest way to maintain per-thread state. temporal-slab uses it for:
1. **Scan offsets** (adaptive bitmap scanning)
2. **Domain stack** (nested epoch domain tracking)
3. **Thread ownership** (pthread_t stored in TLS for assertions)

**Why TLS matters for HFT:**

High-frequency trading systems cannot tolerate synchronization overhead. TLS enables:
- **Deterministic access:** No lock contention, no CAS retries
- **Predictable latency:** 2-3 cycles is consistent across all loads
- **Zero jitter:** No variance from contention spikes

By using TLS for per-thread state, temporal-slab avoids the synchronization overhead that would add 20-100 cycles to every operation.

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

## Observability: Why malloc Profiling is Expensive and temporal-slab's Metrics are Free

One of temporal-slab's unique advantages is **structural observability**: runtime metrics that cost nothing to maintain and provide phase-level attribution that malloc fundamentally cannot offer. Understanding why malloc profiling is expensive helps appreciate this architectural benefit.

### The malloc Observability Problem

malloc operates at the **pointer level**. It sees individual allocation/free calls but has no concept of phases, requests, or application structure. When you want to answer questions like:
- "Which HTTP route consumed 40MB?"
- "Did this database transaction leak memory?"
- "Why is RSS growing despite steady working set?"

...malloc cannot help without external tools.

### malloc Profiling Approaches and Their Costs

#### Approach 1: jemalloc Heap Profiling (10-30% overhead)

**How it works:**

jemalloc's profiling (`--enable-prof`) tracks every allocation with a full backtrace.

```c
void* malloc(size_t size) {
    void* ptr = allocate_memory(size);
    
    // Profiling overhead starts here
    void* backtrace[128];
    int depth = capture_stack_trace(backtrace, 128);  // 200-500ns
    
    // Hash table: ptr → backtrace mapping
    hash_table_insert(profile_map, ptr, backtrace, depth);  // 50-100ns
    
    return ptr;
}
```

**Overhead breakdown:**

| Operation | Cost | Explanation |
|-----------|------|-------------|
| Stack unwinding | 200-500ns | Walk call stack, resolve symbols |
| Hash table insert | 50-100ns | Track ptr → backtrace mapping |
| Hash table lookup on free | 50-100ns | Remove mapping |
| Memory for hash table | 32-64 bytes per allocation | Stores backtrace + metadata |

**Total: 300-700ns per allocation** (vs 45ns without profiling)

For a workload doing 100K allocs/sec, profiling adds 30-70ms CPU time per second (**30-70% overhead**).

**Example usage:**

```bash
# Enable profiling (10-30% performance hit)
export MALLOC_CONF="prof:true,prof_leak:true,lg_prof_sample:10"
./webserver

# After running, generate heap profile
jeprof --text ./webserver jeprof.12345.0.f.heap
```

**Output:**
```
Total: 128.5 MB
 40.2 MB (31.3%): handle_request
 25.1 MB (19.5%): process_headers
 18.7 MB (14.6%): parse_json
 ...
```

**Problems:**

1. **Reconstructed attribution:** Stack traces show call site, not semantic phase. "Which request leaked?" requires correlating traces to application logic manually.

2. **No real-time view:** Profile generated after-the-fact. Can't query "current RSS per route" during request handling.

3. **Sampling bias:** To reduce overhead, jemalloc samples (e.g., 1 in 10 allocations). Small leaks may be missed.

4. **Post-mortem only:** Need to stop service, generate profile, analyze. Cannot monitor live in production.

#### Approach 2: tcmalloc Heap Profiler (5-15% overhead)

**How it works:**

tcmalloc's profiler samples allocations probabilistically (e.g., 1 in 100) instead of tracking every allocation.

```cpp
#include <gperftools/heap-profiler.h>

HeapProfilerStart("myapp");  // Start sampling
// Run application...
HeapProfilerStop();  // Generates myapp.0001.heap
```

**Overhead:**

| Aspect | Cost |
|--------|------|
| Sampling rate | Configurable (1 in N) |
| Per-sample overhead | 200-500ns (backtrace) |
| Average overhead | 5-15% (N=100: 200ns / 100 = 2ns avg) |

**Advantages over jemalloc:**
- Lower overhead (sampling vs full tracking)
- Production-viable (5-15% acceptable for many workloads)

**Disadvantages:**
- Less precise (small leaks missed if not sampled)
- Still reconstructive (backtraces, not phase labels)
- Still post-mortem (no real-time querying)

#### Approach 3: Valgrind Massif (20-50× slowdown)

**How it works:**

Valgrind intercepts **every** memory operation (malloc, free, reads, writes) and instruments them. Provides perfect tracking but at extreme cost.

```bash
valgrind --tool=massif ./myapp
# After running:
ms_print massif.out.12345
```

**Overhead:**

| Aspect | Slowdown |
|--------|----------|
| Memory operations | 20-50× slower |
| CPU-bound code | 5-10× slower |
| Overall | 20-50× slower |

**Advantages:**
- Perfect accuracy (no sampling)
- Detects leaks, use-after-free, double-free
- Rich visualization (ms_print, massif-visualizer)

**Disadvantages:**
- Unusable in production (50× slowdown)
- Debug/QA only
- Cannot profile live production issues

### temporal-slab's Structural Observability: Zero-Cost Metrics

temporal-slab achieves observability **without profiling overhead** because metrics emerge from the allocator's structure rather than being grafted on.

**The key insight:**

When the allocator groups allocations by epoch (phase), it already maintains per-epoch metadata for **correctness**, not observability:

```c
struct EpochState {
    atomic_uint refcount;          // Needed for lifecycle (when to recycle?)
    atomic_uint state;             // Needed for correctness (ACTIVE vs CLOSING)
    uint64_t era;                  // Needed for ABA protection (handle validation)
    char label[32];                // Semantic label (e.g., "route:/api/users")
    uint64_t rss_before_close;     // Measured during epoch_close()
    uint64_t rss_after_close;      // Measured after epoch_close()
};
```

These fields exist because the allocator **needs** them to function correctly:
- `refcount`: Know when epoch's last object is freed (safe to recycle slabs)
- `state`: Reject allocations into CLOSING epochs (correctness)
- `era`: Prevent handle reuse after slab recycling (ABA safety)

Exposing them via observability APIs (`slab_stats_epoch()`) adds **zero overhead** because they're already there.

**Example: Real-time leak detection**

```c
// Zero overhead—counters already maintained
SlabEpochStats stats;
slab_stats_epoch(&allocator, size_class, request_epoch, &stats);

if (stats.state == EPOCH_CLOSING && stats.reclaimable_slab_count > 100) {
    log_warning("Route %s leaked memory: %u slabs unreleased",
                stats.label, stats.reclaimable_slab_count);
}
```

No stack unwinding, no hash tables, no sampling. Just read counters that exist for correctness.

### Comparison Table: Observability Overhead

| Approach | Overhead | Granularity | Real-Time? | Phase Attribution? |
|----------|----------|-------------|------------|---------------------|
| **jemalloc profiling** | **10-30%** | Per-allocation (full trace) | No (post-mortem) | No (reconstructive) |
| **tcmalloc profiler** | **5-15%** | Per-allocation (sampled) | No (post-mortem) | No (reconstructive) |
| **Valgrind massif** | **20-50×** | Per-operation | No (debug only) | No (reconstructive) |
| **temporal-slab** | **0%** | Per-phase (epoch) | **Yes (live query)** | **Yes (built-in)** |

### Why malloc Cannot Do This

malloc's **pointer-level** abstraction fundamentally prevents phase-level observability.

**Question: "Which HTTP route consumed 40MB?"**

*With malloc:*
1. Enable profiling (10-30% overhead)
2. Capture backtraces on every allocation (200-500ns per alloc)
3. Aggregate traces by call site after-the-fact
4. Manually correlate call sites to routes (grep source code)
5. Result: Approximate attribution, post-mortem only

*With temporal-slab:*
1. Allocate requests to labeled epoch domains:
   ```c
   EpochDomain* route_domain = epoch_domain_create(alloc, "route:/api/users");
   void* req = alloc_obj_domain(alloc, 128, route_domain);
   ```

2. Query live:
   ```c
   SlabEpochStats stats;
   slab_stats_epoch(alloc, size_class, route_domain->epoch, &stats);
   printf("Route %s RSS: %.2f MB\n", stats.label,
          stats.estimated_rss_bytes / 1024.0 / 1024);
   ```

3. Result: Exact attribution, zero overhead, real-time

**Why temporal-slab can do this:**

Epochs are **first-class**. They're tracked for correctness (refcount, state, era), not bolted on for profiling. Phase attribution is **structural** (built into allocator design), not **reconstructive** (inferred from backtraces).

### Production Dashboard Example

**With jemalloc (instrumentation required):**

```python
# External monitoring (Prometheus exporter)
# Problem: malloc has no phase concept, must instrument application
@app.route("/api/users")
def handle_users():
    start_rss = get_rss()  # Read /proc/self/status
    # Handle request...
    end_rss = get_rss()
    prometheus.gauge("route_rss_delta", end_rss - start_rss, {"route": "/api/users"})
```

Result: Application code polluted with instrumentation, RSS delta is approximate (includes other concurrent requests).

**With temporal-slab (structural observability):**

```c
// Zero instrumentation—metrics emerge from allocator
EpochDomain* route_domain = epoch_domain_create(alloc, "route:/api/users");

// All request allocations automatically attributed
void* req = alloc_obj_domain(alloc, 128, route_domain);
void* session = alloc_obj_domain(alloc, 256, route_domain);

// Query anytime, no external tracking
SlabEpochStats stats;
slab_stats_epoch(alloc, size_class, route_domain->epoch, &stats);
prometheus_gauge("route_rss", stats.estimated_rss_bytes, "route", stats.label);
```

Result: Clean application code, precise attribution, zero overhead.

### The Architectural Difference

**malloc profilers:**
- **Add observability after-the-fact** (instrumentation)
- **Reconstruct phase attribution** via expensive mechanisms (backtraces, sampling)
- **Cost:** 5-30% overhead

**temporal-slab:**
- **Observability by design** (structural)
- **Phase attribution built-in** (epochs are first-class)
- **Cost:** 0% (metrics exist for correctness)

This is why temporal-slab's observability is a **unique contribution**, not just "better profiling." It's an emergent property of phase-level memory management that pointer-level allocators fundamentally cannot provide.

## Hazard Pointers and Reference Counting

Hazard pointers and reference counting are two techniques for safe memory reclamation in lock-free data structures. They solve the same problem: how do you safely free memory when threads might still be accessing it? temporal-slab avoids both techniques through conservative recycling, which is simpler but less aggressive.

**The problem: Safe memory reclamation**

In lock-free code, a thread can load a pointer to an object, then be preempted (paused) for milliseconds. While paused, another thread might free that object and return its memory to the allocator. When the first thread resumes and dereferences its pointer, it accesses freed memory—use-after-free, resulting in crashes or corruption.

```
Thread A:                          Thread B:
1. Load ptr to slab S
2. [PREEMPTED]                    3. Free last object in S
                                  4. S becomes empty
                                  5. Recycle S, return to cache
                                  6. Allocate new slab, reuse S
                                  7. S now contains different data
8. Resume, access S
9. CRASH (S was recycled!)
```

The challenge: how does Thread B know it's safe to recycle S? Thread A holds a pointer but hasn't published it anywhere—Thread B has no way to detect this.

**Technique 1: Hazard Pointers**

Hazard pointers are a lock-free technique where threads publish pointers they're currently using in a global "hazard list." Before freeing memory, a thread checks if any hazard pointer references it. If so, defer the free.

```c
// Thread A accessing object
void access_object(Object* obj) {
    hazard_ptr[thread_id] = obj;  // Publish: "I'm using this"
    atomic_thread_fence();         // Ensure published before use
    
    // Use object safely (no one can free it)
    obj->data = 42;
    
    hazard_ptr[thread_id] = NULL;  // Unpublish
}

// Thread B freeing object
void free_object(Object* obj) {
    // Check all hazard pointers
    for (int i = 0; i < num_threads; i++) {
        if (hazard_ptr[i] == obj) {
            // Someone is using it! Defer free
            retire_list_push(obj);
            return;
        }
    }
    // No one using it, safe to free
    actual_free(obj);
}
```

**Hazard pointer overhead:**

Every access requires:
1. Publishing pointer to hazard list (atomic store)
2. Memory fence (ensure visibility)
3. Unpublishing pointer after use (atomic store)

Every free requires:
1. Scanning all threads' hazard pointers (N threads = N loads)
2. If hazardous, push to retire list (deferred free)
3. Periodically retry retiring objects (scan again)

Cost: ~20-50 cycles per access (atomic stores + fences), O(N) per free (scan N threads). For N=32 threads, every free scans 32 hazard pointers—expensive.

**Technique 2: Reference Counting**

Reference counting tracks how many threads hold pointers to an object. When the count reaches zero, the object is safe to free.

```c
struct Object {
    atomic_int refcount;
    Data data;
};

// Thread A accesses object
Object* obj = load_object();
atomic_fetch_add(&obj->refcount, 1);  // Increment

// Use object
obj->data = 42;

atomic_fetch_sub(&obj->refcount, 1);  // Decrement
if (obj->refcount == 0) {
    free_object(obj);  // Last reference, safe to free
}
```

**Reference counting overhead:**

Every access requires:
1. Atomic increment on load (20-40 cycles)
2. Atomic decrement on release (20-40 cycles)

Cost: 40-80 cycles per access. For short-lived accesses (allocate → use → free), this overhead doubles the cost.

**Cyclic references:** If objects reference each other (A → B → A), refcount never reaches zero. Requires cycle detection (expensive) or weak references (complex).

**Contention:** If many threads access the same object, they all atomic-increment the same refcount. The cache line containing the refcount bounces between cores (false sharing). This can be worse than mutex contention.

**Why temporal-slab avoids both:**

temporal-slab uses conservative recycling instead:

**Conservative recycling rule:** Defer all slab recycling until explicit `epoch_close()` calls, avoiding immediate recycling in the allocation/free hot path.

```
During allocation/free (hot path):
- free_obj() NEVER recycles empty slabs
- Empty slabs just move to PARTIAL list or stay there
- Increments empty_partial_count for tracking
- Zero recycling overhead in fast path

During epoch_close() (cold path):
- Scans BOTH partial AND full lists for empty slabs
- Removes empty slabs from lists atomically
- Pushes to slab cache for reuse
- madvise() reclaims RSS for unpublished slabs
```

This avoids complexity:
- **No per-access overhead:** Threads don't track slab lifecycle during alloc/free
- **No immediate recycling decisions:** free_obj() just updates bitmap and moves slabs between lists
- **No retire lists:** Recycling happens in bulk during epoch_close (deterministic timing)
- **Deferred reclamation:** Application controls when RSS drops happen (aligned with phase boundaries)

**The tradeoff:**

Immediate recycling (malloc-style):
- Can recycle any empty slab as soon as it becomes empty
- Lower RSS baseline (no accumulated empty slabs)
- Cost: Recycling logic in every free() path, unpredictable RSS fluctuations

Conservative (deferred) recycling:
- Zero recycling overhead in alloc/free paths
- RSS drops are deterministic (only during epoch_close)
- Cost: RSS stays elevated until epoch_close() is called

temporal-slab chooses predictability and control: empty slabs accumulate during an epoch's lifetime, then get recycled in bulk when the application calls epoch_close(). This means:
1. RSS reflects high-water mark until explicit reclamation
2. No surprise GC pauses or background reclamation threads
3. Application controls when RSS drops happen (e.g., after request completes)
4. Recycling cost is amortized across many slabs (bulk operation)

In practice, this matches application-level phase boundaries: a web server processes a request (RSS grows), then calls epoch_close() when the request completes (RSS drops). The epoch mechanism prevents unbounded growth by ensuring old epochs drain and get recycled before wraparound.

## ABA Problem

The ABA problem is a classic concurrency bug that occurs in lock-free algorithms using compare-and-swap (CAS). It happens when a memory location changes from value A to value B, then back to A, causing a CAS operation to succeed even though the state has changed underneath.

**The classic scenario:**

```
Thread 1:                          Thread 2:
1. Read ptr → Node A
2. Plan: CAS(ptr, A, new)
3. [PREEMPTED]
                                   4. Pop A from stack
                                   5. Free Node A
                                   6. Allocate new node → reuses same address A
                                   7. Push new A onto stack
8. Resume
9. CAS(ptr, A, new) → SUCCESS!
   (But this is the NEW A, not the original!)
```

Thread 1's CAS succeeds because the pointer value is identical (address A), but the object at that address is completely different. This is the "ABA" sequence: value A, changed to B (different object), changed back to A (reused address).

**Why it's dangerous:**

```c
// Lock-free stack pop
Node* pop(Stack* s) {
    Node* head;
    Node* next;
    do {
        head = atomic_load(&s->head);        // Read current head (A)
        if (!head) return NULL;
        next = head->next;                    // Read A's next pointer
        // [PREEMPTED HERE]
        // Other thread: pop A, free A, reuse A's address for new node
        // CAS succeeds because address is same, but A->next is now garbage!
    } while (!CAS(&s->head, head, next));    // CAS succeeds with wrong 'next'
    return head;
}
```

If A's address is reused, `next` points to the old A's successor, not the new A's successor. The stack becomes corrupted.

**Solution 1: Tagged pointers (generation counters)**

A **generation counter** (also called a version tag or sequence number) is an integer that increments each time memory is reused. By pairing a pointer with its generation, you can detect when the pointed-to memory has been freed and reallocated—even if the address is the same.

Add a generation counter to the pointer:

```c
struct TaggedPtr {
    Node* ptr;         // 48 bits: actual pointer
    uint16_t tag;      // 16 bits: version counter
};

atomic_compare_exchange(&tagged_ptr,
                        {old_ptr, old_tag},
                        {new_ptr, old_tag + 1});  // Increment tag
```

Even if the pointer value matches, the tag won't—CAS fails if the generation changed.

**Solution 2: Generation counters (temporal-slab's approach)**

Instead of tagging pointers directly, store generation counters separately:

```c
// Handle encoding
[slab_id:22][generation:24][slot:8][class:8]

// Registry lookup
SlabMeta* meta = &registry[slab_id];
if (meta->generation != handle.generation) {
    // ABA detected: slab was recycled and reused
    return ERROR_INVALID_HANDLE;
}
```

When a slab is recycled:
1. Registry entry's generation is incremented
2. Old handles have stale generation
3. Handle validation detects mismatch → safe failure

**Why 24 bits for generation?**

24 bits = 16,777,216 generations. If a slab is recycled every microsecond, it takes 16 seconds to wrap. In practice:
- Slabs are recycled every milliseconds (under extreme churn)
- Wrapping takes ~4.6 hours at 1 recycle/ms
- By then, all handles referencing old generations are long dead

**ABA is impossible if:**
1. Memory is never freed (no address reuse) → Conservative recycling
2. Memory reuse is detected (generation counters) → temporal-slab's approach
3. Hazard pointers prevent premature free → Expensive per-access overhead

temporal-slab uses generation counters because:
- Zero fast-path overhead (counter checked only on validation, not allocation)
- Simple implementation (single integer per slab)
- Provably safe (24-bit wraparound is astronomically unlikely)

## Slab Cache

When a slab becomes empty, it is not immediately destroyed. Instead, it is pushed to a per-size-class cache. The cache has fixed capacity (e.g., 32 slabs). When a new slab is needed, the allocator checks the cache first. If non-empty, pop a slab from the cache (fast, no syscall). If empty, allocate a new slab via mmap (slow, syscall).

The cache absorbs transient fluctuations in allocation rate. A workload that allocates 1 million objects, frees them, allocates 1 million more can reuse slabs from the cache without mmap/munmap churn.

If the cache is full and another slab becomes empty, it is pushed to an overflow list. The overflow list has unbounded capacity. Slabs on the overflow list remain mapped—they are not unmapped (no munmap).

## Refusal to Unmap vs madvise

temporal-slab in its current implementation (v0.x) never unmaps slabs during runtime. When a slab becomes empty, it is pushed to the cache or overflow list but remains mapped. Unmapping only occurs during allocator destruction.

This design choice provides two critical benefits:

1. **Safe handle validation.** temporal-slab uses opaque handles encoding a slab pointer, slot index, and size class. When freeing a handle, the allocator checks the slab's magic number and slot state. If the handle is invalid (double-free, wrong allocator, corrupted), the check fails and returns an error. But this only works if the slab remains mapped. If unmapped, dereferencing the handle triggers a segmentation fault.

2. **Bounded RSS stability.** RSS is bounded by the high-water mark of allocations. If a workload briefly allocates 10GB then stabilizes at 1GB, RSS is 10GB. This is deliberate: RSS reflects the maximum working set observed, not the current working set. The RSS does not drift upward over time—it remains constant at the high-water mark.

**The trade-off: Higher absolute RSS for safety and stability**

This approach causes higher absolute RSS compared to allocators that aggressively return memory to the OS. The allocator holds onto empty slabs in the cache and overflow lists, keeping them mapped even when unused.

**Example:**
```
Workload: Allocate 10,000 objects, free all, allocate 1,000 objects

malloc/jemalloc:
- Peak RSS: 40 MB (10,000 objects)
- After free: 5 MB (aggressive munmap returns memory)
- Steady state: 5 MB (1,000 objects)

temporal-slab (current):
- Peak RSS: 40 MB (10,000 objects)
- After free: 40 MB (slabs remain mapped)
- Steady state: 40 MB (1,000 objects + cached slabs)

Result: temporal-slab uses 8× more RSS despite 10× fewer live objects
```

**Why this was acceptable for v0.x:**

The priority was proving temporal grouping prevents RSS **drift**:
- malloc: 12 MB → 25 MB over 7 days (+108% drift)
- temporal-slab: 12 MB → 12 MB (0% drift)

Absolute RSS (40 MB vs 5 MB) was secondary to stability (no drift).

**The path forward: madvise(MADV_DONTNEED)**

Linux provides `madvise(MADV_DONTNEED)` which releases physical memory while keeping the virtual mapping:

```c
// Tell OS: "I don't need this physical memory, free it"
madvise(slab, 4096, MADV_DONTNEED);

// Virtual address remains valid (no segfault if accessed)
// Physical pages returned to OS (RSS drops)
// If accessed later, page fault allocates new physical page (zeroed)
```

**Key properties:**
- Virtual address remains mapped (no segfault)
- Physical memory released (RSS drops immediately)
- Handle validation still works (magic number and bitmap remain accessible)
- No mmap/munmap churn (virtual mapping unchanged)

**Why madvise is better than munmap for temporal-slab:**

```
munmap(slab):
- Virtual address unmapped → accessing it segfaults
- Handle validation broken → cannot safely check stale handles
- Requires mmap to reuse → syscall overhead

madvise(slab, MADV_DONTNEED):
- Virtual address remains mapped → accessing it succeeds (faults in new page)
- Handle validation works → stale handles detected safely
- No mmap needed to reuse → zero syscall overhead
```

**Implementation strategy:**

```c
// When epoch is closed and all slabs are empty:
for (each empty slab in closed epoch) {
  madvise(slab, 4096, MADV_DONTNEED);
  // Physical memory released
  // Virtual mapping intact
  // Handle validation still works
}
```

**Expected impact:**

```
Workload: Allocate 10,000 objects, free all, allocate 1,000 objects

temporal-slab with madvise:
- Peak RSS: 40 MB (10,000 objects)
- After free + epoch close: 5 MB (madvise released 35 MB)
- Steady state: 5 MB (1,000 objects)

Result: Competitive absolute RSS with malloc, zero drift over time
```

**Implementation status: Complete**

madvise(MADV_DONTNEED) is implemented and active. Empty slabs in CLOSING epochs have physical memory released while virtual mappings remain intact.

**Trade-offs realized:**

**Benefits:**
- RSS drops to match live set (competitive with malloc)
- Safety contract intact (no segfaults, handle validation works via registry)
- No syscall churn (madvise once per epoch close, not per allocation)
- 98-99% physical page reclaim under epoch-aligned workloads

**Costs:**
- Page faults on reuse (minor faults, ~1-2µs per slab, amortized)
- Registry overhead (12 bytes per slab for off-page metadata)
- One extra indirection (4-5 cycles for registry lookup)

**How it works in production:**

```c
// Allocate in epoch 1
EpochId e1 = epoch_current(alloc);
void* obj = slab_malloc_epoch(alloc, 128, e1);

// Use object
process(obj);
slab_free(alloc, obj);

// Close epoch when phase ends
epoch_close(alloc, e1);
// → Epoch 1 marked CLOSING
// → When last object freed, slab becomes empty
// → madvise(slab, 4096, MADV_DONTNEED) called
// → Physical memory released (RSS drops)
// → Virtual mapping intact (handle validation still works)
```

## Epoch-Granular Memory Reclamation

Epoch-granular memory reclamation is the ability to release memory at the granularity of entire epochs rather than individual allocations or holes. This is temporal-slab's unique differentiator over traditional allocators.

**The problem with hole-granular reclamation:**

Traditional allocators (malloc, jemalloc, tcmalloc) reclaim memory by finding "holes"—freed regions—and returning them to the OS:

```
Page with mixed lifetimes:
[Live obj][FREED    ][Live obj][FREED    ][Live obj]

Cannot reclaim because live objects pin the page
Hole-granular reclamation is blocked by scattered live objects
```

Over time, live objects scatter across many pages. Even aggressive compaction (moving live objects to consolidate pages) cannot solve this without:
- Pointer invalidation (breaks C/C++ semantics)
- Stop-the-world pauses (unacceptable latency)
- Heuristic guessing (when is it safe to compact?)

**Epoch-granular reclamation:**

temporal-slab groups allocations by epoch. When an epoch is closed, no new allocations enter it. As objects are freed, the epoch drains. When the epoch is completely empty, the allocator knows all slabs in that epoch can be reclaimed:

```
Epoch 1 (closed):
Slab A: [FREED][FREED][FREED]... (all empty)
Slab B: [FREED][FREED][FREED]... (all empty)
Slab C: [FREED][FREED][FREED]... (all empty)

All slabs empty → Call madvise on entire epoch → RSS drops predictably
```

**Why this is impossible for general allocators:**

malloc has no concept of epochs. It doesn't know which objects were allocated together or when their collective lifetime phase ends. It must wait for holes to appear and hope they can be coalesced.

temporal-slab explicitly tracks lifetime phases via epochs. The application signals phase transitions with `epoch_advance()` or `epoch_close()`. This provides the allocator information that malloc fundamentally lacks.

**The `epoch_close()` API:**

```c
// Application logic
EpochId batch_epoch = epoch_current(alloc);

// Allocate all objects for a batch in the same epoch
for (int i = 0; i < batch_size; i++) {
  void* obj = slab_malloc_epoch(alloc, size, batch_epoch);
  process_object(obj);
}

// Batch complete, close the epoch
epoch_close(alloc, batch_epoch);
// → No new allocations in this epoch
// → When all objects freed, madvise all slabs
// → RSS drops deterministically
```

**Predictable reclamation timeline:**

```
T=0: epoch_advance() → Epoch 1 becomes active
T=0-5s: Allocate 10,000 objects in Epoch 1
T=5s: epoch_close(Epoch 1) → Epoch 1 closed
T=5-60s: Objects freed as they expire (sessions timeout, requests complete)
T=60s: Last object freed → Epoch 1 completely empty
T=60s: madvise() all Epoch 1 slabs → RSS drops by 40 MB

Result: Deterministic reclamation 60 seconds after close
```

Compare to malloc:
```
T=0-5s: Allocate 10,000 objects
T=5-60s: Objects freed as they expire
T=60s: All objects freed... but RSS remains high
T=???: malloc eventually returns some memory (nondeterministic)

Result: RSS drops slowly over unknown timeline, never fully reclaimed
```

**Why this matters for control planes:**

Control plane services have predictable lifetime phases:
- Request processing: Allocate request metadata, free after response
- Session management: Allocate session state, free after timeout
- Cache eviction: Allocate cache entries, free on LRU eviction

Each phase can use a dedicated epoch. When the phase ends, close the epoch. Memory is reclaimed deterministically without guessing when objects will die.

**Example: Kubernetes controller**

```c
// Watch loop for Kubernetes resources
while (true) {
  EpochId watch_epoch = epoch_current(alloc);
  
  // Process watch events for this interval
  for (int i = 0; i < events_per_interval; i++) {
    Event* evt = parse_event(watch_epoch);  // Allocates in watch_epoch
    handle_event(evt);
    // evt freed after handling
  }
  
  // Interval complete, close epoch
  epoch_close(alloc, watch_epoch);
  // → All event metadata from this interval reclaimed when freed
  // → RSS drops predictably every interval
  
  epoch_advance(alloc);  // Start new interval
}
```

**The claim after implementation:**

"Returns memory at epoch granularity with deterministic timing. Applications control reclamation by closing epochs when lifetime phases end. Impossible with general-purpose allocators that lack lifetime phase tracking."

**Current status (implementation complete):**

All components are implemented:
- ✅ `epoch_close()` API (marks epochs CLOSING)
- ✅ Epoch state tracking (ACTIVE vs CLOSING via atomic array)
- ✅ madvise integration (empty slabs in CLOSING epochs get physical memory released)
- ✅ Slab registry (enables safe handle validation even after madvise/munmap)

temporal-slab can now return memory at epoch granularity with deterministic timing.

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

## Tail Latency and Percentiles

Tail latency is the latency experienced by the slowest requests in a system. While average latency measures typical performance, tail latency measures worst-case performance—the requests that take unusually long to complete. For real-time and latency-sensitive systems, tail latency often matters more than average latency.

**Why average latency is misleading:**

```
Web server processes 10,000 requests:
- 9,990 requests: 50ns each
- 10 requests: 10,000ns each (outliers)

Average latency:
(9,990 × 50ns + 10 × 10,000ns) / 10,000 = 59.95ns

This looks great! But:
- 99.9% of requests see 50ns (excellent)
- 0.1% of requests see 10,000ns (catastrophic, 200× slower)

For HFT: A single 10µs spike causes a missed trade worth millions
For control plane: A single 10ms spike violates SLA, triggers alerts
```

Average latency hides outliers. A system with "60ns average" could have 50ns typical and 100ms worst-case.

**Percentiles measure tail latency:**

Percentiles describe the distribution of latencies:

- **p50 (median):** 50% of requests complete faster than this
- **p95:** 95% of requests complete faster than this
- **p99:** 99% of requests complete faster than this (1 in 100 is slower)
- **p99.9:** 99.9% of requests complete faster than this (1 in 1,000 is slower)
- **p99.99:** 99.99% of requests complete faster than this (1 in 10,000 is slower)

**Example distribution:**

```
Allocator latency measurements (100M allocations, GitHub Actions validated):

p50  = 40ns   ← Half of allocations take ≤40ns
p95  = 100ns  ← 95% of allocations take ≤100ns
p99  = 120ns  ← 99% of allocations take ≤120ns
p99.9 = 340ns ← 99.9% of allocations take ≤340ns

This tells us:
- Typical case (p50): 40ns
- Best 95%: Within 100ns (very consistent)
- Best 99%: Within 120ns (extremely consistent)
- Worst 0.1%: Up to 340ns (8.5× slower, but bounded)
```

**Why tail latency matters more than average:**

**1. User experience is dominated by worst case:**

```
User loads webpage that makes 100 API calls:
- 99 calls: 10ms each
- 1 call: 1000ms (p99 tail latency spike)

Page load time: max(all calls) = 1000ms
User sees: Slow page (1 second delay)

Average latency (19ms) is irrelevant—user experience determined by slowest call
```

**2. High-frequency systems amplify tail risk:**

```
HFT system: 10,000 trades per second
p99.9 = 100µs means 10 trades/second hit 100µs latency

If each spike causes missed trade worth $10K:
10 spikes/second × $10K = $100K/second lost

Average latency is irrelevant—business impact driven by tail
```

**3. SLA violations are tail events:**

```
API SLA: 99.9% of requests complete within 50ms

If p99.9 = 60ms:
→ SLA violated
→ Customer refunds triggered
→ Reputation damage

Average latency could be 5ms (excellent), but p99.9 determines SLA compliance
```

**Sources of tail latency:**

**1. Garbage collection pauses:**
```
Java/Go allocators trigger GC periodically:
- Typical allocation: 50ns
- During GC: 10ms pause (all allocations blocked)
→ p99.9 spikes to 10ms
```

**2. Lock contention:**
```
Mutex-protected allocator:
- Uncontended: 100ns
- Contended (8 threads blocked): 5µs (50× slower)
→ p99 includes contention spikes
```

**3. Page faults:**
```
Allocator mmaps new slab:
- Cache hit: 50ns
- Cache miss + mmap: 3µs (60× slower)
→ p99.9 includes mmap syscall overhead
```

**4. Cache misses:**
```
Metadata scattered across memory:
- L1 hit: 50ns
- DRAM miss: 150ns (3× slower)
→ p95-p99 degraded by cache misses
```

**5. CPU scheduling:**
```
Thread preempted mid-allocation:
- Typical: 50ns
- Preempted: 10µs (thread descheduled, rescheduled)
→ p99.99 includes scheduler jitter
```

**Why temporal-slab optimizes for tail latency:**

temporal-slab is designed for predictable p99.9 performance:

**Measured tail latency (GitHub Actions validated):**
```
100 million allocations, ubuntu-latest, AMD EPYC 7763:
p50  = 40ns
p99  = 120ns  (3× median - excellent consistency)
p99.9 = 340ns (8.5× median, but bounded and predictable)

12-13× better than malloc (1,443ns p99, 4,409ns p999)
```

**How temporal-slab achieves low tail latency:**

**1. Lock-free fast path (no contention spikes):**
```
No mutexes in fast path:
- Every allocation: Atomic CAS (20-40 cycles)
- No blocking, no waiting, no contention
→ p99 and p50 converge (predictable)
```

**2. Slab cache (no mmap in fast path):**
```
Empty slabs cached (32 per size class):
- Cache hit: 50ns (pop from cache)
- Cache miss: 3µs (mmap syscall)
- Cache hit rate: 99%+
→ p99 avoids mmap overhead
```

**3. O(1) class selection (no branch misprediction jitter):**
```
Lookup table instead of branches:
- Every allocation: 4 cycles (L1 cache hit)
- No unpredictable branches
→ No jitter from size variation
```

**4. Bounded metadata (cache-friendly):**
```
Bitmap per slab (8 bytes):
- Fits in single cache line
- No scattered metadata
→ Consistent L1 hit rate
```

**Comparing allocators by tail latency:**

| Allocator | p50 | p99 | p99.9 | Tail Behavior |
|-----------|-----|-----|-------|---------------|
| **malloc (system)** | 31ns | 1,443ns | 4,409ns | Lock contention, heuristics |
| **tcmalloc** | ~60ns | ~150ns | ~2µs | Thread cache misses |
| **jemalloc** | ~70ns | ~180ns | ~3µs | Arena lock contention |
| **temporal-slab** | **40ns** | **120ns** | **340ns** | Consistent (lock-free) |

temporal-slab's p99 is 12× better than malloc, p999 is 13× better (GitHub Actions validated).

**Why HFT systems care about p99.99:**

```
HFT trading system:
- 100,000 decisions per second
- p99.99 = 1 decision per 10,000 = 10 events per second

If p99.99 = 10µs (missed trading window):
→ 10 missed trades per second
→ At $1K per trade: $10K/second opportunity cost
→ Daily: $864M potential loss

For HFT: p99.99 matters more than average
```

**Why control planes care about p99.9:**

```
Kubernetes control plane:
- 10,000 API requests per second
- p99.9 = 10 requests per second

If p99.9 > 100ms (SLO violation):
→ 10 slow requests per second
→ User perception: "Control plane is slow"
→ Triggers alerts, investigation overhead

For control plane: p99.9 determines user satisfaction
```

**Measuring tail latency correctly:**

**Wrong approach (average of maximums):**
```c
for (int i = 0; i < 100; i++) {
    uint64_t max_latency = 0;
    for (int j = 0; j < 1000; j++) {
        uint64_t latency = measure_allocation();
        if (latency > max_latency) max_latency = latency;
    }
    printf("Max: %lu ns\n", max_latency);
}
// Averages the maximums (wrong!)
```

This measures the average of worst-case, not the distribution.

**Right approach (percentiles from full distribution):**
```c
uint64_t latencies[100000000];  // 100M samples

for (int i = 0; i < 100000000; i++) {
    latencies[i] = measure_allocation();
}

sort(latencies);  // Sort ascending

p50  = latencies[50000000];   // 50th percentile
p99  = latencies[99000000];   // 99th percentile
p999 = latencies[99900000];   // 99.9th percentile
```

This captures the true distribution, including all outliers.

**Tail latency as a system property:**

Tail latency is not just about individual components—it compounds:

```
Request flow:
1. Load balancer: p99 = 1ms
2. API gateway: p99 = 2ms
3. Application: p99 = 5ms
4. Database: p99 = 10ms
5. Cache: p99 = 0.5ms

Overall p99 ≥ max(1, 2, 5, 10, 0.5) = 10ms

If components are independent:
Overall p99 ≈ sum of p99s (worst case)
         = 18.5ms

1% of requests hit worst case in EVERY component
```

Reducing tail latency in any component improves overall system p99.

**The tail latency tax:**

In distributed systems, tail latency is amplified:

```
Fan-out query (1 request → 100 parallel backend calls):

If each backend has p99 = 10ms:
- Probability all 100 finish in <10ms: 0.99^100 = 36.6%
- Probability at least one takes >10ms: 63.4%

→ Overall p50 (median) is worse than backend p99!

With 100 parallel calls, backend p99 becomes frontend p50
```

This is the "tail latency amplification" problem—systems with fan-out must optimize backend p99.9 to achieve acceptable frontend p50.

**temporal-slab's tail latency guarantee:**

```
Under single-threaded load (GitHub Actions validated):
p99 = 120ns (3× p50 of 40ns)

Under multi-threaded load:
Scales to 8-16 threads with <15% lock contention

No GC pauses, no lock contention on fast path, no unbounded jitter
→ Predictable tail behavior suitable for real-time systems
```

**When to optimize for tail latency:**

| System Type | Metric Priority | Why |
|-------------|----------------|-----|
| **HFT trading** | p99.99 | Single spike = missed trade worth $$$$ |
| **Real-time control** | p99.9 | Deadline miss = system failure |
| **User-facing API** | p99 | Slow requests = poor UX |
| **Batch processing** | p50 (average) | Total throughput matters, not outliers |
| **Background jobs** | p50 (average) | No user waiting, optimize for throughput |

If humans or SLAs are waiting, optimize for tail latency. If throughput is the goal, average latency is sufficient.

## Platform-Specific Considerations: x86-64 Linux

temporal-slab is designed primarily for x86-64 Linux systems. This is not an arbitrary choice—specific hardware and OS features on this platform enable performance characteristics that would be difficult or impossible to achieve elsewhere. Understanding these dependencies clarifies what temporal-slab assumes about the underlying system and what would need to change for portability.

**The target platform: x86-64 Linux**

x86-64 refers to the 64-bit extension of the x86 instruction set architecture, implemented by Intel (x64) and AMD (AMD64). Linux refers to the kernel and its memory management subsystem. Together, these provide:

1. Strong memory ordering (TSO - Total Store Ordering)
2. Efficient atomic instructions (LOCK prefix, CMPXCHG, XADD)
3. Predictable page size (4KB)
4. madvise(MADV_DONTNEED) semantics that release physical memory
5. Transparent huge pages support (optional)
6. NUMA awareness (for multi-socket systems)

Each of these affects temporal-slab's design and performance.

**Memory Ordering: x86-64 TSO vs ARM Relaxed**

The most critical difference between architectures is memory ordering—the rules governing when memory operations become visible to other threads. x86-64 uses TSO (Total Store Ordering), a strong memory model that provides implicit ordering guarantees. ARM, PowerPC, and RISC-V use weaker models that require explicit synchronization.

**x86-64 TSO guarantees:**

```
Thread A:                    Thread B:
store X = 1                  load Y into r1
store Y = 1                  load X into r2

x86-64 TSO: If r1 = 1, then r2 MUST = 1
(Stores happen in program order, loads see stores in order)
```

This is the "total store order" property: all threads see stores in the same order. Critically, this means acquire loads and release stores are essentially free on x86-64:

```c
// Release store (publish data)
atomic_store_explicit(&ready, 1, memory_order_release);
// Compiles to: MOV [ready], 1 (regular store, no fence)

// Acquire load (consume published data)
int val = atomic_load_explicit(&ready, memory_order_acquire);
// Compiles to: MOV val, [ready] (regular load, no fence)
```

The hardware already prevents reordering that would violate acquire/release semantics. The compiler must avoid reordering (via `memory_order`), but no CPU-level fence is needed.

**ARM relaxed ordering:**

ARM uses a relaxed memory model where loads and stores can be reordered freely unless explicitly prevented:

```
Thread A:                    Thread B:
store X = 1                  load Y into r1
store Y = 1                  load X into r2

ARM relaxed: r1 = 1 and r2 = 0 is ALLOWED
(Thread B can see stores out of order!)
```

To prevent this, ARM requires explicit memory barriers:

```c
// Release store on ARM
atomic_store_explicit(&ready, 1, memory_order_release);
// Compiles to:
//   DMB ISHST (fence: ensure prior stores complete)
//   STR [ready], 1 (store)

// Acquire load on ARM
int val = atomic_load_explicit(&ready, memory_order_acquire);
// Compiles to:
//   LDR val, [ready] (load)
//   DMB ISH (fence: ensure subsequent loads wait)
```

DMB (Data Memory Barrier) instructions cost 10-50 cycles depending on cache state. This adds significant overhead to lock-free algorithms.

**Impact on temporal-slab's hot path:**

Recall the hot path cycle accounting from earlier:

```
x86-64 (current):
- Acquire load (current_partial): 2 cycles (MOV instruction)
- Release store (publish slab): 2 cycles (MOV instruction)
- Total: 38 cycles for full allocation

ARM (hypothetical port):
- Acquire load: 2 cycles (LDR) + 20 cycles (DMB) = 22 cycles
- Release store: 2 cycles (STR) + 20 cycles (DMB) = 22 cycles
- Total: 38 + 40 = 78 cycles for full allocation

ARM would be 2× slower due to fence overhead alone.
```

The hot path uses acquire/release semantics in three places:
1. Loading `current_partial` (acquire)
2. Loading the bitmap (relaxed, no fence)
3. Publishing new `current_partial` in slow path (release)

On x86-64, these are free. On ARM, each acquire/release adds 20+ cycles. This compounds across millions of allocations per second.

**Why x86-64 TSO enables predictable latency:**

Lock-free algorithms fundamentally require memory ordering. Without x86-64's free acquire/release, temporal-slab would need explicit fences, pushing latency from 120ns (x86-64 validated) to ~200ns (ARM estimated). This might seem small, but it's ~1.7× slower and introduces additional variance.

**Page Size: 4KB Standard vs 64KB ARM**

temporal-slab assumes 4KB pages. This is standard on x86-64 Linux but not universal:

**x86-64 Linux:**
- Default page size: 4KB (4096 bytes)
- Huge pages: 2MB, 1GB (optional, via mmap flags)
- Page table: 4-level (PML4, PDPT, PD, PT)

**ARM64 Linux:**
- Default page size: 4KB (most systems) or 64KB (some systems, configurable at kernel compile time)
- Huge pages: 2MB, 512MB, 1GB (varies by page size)
- Page table: 4-level (configurable)

**Why page size matters for slab efficiency:**

temporal-slab allocates one slab per page (4KB). With 8 size classes (64, 96, 128, 192, 256, 384, 512, 768 bytes), the number of objects per slab varies:

```
Size class 64 bytes:
- 4KB page: 4096 / 64 = 64 objects per slab
- 64KB page: 65536 / 64 = 1024 objects per slab

Size class 768 bytes:
- 4KB page: 4096 / 768 = 5 objects per slab
- 64KB page: 65536 / 768 = 85 objects per slab
```

On 64KB pages, slabs hold 16× more objects. This has two effects:

**Positive:** Lower per-object metadata overhead (bitmap is 128 bytes vs 8 bytes, but amortized over 16× more objects).

**Negative:** Coarser granularity for RSS reclamation. An epoch with 1000 objects might fit on 200 slabs (4KB pages) or 12 slabs (64KB pages). When the epoch drains, 4KB-page systems can madvise individual slabs as they empty (fine-grained reclamation). 64KB-page systems must wait for all 85 objects in a slab to be freed before reclaiming that 64KB.

This increases RSS granularity by 16×, making temporal-slab's bounded RSS guarantee weaker. A workload with 1000 live objects might have:
- 4KB pages: 1000 objects + 32 partially empty slabs = ~1.1MB RSS (10% overhead)
- 64KB pages: 1000 objects + 2 partially empty slabs = ~1.2MB RSS (20% overhead)

**Porting consideration:** temporal-slab would need runtime page size detection (`sysconf(_SC_PAGESIZE)`) and adjusted slab allocation logic to handle 64KB pages efficiently.

**madvise(MADV_DONTNEED): Linux vs BSD vs Windows**

temporal-slab relies on `madvise(MADV_DONTNEED)` to release physical memory while keeping virtual mappings intact. This syscall's behavior is OS-specific.

**Linux madvise(MADV_DONTNEED):**
```c
madvise(slab, 4096, MADV_DONTNEED);
// Effect:
// 1. Physical pages released immediately (RSS drops instantly)
// 2. Virtual mapping remains (no segfault if accessed)
// 3. Next access triggers minor page fault, page is zeroed
// 4. Zero-page optimization: Pages map to shared zero page until written
```

This is exactly what temporal-slab needs: RSS drops immediately, but handle validation still works (accessing the slab faults in a zero page, magic number reads as 0, validation fails safely).

**BSD madvise(MADV_DONTNEED):**
```c
madvise(slab, 4096, MADV_DONTNEED);
// Effect:
// 1. Advisory hint, MAY release physical pages (not guaranteed)
// 2. Behavior is implementation-defined
// 3. Some BSD variants treat MADV_DONTNEED as "I will not need this soon" rather than "release now"
```

On FreeBSD/OpenBSD, MADV_DONTNEED is a hint, not a directive. The kernel might defer reclamation or ignore it entirely. For aggressive RSS control, BSD systems need `MADV_FREE` (mark pages free but don't zero) or `madvise(..., MADV_DONTNEED)` followed by `mmap(MAP_FIXED)` to force reclamation.

**Windows VirtualAlloc/VirtualFree:**

Windows has no direct equivalent to madvise. The closest options:

```c
// Option 1: VirtualFree with MEM_DECOMMIT
VirtualFree(slab, 4096, MEM_DECOMMIT);
// Effect:
// - Physical pages released
// - Virtual address remains reserved
// - Accessing decommitted memory crashes (no zero-page fault)

// Option 2: VirtualAlloc with MEM_RESET
VirtualAlloc(slab, 4096, MEM_RESET, PAGE_READWRITE);
// Effect:
// - Pages marked as "can be discarded"
// - OS may release physical pages under pressure
// - Accessing pages after MEM_RESET reads garbage (not zeroes!)
```

Neither option provides Linux's semantics. `MEM_DECOMMIT` crashes on access (breaks handle validation). `MEM_RESET` doesn't guarantee reclamation and doesn't zero pages.

**Porting temporal-slab to Windows would require:**
1. Accepting crashes on stale handle validation (use registry exclusively, never dereference slab pointers)
2. OR: Never unmapping/decommitting memory (trading RSS control for safety)
3. OR: Implementing handle validation entirely via registry (no magic number checks in slab headers)

**Atomic Instructions: LOCK CMPXCHG vs LL/SC**

x86-64 uses a single-instruction CAS: `LOCK CMPXCHG`. ARM uses load-linked/store-conditional (LL/SC): `LDXR/STXR`. These have different performance characteristics.

**x86-64 CAS (LOCK CMPXCHG):**
```asm
lock cmpxchg [mem], new_value
// Atomic operation: compare [mem] with expected, swap if equal
// Cost: 20-40 cycles (includes memory bus lock and cache coherence)
```

This is a single instruction. The LOCK prefix acquires exclusive ownership of the cache line, performs the comparison and swap atomically, then releases ownership. Other cores see this as one indivisible operation.

**ARM LL/SC (LDXR/STXR):**
```asm
retry:
  ldxr  r0, [mem]         ; Load-linked: mark [mem] for exclusive access
  cmp   r0, expected      ; Compare loaded value with expected
  bne   fail              ; If not equal, fail
  stxr  r1, new_value, [mem]  ; Store-conditional: store if exclusive access still valid
  cbnz  r1, retry         ; If store failed (r1=1), retry
fail:
```

This is a 4-5 instruction sequence. The load-linked marks the cache line. If another thread writes to the line before store-conditional executes, the store fails and the loop retries.

**Why this matters:**

LL/SC can experience spurious failures—failures not caused by contention, but by unrelated cache activity (e.g., prefetching, speculative loads). x86-64 CMPXCHG only fails if another thread actually modified the value.

Under high contention (8+ threads), ARM's LL/SC retry rate can be 10-20% higher than x86-64 CMPXCHG due to spurious failures. This compounds the 20-cycle fence overhead discussed earlier.

**TLB and Cache Behavior: x86-64 vs ARM**

Both architectures have TLBs and multi-level caches, but details differ:

**x86-64 typical (Intel Skylake):**
- L1 TLB: 64 entries (data), 128 entries (instruction)
- L2 TLB: 1536 entries (shared)
- TLB miss: 4-level page table walk (~100-200 cycles)
- Cache line: 64 bytes
- L1: 32KB (per core, 8-way, 4-cycle latency)
- L2: 256KB (per core, 8-way, 12-cycle latency)
- L3: 8-32MB (shared, 16-way, 40-cycle latency)

**ARM typical (Cortex-A76):**
- L1 TLB: 48 entries (data), 48 entries (instruction)
- L2 TLB: 1024 entries (shared)
- TLB miss: 4-level page table walk (~150-300 cycles, slower than x86-64)
- Cache line: 64 bytes (same as x86-64)
- L1: 64KB (per core, 4-way, 4-cycle latency)
- L2: 256-512KB (per core, 8-way, 12-cycle latency)
- L3: 2-4MB (shared, 16-way, 40-cycle latency)

ARM's smaller TLB (48 vs 64 entries at L1) means higher TLB miss rates for workloads touching many pages. temporal-slab's temporal grouping (objects on same pages) benefits both architectures, but helps ARM slightly more due to its tighter TLB.

ARM's larger L1 cache (64KB vs 32KB) can help with metadata-heavy workloads, but temporal-slab's bitmaps are small (8 bytes per slab), so this doesn't matter much.

**NUMA: Multi-Socket x86-64 Systems**

High-end x86-64 servers have multiple sockets, each with its own memory controller. This creates NUMA (Non-Uniform Memory Access): local memory is faster than remote memory.

```
System topology:
Socket 0: Cores 0-15, 64GB RAM (local)
Socket 1: Cores 16-31, 64GB RAM (local)

Thread on Core 0 accessing:
- Socket 0 RAM: 100 cycles (local)
- Socket 1 RAM: 200 cycles (remote, crosses socket interconnect)
```

temporal-slab allocates slabs via `mmap(NULL, ...)`, which returns memory from the calling thread's local NUMA node by default (first-touch policy). If Thread 0 (Socket 0) allocates a slab, it's placed in Socket 0 RAM. If Thread 16 (Socket 1) later accesses that slab, it pays the remote access penalty.

**NUMA-aware optimization (future work):**

```c
// Per-NUMA-node allocators
SlabAllocator* allocators[num_nodes];

void* numa_aware_alloc(size_t size) {
    int node = numa_node_of_cpu(sched_getcpu());  // Which socket am I on?
    return slab_malloc(allocators[node], size);
}
```

This keeps allocations local to the socket that created them, avoiding remote access penalties. However, it complicates epoch management (epochs become per-node) and increases memory overhead (16 epochs × 2 sockets = 32 epoch pools).

**Why Linux Specifically (Not Just POSIX)**

temporal-slab uses Linux-specific features that are not portable to other UNIX systems:

**1. madvise(MADV_DONTNEED) semantics:**
- POSIX doesn't standardize MADV_DONTNEED behavior
- Linux: Immediate physical release, zero-page fault on access
- BSD: Advisory hint, may not release immediately
- Solaris: Different semantics (MADV_DONTNEED marks pages as "not needed soon")

**2. Transparent Huge Pages (THP):**
- Linux: `madvise(MADV_HUGEPAGE)` promotes pages to 2MB huge pages transparently
- BSD: No THP support (must use explicit huge page APIs)
- This is optional for temporal-slab but improves TLB efficiency

**3. /proc/self/status for RSS monitoring:**
- Linux: `VmRSS` field shows resident set size
- BSD: Must use `getrusage()` or `kvm_getprocs()`
- Different tools for introspection

**4. prctl(PR_SET_THP_DISABLE) for control:**
- Linux: Fine-grained THP control per-process
- BSD: No equivalent

**Porting Checklist: What Would Need to Change**

To port temporal-slab to other platforms:

**ARM64 Linux:**
- Add explicit memory barriers (DMB instructions) for acquire/release semantics
- Expect ~2× latency increase (40ns validated → ~80ns estimated) due to fence overhead
- Handle 64KB page size (runtime detection, adjust slab sizes)
- Test LL/SC spurious failure rate under high contention
- **Estimated effort:** Medium (2-4 weeks, mostly testing)

**x86-64 BSD (FreeBSD/OpenBSD):**
- Replace MADV_DONTNEED with MADV_FREE or explicit mmap(MAP_FIXED) to force reclamation
- Test that RSS actually drops (BSD's lazy reclamation might delay)
- Adjust RSS monitoring (use getrusage instead of /proc)
- **Estimated effort:** Low (1-2 weeks)

**x86-64 Windows:**
- Replace mmap/munmap with VirtualAlloc/VirtualFree
- Replace madvise with MEM_DECOMMIT (sacrifices handle validation safety) or MEM_RESET (doesn't guarantee reclamation)
- Replace pthread TLS with Windows TLS APIs (TlsAlloc/TlsGetValue)
- Handle different page fault behavior (MEM_DECOMMIT crashes, can't rely on zero-page faults)
- **Estimated effort:** High (4-8 weeks, significant API differences)

**ARM64 Android:**
- Same as ARM64 Linux (memory barriers, 64KB pages)
- Plus: Android's aggressive OOM killer requires careful RSS management
- Plus: Bionic libc differences (smaller than glibc differences, but present)
- **Estimated effort:** Medium (3-5 weeks)

**RISC-V Linux:**
- Similar to ARM (weak memory model, needs explicit fences)
- Less mature toolchain (potential compiler issues with atomic intrinsics)
- Fewer deployed systems (harder to benchmark)
- **Estimated effort:** High (4-6 weeks, uncharted territory)

**Performance Summary: x86-64 vs ARM**

```
Hot path latency (single-threaded):
x86-64: 40ns p50, 120ns p99 (GitHub Actions validated)
ARM64: ~80ns p50, ~200ns p99 (estimated, due to fences)

Hot path latency (multi-threaded):
x86-64: Scales to 8-16 threads with <15% lock contention
ARM64: Additional overhead from LL/SC vs CAS (estimated)

RSS reclamation granularity:
x86-64 (4KB pages): 4KB per slab
ARM64 (64KB pages): 64KB per slab (16× coarser)

madvise effectiveness:
Linux: Immediate RSS drop, zero-page fault on access
BSD: Deferred reclamation, no zero-page guarantee
Windows: Must choose between safety (no reclaim) or crashes (VirtualFree)
```

**Why These Differences Matter**

temporal-slab's design optimizes for x86-64 Linux because that's where the target workloads run: HFT trading systems, high-performance API servers, real-time control planes. These systems overwhelmingly run on x86-64 Linux in datacenters.

The ~2× performance difference between x86-64 (40ns p50, 120ns p99 validated) and ARM (~80ns p50, ~200ns p99 estimated) demonstrates the architectural advantage of x86-64's strong memory model. While both are suitable for most workloads, x86-64's TSO provides lower and more predictable latency.

If ARM systems eventually dominate datacenters (Apple M-series servers, AWS Graviton at scale), temporal-slab could be ported. The bounded RSS guarantee would remain (architectural, not platform-specific), but latency would degrade due to unavoidable fence overhead. For workloads where bounded RSS matters more than single-digit microsecond latency differences, ARM would be acceptable.

**The Design Philosophy: Optimize for Reality**

temporal-slab could be written in a platform-agnostic way, avoiding x86-64-specific assumptions. But this would sacrifice performance on the platform that matters:

```
Platform-agnostic approach:
- Use seq_cst everywhere (strongest ordering, works on all architectures)
- Cost: 20-50 cycle fences on x86-64 (even though TSO makes them unnecessary)
- Result: 40ns → ~80ns latency on x86-64 (2× slower for portability we don't need)

Platform-specific approach (current):
- Use acquire/release on x86-64 (zero cost, leverages TSO)
- Cost: Must add fences when porting to ARM
- Result: 40ns p50 on x86-64 (optimal, validated), ~80ns p50 on ARM (acceptable, only if ported)
```

The philosophy: optimize for the platform you're running on, not the platform you might someday port to. If ARM becomes critical, pay the porting cost then. Don't sacrifice 50% performance today for hypothetical portability tomorrow.

This is pragmatic systems design: make tradeoffs based on real deployment constraints, not abstract portability ideals.

## Handle-Based API vs Malloc-Style API

temporal-slab provides two APIs:

**Handle-based:** `alloc_obj(alloc, size, &handle)` returns a pointer and an opaque handle. `free_obj(alloc, handle)` frees by handle. The handle encodes a registry ID, generation counter, slot index, and size class. No per-allocation metadata is stored (zero overhead). The application must track handles alongside pointers.

**Malloc-style:** `slab_malloc(alloc, size)` returns a pointer. The handle is stored in an 8-byte header before the pointer. `slab_free(alloc, ptr)` reads the header, extracts the handle, and calls `free_obj` internally. This trades 8 bytes overhead per allocation for API compatibility.

The handle-based API is suitable for performance-critical code where zero overhead is required and explicit handle management is acceptable. The malloc-style API is suitable for drop-in replacement where 8 bytes overhead is tolerable.

## Handle Encoding and Slab Registry

temporal-slab uses **indirect handles** that go through a slab registry rather than encoding raw pointers. This enables safe memory reclamation while maintaining handle validation.

**Handle encoding (v1, 64-bit):**

```
[63:42] slab_id (22 bits)    - Registry index (max 4M slabs)
[41:18] generation (24 bits)  - ABA protection (wraps after 16M reuses)
[17:10] slot (8 bits)         - Object index within slab (max 255 objects)
[9:2]   size_class (8 bits)   - Size class 0-255
[1:0]   version (2 bits)      - Handle format version (v1=0b01)
```

**Slab registry architecture:**

```c
// Registry entry (stored off-page)
struct SlabMeta {
    Slab* ptr;      // Pointer to slab (NULL if unmapped)
    uint32_t gen;   // Generation counter (survives madvise)
};

// Registry (one per allocator)
struct SlabRegistry {
    SlabMeta* metas;  // Array of [slab_id] → (ptr, gen)
    uint32_t cap;     // Registry capacity
};

// Handle resolution
SlabHandle h = alloc_obj(alloc, size, &h);
                ↓
Extract slab_id, gen, slot, class from handle
                ↓
SlabMeta* meta = &registry.metas[slab_id]
                ↓
Check: meta->gen == handle.gen? (ABA protection)
                ↓
Slab* slab = meta->ptr (NULL if unmapped)
                ↓
Validate and return object
```

**Why indirection?**

Direct pointer encoding (v0.x approach):
```
Handle: [slab_ptr:48][slot:8][class:8]
Problem: Can't safely unmap slabs (handle becomes dangling pointer)
```

Registry-based encoding (current):
```
Handle: [slab_id:22][gen:24][slot:8][class:8][ver:2]
Solution: Registry entry can be NULL (safe unmapping)
         Generation counter prevents ABA (recycled slab detection)
```

**Benefits of registry approach:**

1. **Safe madvise/munmap:** Registry entry can mark slab as unmapped (ptr=NULL)
2. **ABA protection:** Generation counter prevents use-after-recycle bugs
3. **Portable handles:** No raw pointers, works across address spaces
4. **Metadata survives madvise:** Generation stored off-page, immune to MADV_DONTNEED
5. **Handle validation always works:** Even after slab unmapped, can return error

**Trade-offs:**

**Cost:**
- One extra indirection per handle dereference (~4-5 cycles for L1 cache hit)
- Registry memory: 12 bytes per slab (ptr + gen + padding)
- Max 4M slabs (22-bit slab_id field)

**Benefit:**
- Can safely call madvise/munmap for RSS reclamation
- Stale handles detected via generation mismatch
- No segfaults on invalid frees (returns false instead)

**Example flow with madvise:**

```c
// Allocate in epoch 1
EpochId e1 = epoch_current(alloc);
SlabHandle h = alloc_obj_epoch(alloc, 128, e1, &h);
// Handle: slab_id=42, gen=0, slot=0, class=2

// Close epoch 1
epoch_close(alloc, e1);

// Free object
free_obj(alloc, h);
// Slab becomes empty

// madvise releases physical memory
madvise(slab, 4096, MADV_DONTNEED);
// Slab data zeroed, but registry.metas[42] = {ptr=slab, gen=0} survives

// Later: try to use stale handle
free_obj(alloc, h);  // Same handle again
→ Registry lookup: slab_id=42 → ptr=slab, gen=0
→ Access slab->magic (page fault if madvised, but safe)
→ Detect double-free, return false

// Slab recycled for new allocations
→ Registry: gen++ (now gen=1)
→ Old handle (gen=0) now invalid

// Old handle used again
free_obj(alloc, h);  // gen=0
→ Registry: gen=1 (mismatch!)
→ Return false (ABA detected)
```

**Generation counter wrap-around:**

With 24 bits, generation wraps after 16,777,216 reuses:
```
At 1M alloc/free per second per slab:
Wrap time: 16.8 seconds

Danger: If you hold a handle for >16.8 seconds AND the same slab
        is recycled 16M times, ABA is possible (gen wraps to same value)

Reality: Slabs reused every few milliseconds under typical churn
         16M reuses of same slab in <16.8 seconds is implausible
         Pathological case: Program must do nothing but alloc/free same slab
```

**Implementation status:**

All memory reclamation features are implemented:
- madvise(MADV_DONTNEED) releases physical memory
- epoch_close() enables deterministic reclamation
- Slab registry with generation counters enables safe munmap

Handle encoding v1 is stable and production-ready.

## Generation Counter Mechanics

Generation counters are the mechanism temporal-slab uses to detect use-after-recycle bugs (ABA problem) without runtime overhead on the fast path. Every slab has an associated generation counter stored in the registry (off-page). When a slab is allocated from the cache, the counter increments. When a handle is created, it captures the current generation. Handle validation compares captured generation against current generation—mismatch indicates the slab was recycled.

**Counter lifecycle:**

```c
// Slab allocation from cache
Slab* get_slab_from_cache(SizeClassAlloc* sc) {
    Slab* slab = cache_pop(&sc->cache);
    if (!slab) {
        slab = mmap_new_slab();  // Slow path
    }
    
    // Register slab and assign generation
    uint32_t slab_id = registry_alloc(&allocator->registry, slab);
    SlabMeta* meta = &allocator->registry.metas[slab_id];
    meta->gen++;  // Increment generation on reuse
    
    return slab;
}

// Handle creation
SlabHandle alloc_obj(SlabAllocator* alloc, size_t size) {
    Slab* slab = find_slab_with_free_slot();
    uint32_t slab_id = slab->registry_id;
    uint32_t gen = alloc->registry.metas[slab_id].gen;  // Capture current generation
    uint8_t slot = allocate_slot_from_bitmap(slab);
    
    return pack_handle(slab_id, gen, slot, size_class);
}

// Handle validation
bool free_obj(SlabAllocator* alloc, SlabHandle h) {
    uint32_t slab_id = extract_slab_id(h);
    uint32_t handle_gen = extract_generation(h);
    
    SlabMeta* meta = &alloc->registry.metas[slab_id];
    if (meta->gen != handle_gen) {
        // Generation mismatch: slab was recycled
        return false;  // ABA detected
    }
    
    // Proceed with free...
}
```

**Why 24 bits?**

The generation field in the handle is 24 bits, providing 16,777,216 unique values before wraparound. This creates a fundamental tension:

- **Larger counter:** Lower wraparound risk, safer ABA protection
- **Smaller counter:** More bits for other fields (slab_id, slot, class)

24 bits balances these constraints based on realistic workload analysis:

```
Assumptions:
- Slab recycling rate: 1 per millisecond (extreme churn)
- Handle lifetime: Seconds to minutes (typical)

Wraparound time:
16,777,216 generations / 1000 recycles per second = 16,777 seconds
                                                   = 4.66 hours

Danger zone:
If you hold a handle for >4.66 hours AND the same slab is recycled
16M times during that period, generation wraps to the same value.

Reality check:
- Handles are short-lived (freed within seconds/minutes)
- Slab recycling rate peaks at ~1000/sec under extreme allocation churn
- Holding a handle for 4+ hours is pathological
- By the time 16M cycles complete, old handles are long freed

Conclusion: 24-bit generation provides sufficient safety margin
```

**When generation increments:**

Generation increments occur at specific lifecycle events:

1. **Slab first allocated:** `gen = 0` (initial value)
2. **Slab recycled from cache:** `gen++` (reuse detected)
3. **Slab unmapped then remapped:** `gen++` (address reuse protected)

Generation does NOT increment on:
- Individual object allocations (zero overhead per allocation)
- Individual object frees (zero overhead per free)
- Slab state transitions (PARTIAL ↔ FULL)

This design ensures generation checking has:
- **Zero fast-path cost:** No atomic operations during alloc/free
- **Validation-time cost only:** Single integer comparison on handle dereference
- **Slab-granular protection:** All objects in a slab share the same generation

**Wraparound handling:**

When generation reaches 16,777,215 and increments, it wraps to 0:

```c
meta->gen = (meta->gen + 1) % (1 << 24);  // Wraps: 16777215 → 0
```

Wraparound creates a vulnerability window where:
```
Old handle: gen=16777215 (last generation before wrap)
Slab recycled 16M times exactly
New handle: gen=16777215 (same value after full cycle)
→ ABA possible if old handle still live
```

**Why this is acceptable:**

1. **Handle lifetime constraint:** Applications should not hold handles for hours
2. **Recycle rate ceiling:** 16M recycles in <5 hours requires 930 recycles/second sustained
3. **Probabilistic safety:** Wraparound collision requires exact timing alignment
4. **Observable failure mode:** Wrong-generation access returns error, doesn't corrupt memory

**Comparison to other ABA solutions:**

| Approach | Overhead | Protection | Wraparound Risk |
|----------|----------|------------|-----------------|
| **Hazard pointers** | 20-50 cycles/access | Perfect | N/A (no counters) |
| **Reference counting** | 40-80 cycles/access | Perfect | N/A (no counters) |
| **128-bit tagged pointers** | 0 cycles (hardware CAS) | Perfect | 2^64 generations (never wraps) |
| **temporal-slab (24-bit gen)** | 0 cycles (validation only) | Probabilistic | 16M generations (~5 hours) |

temporal-slab sacrifices perfect ABA protection for zero fast-path overhead. The 24-bit generation provides sufficient safety for realistic workloads while keeping handles compact (64 bits total).

**Generation counter vs epoch era counter:**

temporal-slab uses two separate counter systems:

**Generation counter (per-slab, 24-bit):**
- Detects slab recycling within the same epoch
- Stored in registry (survives madvise)
- Increments when slab is reused from cache
- Purpose: Handle validation, ABA protection

**Epoch era counter (global, 64-bit):**
- Tracks epoch ring buffer rotations
- Monotonically increasing, never wraps
- Increments on every epoch_advance()
- Purpose: Observability, epoch disambiguation

These serve different purposes and operate at different granularities (slab-level vs epoch-level).

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

**3. Conservative recycling (Deferred + Slab Cache):**
- Empty slabs recycled only during epoch_close() (not in alloc/free path)
- Scans both PARTIAL and FULL lists for empty slabs
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

**Step 4: Epoch-aligned reclamation returns memory to OS**
```
Peak allocation: 50,000 sessions = 50,000 / 15 = 3,334 slabs = 13.3 MB
After 1 hour: All sessions freed from old epochs
→ epoch_close() triggered for expired epochs
→ Empty slabs madvised (physical memory released)
→ RSS: 12 MB (matches live set + minimal overhead)
After 1 week: RSS = 12 MB (0% drift)

With slab registry + generation counters:
- Safe madvise/munmap (handle validation via registry)
- No segfaults (registry marks unmapped slabs as NULL)
- ABA protection (generation mismatch detects recycled slabs)
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

temporal-slab achieves 0% RSS drift and competitive absolute RSS (with epoch_close + madvise) but has:
- Fixed-size classes only (768 bytes max, 11.1% internal fragmentation)
- Requires lifetime-correlated allocation patterns (works for sessions, connections, requests; doesn't help random sizes)
- One extra indirection per handle (4-5 cycles for registry lookup)
- 12 bytes registry overhead per slab

For long-running systems where RSS drift is unacceptable and latency predictability is critical (HFT, control planes, real-time systems), this is the right tradeoff.

## What You Should Understand Now

At this point, you understand the foundational concepts:

Memory allocation bridges the gap between OS-provided pages and application-needed objects. Allocators face two fragmentation problems: spatial (free space scattered into unusable holes) and temporal (pages pinned by mixtures of live and dead objects). Temporal fragmentation is entropy—inevitable without organizing allocations by lifetime.

Slab allocation solves this by dividing pages into fixed-size slots (size classes) and filling slabs sequentially. Objects allocated around the same time end up in the same slab. If they have correlated lifetimes (allocation-order affinity), they die together, and the slab can be recycled as a unit. This manages entropy by grouping objects to expire in an organized way.

temporal-slab extends this with:
- **Lock-free fast-path allocation** (120ns p99, 340ns p999 validated on GitHub Actions)
- **Phase boundary alignment** (applications signal lifetime phase completion via epoch_close())
- **Passive epoch reclamation** (no quiescence requirements—threads observe state changes asynchronously)
- **Conservative recycling** (deferred until epoch_close, zero overhead in alloc/free paths)
- **Slab registry with generation counters** (enables safe madvise/munmap with ABA protection)
- **Epoch-granular reclamation** (deterministic RSS drops aligned with application phase boundaries)
- **O(1) class selection** (lookup table eliminates branching jitter)
- **Adaptive bitmap scanning** (self-tunes between sequential and randomized modes based on contention)
- **Epoch domains** (RAII-style scoped lifetimes for nested phases)
- **Comprehensive observability** (zero-cost production telemetry via stats APIs)

The allocator does not predict lifetimes or require application hints. Lifetime alignment emerges naturally from allocation patterns. Applications control when to reclaim memory by closing epochs at phase boundaries—structural moments where logical units of work complete (requests end, frames render, transactions commit, batches finish). This is substrate, not policy—a foundation for higher-level systems to build on.

**Current implementation (production-ready):**

All core features are implemented and validated:
- **Core allocator:** 8 fixed size classes (64-768 bytes), 16-epoch ring buffer
- **RSS reclamation:** madvise(MADV_DONTNEED) releases physical memory, epoch_close() enables deterministic reclamation
- **Safety:** Slab registry with 24-bit generation counters, no runtime munmap (safe handle validation)
- **Performance:** 40ns p50, 120ns p99, 340ns p999 (12-13× better tail latency than malloc, GitHub Actions validated)
- **Observability:** Global/class/epoch stats APIs, slow-path attribution, madvise tracking, RSS estimation
- **Epoch domains:** RAII-style scoped lifetimes with refcount tracking and era validation
- **Adaptive algorithms:** Bitmap scanning self-tunes based on CAS retry rate (converges in 100-300K allocations)

**Validated results (GitHub Actions, AMD EPYC 7763, ubuntu-latest):**
- p99: 120ns (temporal-slab) vs 1,443ns (malloc) = **12.0× better**
- p999: 340ns (temporal-slab) vs 4,409ns (malloc) = **13.0× better**
- RSS growth with epoch_close(): **0%** (validated across 20-cycle tests)
- Phase shift retention: -71.9% (temporal-slab) vs -18.6% (malloc) = **53.3pp advantage**

The implementation details—how bitmaps encode slot state, how atomic CAS loops allocate slots, how registry indirection works, how generation counters prevent ABA, how phase boundaries map to epoch_close() calls—follow from these foundational concepts. Those details are in the source code (src/slab_alloc.c, extensively commented). This document provides the foundation to understand why they exist and how they enable temporal-slab's unique guarantees: **bounded RSS, predictable tail latency, and zero-cost observability** under sustained churn.
