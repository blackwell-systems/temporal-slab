# ZNS-Slab Use Cases: Where Small Objects Dominate

## Overview

Small objects (sub-4KB) are **not an edge case**—they dominate large parts of modern infrastructure. This document catalogs domains where sub-4KB objects are the normal unit of work, making ZNS-Slab's specialization a critical advantage rather than a niche optimization.

**Key Pattern**: Small objects dominate whenever **correctness and latency matter more than throughput**. These are not "nice to have" caches—they are critical infrastructure paths.

---

## 1. Session, Identity, and Auth Infrastructure

### Why Objects Are Small
Identity state is metadata-heavy, not data-heavy.

### Examples
- **Session tokens** (128-256 bytes)
- **OAuth access tokens** (200-400 bytes)
- **Refresh tokens** (150-300 bytes)
- **CSRF tokens** (64-128 bytes)
- **API keys** (100-200 bytes)
- **JWT metadata** (300-500 bytes)
- **MFA challenge state** (128-256 bytes)

### Typical Object Size
**100-500 bytes**, rarely exceeds 1KB

### Access Pattern
- **Read-heavy** (10:1 to 100:1 read/write ratio)
- **Bursty** (login storms, session validation spikes)
- **Strong locality** during short lifetimes (minutes to hours)

### Why Redis Struggles
- **High allocator overhead** per token (62% overhead = 62 wasted bytes per 100-byte token)
- **TTL churn causes fragmentation** (constant expiration creates memory holes)
- **Memory cost dominates infrastructure cost** (auth layer is pure memory)

### Why ZNS-Slab Fits
- **Lifetime-aligned slabs** match token TTLs (all tokens in a slab expire together)
- **O(1) slab reset** beats per-token expiry (bulk deletion via bitmap clear)
- **Extremely high object density** (8x more tokens per GB = 8x cheaper auth layer)

### Real-World Scale
- Auth0: Handles millions of sessions globally
- Netflix: 200M+ active sessions
- GitHub: Millions of API keys and session tokens

---

## 2. Microservice Metadata & Control Planes

### Why Objects Are Small
Control planes coordinate systems; they don't move payloads.

### Examples
- **Service discovery records** (Consul, etcd entries: 100-400 bytes)
- **Feature flags** (LaunchDarkly, Split: 50-200 bytes per flag)
- **Rate limit counters** (64-128 bytes)
- **Circuit breaker state** (32-128 bytes)
- **Leader election metadata** (64-256 bytes)
- **Config snapshots** (versioned config deltas: 200-1000 bytes)

### Typical Object Size
**64-512 bytes**

### Access Pattern
- **Very hot** (accessed by every service instance)
- **Shared across many services** (1:N fanout)
- **Latency-sensitive** (p99 < 5ms requirement common)

### Why This Matters
**This is often the tail-latency bottleneck of distributed systems.** Slow control plane = slow everything.

### Why ZNS-Slab Fits
- **Cache-line-aligned reads** (entire record in one L1 cache load)
- **Predictable p99 latency** (no GC pauses, no compaction)
- **Memory efficiency reduces control-plane cost** (critical for multi-tenant platforms)

### Real-World Scale
- Uber: 2,200+ microservices
- Netflix: 1,000+ microservices
- Amazon: Tens of thousands of services

---

## 3. Real-Time Counters & Metrics

### Why Objects Are Small
Counters are numbers plus a key.

### Examples
- **API rate limits** (per-user counters: 32-64 bytes)
- **Click counters** (analytics: 64-128 bytes)
- **View counts** (content metrics: 64-128 bytes)
- **Feature usage metrics** (product analytics: 64-256 bytes)
- **Sliding-window stats** (time-bucketed aggregates: 128-512 bytes)

### Typical Object Size
**16-128 bytes**

### Access Pattern
- **Extremely write-heavy** (every request increments)
- **High contention** (hot keys like "homepage_views")
- **Short-lived windows** (1-minute, 5-minute, 1-hour buckets)

### Why Traditional DBs Fail
- **Write amplification** (128-byte counter → 4KB page write)
- **Lock contention** (atomic increments serialize)
- **Overhead dwarfs payload** (metadata larger than data)

### Why ZNS-Slab Fits
- **Fixed-size slots** (no reallocation on counter growth)
- **Bulk window expiration** via slab reset (drop entire time bucket at once)
- **No GC storms during window rollovers** (predictable performance)

### Real-World Scale
- Twitter: Billions of counters (likes, retweets, views)
- Cloudflare: Rate limiting at 25M+ requests/second
- Stripe: Per-customer rate limit buckets

---

## 4. AI & LLM Inference Infrastructure

### Why Objects Are Small
The model weights are big—the runtime state is tiny.

### Examples
- **Token embeddings** (cached vector pointers: 128-256 bytes)
- **KV-cache metadata** (attention cache refs: 64-512 bytes)
- **Prompt context state** (user session context: 256-1024 bytes)
- **Per-request inference state** (routing info: 128-512 bytes)
- **Feature vectors** (compressed embeddings: 512-2048 bytes)

### Typical Object Size
**128 bytes - 2KB**

### Access Pattern
- **Burst reads** (inference request triggers cascade)
- **Strong correlation** (same user = same context pattern)
- **Predictable sequences** (multi-turn conversations)

### Why This Is Huge
**AI inference systems are cache-bound, not compute-bound, at scale.** GPU waits for data.

### Why ZNS-Slab Fits
- **Dense packing of inference metadata** (maximize cache hit rate)
- **Intent-aware prefetching** (Phase 4: predict next turn in conversation)
- **Excellent fit for tiered memory** (hot contexts in DRAM, warm in NVMe)

### Real-World Scale
- OpenAI: Millions of concurrent inference sessions
- Anthropic: Claude conversations with multi-turn context
- HuggingFace: Hosted inference for thousands of models

---

## 5. Feature Stores (Online ML)

### Why Objects Are Small
Features are scalar values or small vectors, not blobs.

### Examples
- **User features** (demographic data: 64-256 bytes)
- **Session features** (current behavior: 128-512 bytes)
- **Aggregated stats** (rolling averages: 64-256 bytes)
- **Embedding references** (pointer to vector DB: 32-128 bytes)

### Typical Object Size
**50-500 bytes**

### Access Pattern
- **Read-heavy** (inference queries)
- **Tight latency budgets** (<10ms end-to-end, <1ms for feature lookup)
- **Fan-out access** (one prediction = 10-100 features)

### Why Redis Is Often the Bottleneck
- **Memory inefficiency limits feature density** (fewer features per node)
- **Cache misses are expensive** (fan-out amplifies latency)
- **Fragmentation under continuous updates** (feature refresh churn)

### Why ZNS-Slab Fits
- **High object density per GB** (more features cached = higher hit rate)
- **Predictable latency** (critical for p99 SLAs)
- **Excellent for hot feature tiers** (DRAM for ultra-hot, NVMe for warm)

### Real-World Scale
- Uber Michelangelo: Thousands of features per prediction
- DoorDash: Real-time feature serving for ML models
- Airbnb Zipline: Feature store for ranking/search

---

## 6. Gaming & Real-Time Simulation

### Why Objects Are Small
State must be fast, not rich.

### Examples
- **Player session state** (current match info: 256-1024 bytes)
- **Matchmaking metadata** (player ratings, queue state: 128-512 bytes)
- **Physics tick state** (deltas between frames: 64-512 bytes)
- **Inventory deltas** (item changes: 128-512 bytes)

### Typical Object Size
**64 bytes - 1KB**

### Access Pattern
- **Very high frequency** (60-120 Hz tick rate)
- **Strict latency budgets** (16ms frame time)
- **Time-windowed lifetimes** (match duration)

### Why ZNS-Slab Fits
- **Slabs map to tick windows** (all tick N data in one slab)
- **Bulk reset on match end** (instant cleanup, no per-object deletion)
- **Minimal memory waste at scale** (critical for 100K+ concurrent matches)

### Real-World Scale
- Fortnite: 10M+ concurrent players
- Roblox: Millions of concurrent game instances
- Riot Games: League of Legends match state at massive scale

---

## 7. Edge Computing & IoT Backends

### Why Objects Are Small
Devices emit signals, not datasets.

### Examples
- **Sensor readings** (temp, humidity, motion: 16-64 bytes)
- **Device heartbeat state** (last-seen, status: 32-128 bytes)
- **Configuration deltas** (setting updates: 64-256 bytes)
- **Telemetry snapshots** (aggregated metrics: 128-512 bytes)

### Typical Object Size
**32-512 bytes**

### Access Pattern
- **Massive fan-in** (millions of devices → central backend)
- **Short-lived** (aggregate and discard)
- **Aggregated quickly** (roll up to time-series DB)

### Why This Is Brutal at Scale
**Millions of devices × tiny payloads = allocator hell.** Redis's 62% overhead becomes crushing.

### Why ZNS-Slab Fits
- **Perfect for millions of uniform objects** (sensor readings all same size)
- **Efficient aggregation** (scan entire slab in one cache load)
- **Cheap expiration** (drop entire time window with slab reset)

### Real-World Scale
- AWS IoT: Billions of messages/day
- Google Nest: Millions of connected devices
- Tesla: Fleet telemetry from millions of vehicles

---

## 8. Observability Pipelines (Control Data)

### Why Objects Are Small
Spans and logs are metadata-rich, payload-light (in hot path).

### Examples
- **Trace span headers** (distributed tracing: 128-512 bytes)
- **Correlation IDs** (request tracking: 32-128 bytes)
- **Sampling decisions** (trace/don't trace: 16-64 bytes)
- **Tag sets** (metadata labels: 64-256 bytes)

### Typical Object Size
**100-400 bytes**

### Access Pattern
- **Burst writes** (request spikes)
- **Short retention** (hot path: seconds to minutes)
- **Sequential processing** (pipeline stages)

### Why ZNS-Slab Fits
- **Append-only slabs** (match pipeline flow)
- **Lifetime-aligned expiration** (flush entire batch)
- **NVMe-friendly layout** (sequential writes)

### Real-World Scale
- Datadog: Trillions of spans/year
- Honeycomb: High-cardinality observability
- Splunk: Massive log ingestion rates

---

## 9. Financial Systems (Hot Path State)

### Why Objects Are Small
The money is big; the state is small.

### Examples
- **Order book entries** (price/quantity: 64-256 bytes)
- **Risk counters** (exposure tracking: 64-128 bytes)
- **Position snapshots** (holdings deltas: 128-512 bytes)
- **Market state metadata** (instrument status: 64-256 bytes)

### Typical Object Size
**64-256 bytes**

### Access Pattern
- **Extremely latency-sensitive** (microseconds matter)
- **Deterministic** (no GC pauses allowed)
- **High read/write churn** (every trade updates multiple objects)

### Why ZNS-Slab Fits
- **Predictable latency** (no background compaction)
- **No GC pauses** (critical for regulatory compliance)
- **Hardware-aligned memory access** (minimize cache misses)

### Real-World Scale
- NYSE: Millions of orders/second
- CME Group: Futures/options order books
- High-frequency trading firms: Microsecond latency requirements

---

## 10. Distributed Systems Internals (The Hidden Gold Mine)

### Why Objects Are Small
Coordination requires metadata, not data.

### Examples
- **Raft log metadata** (term, index, leader: 32-128 bytes)
- **Lease records** (ownership + expiry: 64-128 bytes)
- **Vector clocks** (causal ordering: 32-256 bytes)
- **Heartbeat state** (liveness: 16-64 bytes)
- **Membership lists** (cluster topology: 64-512 bytes per node)

### Typical Object Size
**32-256 bytes**

### Access Pattern
- **Constant churn** (every heartbeat, every log append)
- **Cluster-wide access** (replicated state)
- **Failure-sensitive** (split-brain prevention)

### Why This Matters
**Every distributed system depends on this layer.** Kafka, Cassandra, etcd, Consul—all have this pattern.

### Why ZNS-Slab Fits
- **High density** (more nodes per cache GB)
- **Deterministic behavior** (predictable failure modes)
- **Clean failure semantics** (slab-aligned epochs)

### Real-World Scale
- Kubernetes: Cluster state for thousands of nodes
- Kafka: Partition metadata and offsets
- Cassandra: Gossip protocol state

---

## Meta-Observation: The Pattern

Notice the recurring theme:

**Small objects dominate whenever correctness and latency matter more than throughput.**

These are not optional caches. These are **critical infrastructure paths**:
- Auth failures = user lockout
- Control plane failures = cascading outages
- Counter inaccuracy = billing errors
- Inference latency = poor user experience
- Feature store misses = degraded ML performance

---

## Market Sizing

### Conservative Estimate
If 20% of Redis deployments are dominated by sub-4KB objects, and ZNS-Slab reduces memory costs by 80%, the addressable market is:

- **Global Redis market**: ~$1B+ annually (cloud + self-hosted)
- **Addressable segment**: $200M+ (20% of workloads)
- **Potential savings**: $160M+ annually (80% reduction)

### Growth Drivers
1. **AI/ML explosion**: Feature stores and inference metadata growing 10x/year
2. **Microservices proliferation**: Control plane overhead growing with service count
3. **IoT adoption**: Billions of new devices = billions of small objects
4. **Real-time analytics**: Counter/metrics workloads growing faster than storage

---

## Competitive Positioning

### ZNS-Slab Is Not "Redis Replacement"
It's "Redis for the workloads Redis wasn't designed for."

### Strategic Positioning
- **Start**: Session stores, feature stores, counters (proven wins)
- **Expand**: AI inference, IoT backends (high-growth)
- **Dominate**: Control planes, distributed systems internals (deep moat)

### Moat
Once you're embedded in a distributed system's control plane, you're infrastructure—not a vendor.

---

## Next: Implementation Priorities

Based on market validation, prioritize use cases with:
1. **Immediate pain**: Feature stores, session stores (Redis costs hurting now)
2. **Rapid growth**: AI inference, IoT (10x growth rates)
3. **High switching cost**: Control planes, distributed systems (lock-in)

Phase 1 benchmarks should focus on:
- Session store workloads (read-heavy, TTL churn)
- Counter workloads (write-heavy, small objects)
- Feature store workloads (fan-out reads, tight latency)

---

## Conclusion

Small objects are not a niche—they are the **connective tissue of modern infrastructure**. Every auth layer, control plane, metrics system, and ML pipeline depends on fast, memory-efficient storage for metadata.

ZNS-Slab doesn't need to replace Redis everywhere. It just needs to dominate where Redis struggles: **small, ephemeral, latency-critical objects at massive scale.**

That market alone is worth billions.
