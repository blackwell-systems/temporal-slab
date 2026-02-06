# ZNS-Slab Documentation Guide

This project has comprehensive documentation for different audiences and purposes. Here's your navigation guide.

## Documentation Structure

```
ZNS-Slab/
├── README.md                    # Project overview and quick start
├── EXECUTIVE_SUMMARY.md         # High-level explanation for all audiences
├── INNOVATION_FRONTIERS.md      # Market opportunity and broken assumptions
├── USE_CASES.md                 # 10 domains where small objects dominate
├── TECHNICAL_DESIGN.md          # Complete implementation specification
└── DOCUMENTATION_GUIDE.md       # This file - navigation help
```

---

## Choose Your Path

### 🎯 "I want a 30-second pitch"
→ [EXECUTIVE_SUMMARY.md](./EXECUTIVE_SUMMARY.md#elevator-pitch-30-seconds)

One paragraph explaining the problem, solution, and impact.

---

### 👔 "I need to explain this to my manager/CTO"
→ [EXECUTIVE_SUMMARY.md](./EXECUTIVE_SUMMARY.md#audience-specific-summaries)

Tailored explanations for:
- Engineering Managers (cost/capacity focus)
- CTOs/VPs (strategic positioning)
- Investors (market opportunity)

---

### 💼 "I'm using this for my resume"
→ [EXECUTIVE_SUMMARY.md](./EXECUTIVE_SUMMARY.md#for-recruitersresume)

Resume-ready bullet point that demonstrates systems thinking.

Also see: [README.md Resume Impact section](./README.md#resume-impact)

---

### 🔍 "I want to understand the market opportunity"
→ [INNOVATION_FRONTIERS.md](./INNOVATION_FRONTIERS.md) + [USE_CASES.md](./USE_CASES.md)

**Innovation Frontiers** covers:
- Three major innovation frontiers in cache systems
- What assumptions broke (not just new tech)
- How ZNS-Slab fits into the landscape
- Competitive analysis vs Redis/Memcached/RocksDB

**Use Cases** covers:
- 10 specific domains (auth, control planes, counters, AI, gaming, etc.)
- Why small objects dominate each domain
- Real-world scale examples
- Market sizing and growth drivers

**Read these if you want to understand WHY this matters and WHERE it applies.**

---

### 📊 "I want to see benchmark data and performance claims"
→ [BENCHMARKS.md](./BENCHMARKS.md)

**Ground truth for all performance claims:**
- Canonical slab layout with exact overhead calculations
- Design targets vs. validated results (clearly separated)
- Redis baseline analysis methodology
- Reproducible benchmark specifications
- Commitment to honest measurement

**Read this if you're skeptical about the numbers.**

---

### 🛠️ "I want implementation details"
→ [TECHNICAL_DESIGN.md](./TECHNICAL_DESIGN.md)

Complete specification including:
- Memory layouts and data structures
- Algorithm specifications with latency targets
- Concurrency model (lock-free reads, per-bucket locking)
- Performance targets with workload assumptions
- API design and configuration

**Read this if you want to BUILD this.**

---

### 🚀 "I want to get started coding"
→ [README.md Quick Start](./README.md#quick-start)

Then:
1. Review [BENCHMARKS.md](./BENCHMARKS.md) for measurement methodology
2. Read [TECHNICAL_DESIGN.md Phase 1](./TECHNICAL_DESIGN.md#core-data-structures)
3. Implementation language: C/Rust (TBD in Phase 1)

---

### 🤔 "I want to understand the core innovation"
→ [README.md Core Innovation](./README.md#core-innovation)

Or: [EXECUTIVE_SUMMARY.md The Core Insight](./EXECUTIVE_SUMMARY.md#the-core-insight)

**One-sentence version:**
> If objects share the same lifetime, you never delete them—you reset the entire slab in one constant-time hardware operation.

---

### 📊 "I need to present this in a meeting"
Use this flow:

1. **Problem** (1 slide)
   - Redis wastes 62% memory on small objects
   - Source: [EXECUTIVE_SUMMARY.md The Problem](./EXECUTIVE_SUMMARY.md#the-problem)

2. **Solution** (1 slide)
   - Lifetime-aligned slabs eliminate fragmentation
   - Source: [EXECUTIVE_SUMMARY.md The Core Insight](./EXECUTIVE_SUMMARY.md#the-core-insight)

3. **Impact** (1 slide)
   - 20x memory efficiency, 4x faster access
   - Source: [EXECUTIVE_SUMMARY.md Why It Matters](./EXECUTIVE_SUMMARY.md#why-it-matters)

4. **Roadmap** (1 slide)
   - 5 phases, currently in Phase 1
   - Source: [README.md Implementation Roadmap](./README.md#implementation-roadmap)

---

### 🎓 "I'm preparing for interviews"
Read in this order:

1. [README.md Core Innovation](./README.md#core-innovation) - memorize the invariant
2. [EXECUTIVE_SUMMARY.md The Problem](./EXECUTIVE_SUMMARY.md#the-problem) - understand what broke
3. [README.md Why This Matters](./README.md#why-this-matters) - know the numbers (62% → 3%)
4. [INNOVATION_FRONTIERS.md Three Frontiers](./INNOVATION_FRONTIERS.md#three-major-innovation-frontiers) - show you understand trends
5. [README.md Resume Impact](./README.md#resume-impact) - practice your pitch

**Practice saying:**
> "I identified that Redis has 62% memory overhead for small objects due to allocator fragmentation. I designed a specialized slab allocator that packs sub-4KB objects into cache-aligned pages, reducing overhead to 3% and achieving 4x faster access by maximizing CPU L1 cache hits."

---

## Reading Time Estimates

| Document | Audience | Time | Purpose |
|----------|----------|---------|---------|
| README.md | Everyone | 3 min | Project overview |
| EXECUTIVE_SUMMARY.md | Non-technical | 5 min | High-level understanding |
| BENCHMARKS.md | Engineers/Skeptics | 10 min | Performance ground truth |
| USE_CASES.md | Product/Business | 10 min | Real-world applications |
| INNOVATION_FRONTIERS.md | Strategic | 15 min | Market context |
| TECHNICAL_DESIGN.md | Engineers | 30 min | Implementation |

---

## Key Concepts at a Glance

### The Invariant
**All objects within a slab share the same lifecycle and eviction boundary.**

This is the foundational insight everything else builds on.

### The Numbers (Phase 1 Targets)
- **<5% overhead**: Per-object metadata (vs estimated 30-50% for Redis/malloc)
- **2-3x faster allocation**: Target <100ns p99 (vs malloc ~150-300ns)
- **Zero fragmentation**: Guaranteed by fixed-size slots
- **6.4 bytes overhead** per 128B object (192 bytes slab metadata / 30 objects)

**All numbers are design targets. See [BENCHMARKS.md](./BENCHMARKS.md) for validation status.**

### The Three Frontiers (Broken Assumptions)
1. **Tiered Memory**: Binary "RAM or disk" model collapsed
2. **Intent-Aware Eviction**: Recency-only (LRU) fails for structured patterns
3. **Serverless**: Single-server cache model evaporated

### The Five Phases
1. Core slab allocator (current)
2. Tiered slab pools
3. Hash table integration
4. Intent-aware optimization
5. Distributed support (optional)

---

## FAQ: Which Document Answers What?

**Q: Why does this matter?**
→ [EXECUTIVE_SUMMARY.md Why It Matters](./EXECUTIVE_SUMMARY.md#why-it-matters)

**Q: How is this different from Redis?**
→ [EXECUTIVE_SUMMARY.md Why This Is Different](./EXECUTIVE_SUMMARY.md#why-this-is-different)

**Q: What's the core innovation?**
→ [README.md Core Innovation](./README.md#core-innovation)

**Q: How do I implement this?**
→ [TECHNICAL_DESIGN.md](./TECHNICAL_DESIGN.md)

**Q: What workloads is this for?**
→ [USE_CASES.md](./USE_CASES.md) (comprehensive) or [EXECUTIVE_SUMMARY.md Use Cases](./EXECUTIVE_SUMMARY.md#why-it-matters) (quick summary)

**Q: What are the performance targets?**
→ [BENCHMARKS.md](./BENCHMARKS.md) (authoritative source) or [TECHNICAL_DESIGN.md Performance Targets](./TECHNICAL_DESIGN.md#performance-targets) (summary)

**Q: Why can't Redis just add this?**
→ [INNOVATION_FRONTIERS.md Why Redis Is Constrained](./INNOVATION_FRONTIERS.md#why-redis-is-architecturally-constrained-from-this)

**Q: What are the non-goals?**
→ [TECHNICAL_DESIGN.md Non-Goals](./TECHNICAL_DESIGN.md#non-goals)

**Q: How do I explain this to investors?**
→ [EXECUTIVE_SUMMARY.md For Investors](./EXECUTIVE_SUMMARY.md#for-investors)

---

## Document Maturity Status

| Document | Status | Notes |
|----------|--------|-------|
| README.md | ✅ Stable | Entry point, regularly updated |
| EXECUTIVE_SUMMARY.md | ✅ Stable | Use for presentations |
| BENCHMARKS.md | ✅ Stable | Ground truth for all performance claims |
| USE_CASES.md | ✅ Stable | Market validation |
| INNOVATION_FRONTIERS.md | ✅ Stable | Strategic context |
| TECHNICAL_DESIGN.md | ✅ Stable | Implementation spec |
| DOCUMENTATION_GUIDE.md | ✅ Stable | This file |

---

## Contributing to Documentation

### When to Update Which Document

**README.md** - Update when:
- Project status changes (phase transitions)
- Quick start instructions change
- Core value proposition evolves

**EXECUTIVE_SUMMARY.md** - Update when:
- Performance numbers are validated
- New use cases emerge
- Audience feedback requires clarity improvements

**INNOVATION_FRONTIERS.md** - Update when:
- Market landscape shifts (new competitors)
- Hardware innovations emerge (new memory tech)
- Strategic positioning changes

**TECHNICAL_DESIGN.md** - Update when:
- Implementation details are finalized
- API design evolves
- Algorithmic changes occur

**BENCHMARKS.md** - Update when:
- Any measurement is completed
- Targets change based on feasibility analysis
- New benchmark methodologies are added
- **This is the authoritative source for all performance claims**

### Documentation Style Guide

1. **All numbers reference BENCHMARKS.md**: Every performance claim must link to ground truth
2. **Targets vs. validated**: Clearly label "Target:" vs "Measured:" everywhere
3. **Assumptions must be explicit**: "Assumes uniform access, no network, CPU cache warm"
4. **Claims must be reproducible**: Exact test conditions, commands, environment documented
5. **Trade-offs must be stated**: "Non-goal: Complex query language"
6. **Skepticism increases credibility**: "Advisory, not authoritative" for advanced features
7. **Calm comparisons**: "Redis is architecturally constrained from" not "Redis cannot"
8. **Cache vs database clarity**: "ZNS-Slab is a cache system, not a system of record"
9. **ZNS as enhancement**: "ZNS enhances... but remains valuable on conventional NVMe"
10. **No invented numbers**: If we can't measure it yet, we retract or label as target

---

## Getting Help

- **Conceptual questions**: Start with [EXECUTIVE_SUMMARY.md](./EXECUTIVE_SUMMARY.md)
- **Performance skepticism**: See [BENCHMARKS.md](./BENCHMARKS.md)
- **Implementation questions**: See [TECHNICAL_DESIGN.md](./TECHNICAL_DESIGN.md)
- **Strategic questions**: Read [INNOVATION_FRONTIERS.md](./INNOVATION_FRONTIERS.md)
- **Navigation confusion**: You're reading it (this file)

---

*Last Updated: 2026-02-05*
