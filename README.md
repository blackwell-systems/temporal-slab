# ZNS-Slab: Specialized Cache for Sub-4KB Objects

A cache system optimized for small objects, designed to outperform Redis through slab allocation and cache-line optimization.

**Important**: ZNS-Slab is a cache system, not a system of record. It provides fast, memory-efficient storage for ephemeral data.

**Target Performance**: 2-3x faster allocation, <5% memory overhead for 128-byte objects.

**Note**: All performance claims are design targets for Phase 1. See [BENCHMARKS.md](./BENCHMARKS.md) for methodology and validated results (pending implementation).

---

## Quick Navigation

**New here?** Start with the [Executive Summary](./EXECUTIVE_SUMMARY.md) for high-level explanation and audience-specific pitches.

**Technical deep-dive?** See [Technical Design](./TECHNICAL_DESIGN.md) for implementation specifications.

**Understanding the opportunity?** Read [Innovation Frontiers](./INNOVATION_FRONTIERS.md) for market context.

---

## Core Innovation

**Invariant**: All objects within a slab share the same lifecycle and eviction boundary.

This constraint eliminates:
- Memory fragmentation (zero holes)
- Garbage collection overhead
- Compaction pauses
- Cache line splits

Result: Perfect packing of small objects into CPU cache-aligned 4KB pages.

## Documentation

### [INNOVATION_FRONTIERS.md](./INNOVATION_FRONTIERS.md)
Explores the three major innovation opportunities in cache systems:

1. **Tiered Memory Systems** - What broke: Binary "RAM or disk" model
2. **Intent-Aware Eviction** - What broke: Recency-only prediction (LRU)
3. **Serverless Architectures** - What broke: Single-server cache model

Each section frames the problem as a *broken assumption*, not just new technology. This is architectural thinking.

### [USE_CASES.md](./USE_CASES.md)
10 domains where small objects dominate critical infrastructure:

- Session/Auth systems (millions of tokens)
- Microservice control planes (service discovery, feature flags)
- Real-time counters (rate limits, analytics)
- AI/LLM inference (context state, KV-cache metadata)
- Feature stores (online ML)
- Gaming (session state, matchmaking)
- IoT backends (sensor data, device state)
- Observability (trace headers, correlation IDs)
- Financial systems (order books, risk counters)
- Distributed systems internals (Raft metadata, leases)

**Key insight**: Small objects aren't edge cases—they're the connective tissue of modern infrastructure.

### [TECHNICAL_DESIGN.md](./TECHNICAL_DESIGN.md)
Complete implementation specification including:

- **Non-Goals** - Explicit constraints that enable specialization
- Memory layouts and slab structures
- Core data structures (Go implementation)
- Algorithms for allocation, get, delete (with latency targets)
- Concurrency model (lock-free reads, per-bucket locking)
- Eviction progression: LRU → LFU → Intent-Aware
- Performance targets with workload assumptions
- Tiered storage with cost/latency model

## Why This Matters

### The Problem
General-purpose allocators and cache systems incur significant per-object overhead for small objects:
- **Object headers**: Type info, refcount, metadata (16-24 bytes)
- **Allocator metadata**: Size classes, alignment padding (10-30%)
- **Fragmentation**: Memory holes from varied lifetimes
- **Cache locality**: Poor packing leads to L1 cache misses

### The Solution
ZNS-Slab packs objects into 4KB slabs with minimal overhead:
- **30 objects per slab** (128-byte size class)
- **192 bytes total metadata** (64B header + 128B bitmap)
- **6.4 bytes per object** overhead (192 / 30)
- **Zero fragmentation** (fixed-size slots)
- **Cache-line aligned** (64-byte boundaries)

### The Targets (Phase 1)
- **<5% memory overhead** (vs estimated 30-50% for Redis/malloc)
- **2-3x faster allocation** (target <100ns p99 vs malloc ~150-300ns)
- **Zero fragmentation** (guaranteed by design)
- **Predictable latency** (no GC pauses, no compaction)

**See [BENCHMARKS.md](./BENCHMARKS.md) for detailed analysis and measurement plan.**

## Implementation Roadmap

### Phase 1: Core Slab Allocator ✅ (Current)
The allocator *is* the product. Everything else is infrastructure.

- Fixed-size slabs (64B, 128B, 256B, 512B objects)
- Bitmap allocation tracking
- Baseline benchmarks vs. malloc

### Phase 2: Tiered Slab Pools
Validate hardware innovation early.

- DRAM pool (hot tier, ~100ns)
- NVMe pool (warm tier, ~20μs)
- Automatic promotion/demotion

### Phase 3: Hash Table Integration
Standard infrastructure layer.

- Per-bucket locking
- LRU eviction within slabs
- Redis comparison benchmarks

### Phase 4: Intent-Aware Optimization
AI-assisted, not AI-dependent. Advisory predictions with LRU fallback.

### Phase 5: Distributed Support (Optional)
RDMA as acceleration layer, not requirement.

## Quick Start

### For Non-Technical Readers
Read the [Executive Summary](./EXECUTIVE_SUMMARY.md) - includes:
- The problem explained simply
- 30-second and 60-second elevator pitches
- Audience-specific summaries (engineers, managers, CTOs, investors)

### For Engineers

**Ready to implement?** See [PHASE1_IMPLEMENTATION.md](./PHASE1_IMPLEMENTATION.md) for:
- Concrete C code structures
- Bitmap operations with ffs()
- mmap parameters and error handling
- Benchmark harness implementation
- Testing strategy

**Architecture overview?** See [TECHNICAL_DESIGN.md](./TECHNICAL_DESIGN.md) for high-level design.

```bash
# Quick start after reading PHASE1_IMPLEMENTATION.md
cd src/
gcc -O2 -o slab_test slab.c bitmap.c test_slab.c
./slab_test
```

## Project Status

**Current Phase**: Phase 1.5 Complete ✅

**Achieved:** Release-quality slab allocator with measurable, attributable, and reproducible performance.

**Performance (Intel Core Ultra 7 165H, GCC 13.3.0 -O3):**
- p50: 26ns allocation, 24ns free
- p99: 1423ns allocation, 45ns free
- p999: 2303ns allocation, 184ns free
- Memory overhead: 3.4%
- Cache hit rate: 97%

**Quick Start:**
```bash
make bench  # One-command build + benchmark + regression check
```

See [PHASE1.5_COMPLETE.md](./PHASE1.5_COMPLETE.md) for full release details and [RELEASE_NOTES_v1.5.md](./RELEASE_NOTES_v1.5.md) for technical architecture.

## License

MIT
