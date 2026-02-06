# ZNS-Slab: Technical Design Document

## Executive Summary

ZNS-Slab is a specialized cache system optimized for sub-4KB objects, designed to outperform Redis through slab allocation, cache-line optimization, and eventual intent-aware eviction. 

**Positioning**: ZNS-Slab is a cache system, not a system of record. It optimizes ephemeral storage (sessions, tokens, counters) rather than durable persistence.

Target: **4x faster access** and **20x better memory efficiency** for small objects.

---

## Non-Goals

This system explicitly does **not** aim to be:

1. **General-purpose database replacement** - ZNS-Slab is a cache system, not a system of record
2. **Complex query language** - Key-value operations only; no SQL, no aggregations
3. **Full transactional semantics** - No multi-key transactions, no distributed commits
4. **Universal workload support** - Optimized for small objects only (sub-4KB)

**Why This Matters**: These constraints are deliberate. They enable the specialization that makes ZNS-Slab faster than general-purpose systems. This is not a limitation—it's architectural leverage.

---

## Design Principles

1. **Cache-First Architecture**: Optimize for CPU L1/L2 cache hits above all else
2. **Zero Fragmentation**: Fixed-size slabs eliminate memory holes
3. **Page Alignment**: Match OS and hardware boundaries (4KB pages, 64B cache lines)
4. **Predictable Performance**: O(1) operations with minimal variance
5. **Specialization Over Generality**: Dominate the small-object niche

---

## Memory Layout

### Slab Structure

```
┌──────────────────────────────────────────────────────────┐
│                    Slab Header (64 bytes)                 │
├──────────────────────────────────────────────────────────┤
│  Magic (4B) │ Version (4B) │ Object Size (4B) │ Count (4B)│
│  Free Bitmap Offset (8B) │ Next Slab (8B) │ Reserved (24B)│
├──────────────────────────────────────────────────────────┤
│                 Free Bitmap (128 bytes)                   │
│          (1024 bits for up to 1024 objects)               │
├──────────────────────────────────────────────────────────┤
│                    Object Slot 0                          │
│                  (size varies: 64-512B)                   │
├──────────────────────────────────────────────────────────┤
│                    Object Slot 1                          │
├──────────────────────────────────────────────────────────┤
│                       ...                                 │
├──────────────────────────────────────────────────────────┤
│                    Object Slot N                          │
└──────────────────────────────────────────────────────────┘

Total Size: 4096 bytes (one OS page)
```

### Slab Size Classes

| Class  | Object Size | Objects/Slab | Per-Object Overhead | Efficiency |
|--------|-------------|--------------|---------------------|------------|
| Tiny   | 64B         | 61           | 3.1 bytes (4.9%)    | 95.3%      |
| Small  | 128B        | 30           | 6.4 bytes (5.0%)    | 93.8%      |
| Medium | 256B        | 15           | 12.8 bytes (5.0%)   | 93.8%      |
| Large  | 512B        | 7            | 27.4 bytes (5.4%)   | 87.5%      |

**Calculations**:
- `Objects/Slab = floor((4096 - 192) / Object Size)`  where 192 = Header (64B) + Bitmap (128B)
- `Efficiency = (Objects × Size) / 4096 bytes`
- `Overhead = 192 / Objects`

**See [BENCHMARKS.md](./BENCHMARKS.md) for detailed analysis and validation plan.**

---

## Core Data Structures

### 1. Slab Allocator

```go
type SlabAllocator struct {
    sizeClass    int           // 64, 128, 256, or 512 bytes
    totalSlabs   int
    freeSlabs    *SlabList     // Linked list of slabs with free slots
    fullSlabs    *SlabList     // Linked list of full slabs
    mmapRegion   []byte        // Memory-mapped region
    lock         sync.RWMutex
}

type Slab struct {
    header       SlabHeader
    freeBitmap   [128]byte     // Fast bitmap allocation
    data         []byte        // Actual object storage
}

type SlabHeader struct {
    magic        uint32        // 0x534C4142 ("SLAB")
    version      uint32
    objectSize   uint32
    objectCount  uint32
    freeCount    uint32
    bitmapOffset uint64
    nextSlab     *Slab
    _            [24]byte      // Reserved for future use
}
```

### 2. Hash Table (Cache)

```go
type CacheEntry struct {
    keyHash      uint64        // 64-bit hash of key
    slabPtr      *Slab         // Pointer to slab
    slotIndex    uint16        // Index within slab
    accessCount  uint32        // For LRU/LFU
    lastAccess   uint64        // Timestamp (nanoseconds)
    _            [6]byte       // Padding to 32 bytes
}

type HashTable struct {
    buckets      []CacheBucket // Power-of-2 size
    size         uint64
    count        uint64
    lock         []sync.RWMutex // Per-bucket locking
}

type CacheBucket struct {
    entries      []*CacheEntry
    lock         sync.RWMutex
}
```

### 3. Memory Pool

```go
type MemoryPool struct {
    slabPools    map[int]*SlabAllocator // Key = size class
    totalMemory  uint64
    usedMemory   uint64
    pageSize     int                    // 4096
    lock         sync.RWMutex
}
```

---

## Key Operations

### Allocation Algorithm

```
ALLOCATE(size):
    1. Determine size class (round up to 64, 128, 256, or 512)
    2. Get allocator for size class
    3. Lock allocator
    4. IF freeSlabs is empty:
         a. Allocate new slab (mmap 4KB)
         b. Initialize header and bitmap
         c. Add to freeSlabs list
    5. Get first slab from freeSlabs
    6. Find first zero bit in bitmap (CTZ instruction)
    7. Set bit to 1 (mark allocated)
    8. Calculate object address: slab.data + (bitIndex × objectSize)
    9. IF slab is now full:
         Move from freeSlabs to fullSlabs
    10. Unlock allocator
    11. Return object address

Time Complexity: O(1) with high probability
Actual Time: ~50-100ns
```

### Get Operation

```
GET(key):
    1. Hash key → 64-bit hash
    2. Calculate bucket index: hash % bucketCount
    3. Lock bucket (read lock)
    4. Linear search bucket for matching hash
    5. IF found:
         a. Get slab pointer and slot index
         b. Calculate address: slab.data + (slotIndex × objectSize)
         c. Update access statistics (atomic)
         d. Unlock bucket
         e. Return data
    6. ELSE:
         a. Unlock bucket
         b. Return miss

Time Complexity: O(1) average
Actual Time: ~30-80ns (depends on bucket size)
```

### Delete Operation

```
DELETE(key):
    1. Hash key → 64-bit hash
    2. Calculate bucket index
    3. Lock bucket (write lock)
    4. Find and remove cache entry
    5. Get slab pointer and slot index
    6. Lock slab's allocator
    7. Clear bit in bitmap (mark free)
    8. IF slab was full:
         Move from fullSlabs to freeSlabs
    9. Unlock allocator
    10. Unlock bucket

Time Complexity: O(1)
Actual Time: ~100-150ns
```

---

## Cache Line Optimization

### Alignment Strategy

```
Cache Line Size: 64 bytes

Strategy:
1. Slab header: 64 bytes (one cache line)
2. Bitmap: 128 bytes (two cache lines)
3. Objects: Aligned to 64-byte boundaries where possible

Example: 128-byte objects
  - Each object = 2 cache lines
  - 31 objects = 62 cache lines
  - Total slab = 64 cache lines
  - Perfect L1 cache utilization
```

### Prefetching

```go
// Compiler hint for prefetching next object
func (s *Slab) PrefetchNext(currentIndex int) {
    nextAddr := uintptr(unsafe.Pointer(&s.data[currentIndex+1]))
    // Use x86 PREFETCHT0 instruction
    prefetch(nextAddr)
}
```

---

## Concurrency Model

### Lock-Free Reads

```go
// Atomic access statistics update
type CacheEntry struct {
    // ... other fields
    accessCount atomic.Uint32
    lastAccess  atomic.Uint64
}

func (e *CacheEntry) RecordAccess() {
    e.accessCount.Add(1)
    e.lastAccess.Store(uint64(time.Now().UnixNano()))
}
```

### Per-Bucket Locking

```
Rationale: Minimize lock contention
- 64K buckets = 65536 locks
- Probability of collision: ~1/65536 for random keys
- Allows massive parallelism

Trade-off: Memory overhead (~1KB per lock × 65K = 64MB)
Mitigation: Use smaller locks on memory-constrained systems
```

### Slab-Level Locking

```
Each SlabAllocator has ONE lock
- Only contended during allocation/deallocation
- Read operations (GET) don't lock slabs
- Write operations (PUT/DELETE) lock only briefly

Result: Reads are ~95% lock-free
```

---

## Eviction Strategy

### Phase 1: LRU (Least Recently Used)

```go
type EvictionManager struct {
    lruList      *list.List    // Doubly-linked list
    maxEntries   uint64
    currentCount atomic.Uint64
}

func (em *EvictionManager) Evict() *CacheEntry {
    // Lock LRU list
    oldest := em.lruList.Back()
    em.lruList.Remove(oldest)
    return oldest.Value.(*CacheEntry)
}
```

### Phase 2: LFU (Least Frequently Used)

```go
// Use Min-Heap for efficient LFU
type LFUHeap []*CacheEntry

func (h *LFUHeap) EvictLFU() *CacheEntry {
    // O(log n) to remove min element
    return heap.Pop(h).(*CacheEntry)
}
```

### Phase 3: Intent-Aware Prediction (Phase 4+ Feature)

**Note**: This is a Phase 4+ enhancement, not a core requirement. The slab allocator is the innovation; intent-aware eviction is an optional optimization layer.

```go
type PredictiveCache struct {
    model        *LightweightModel
    history      *AccessLog
    confidence   float32
    fallbackLRU  *EvictionManager  // CRITICAL: Graceful degradation
}

// Predict probability of access in next N seconds
func (pc *PredictiveCache) PredictAccess(key string, horizon time.Duration) float32 {
    features := pc.extractFeatures(key)
    return pc.model.Predict(features)
}

// Proactive prefetch with safety net
func (pc *PredictiveCache) PrefetchCandidates() []*CacheEntry {
    candidates := pc.model.TopKPredictions(100)
    highConfidence := filterByConfidence(candidates, 0.8)
    
    // If model confidence is low, fall back to LRU
    if len(highConfidence) < 10 {
        return pc.fallbackLRU.GetCandidates()
    }
    return highConfidence
}
```

**Critical Design Constraint**: Intent-aware eviction is *advisory, not authoritative*. Incorrect predictions degrade gracefully to LRU behavior, not cache thrashing.

---

## Performance Targets

### Latency (p99)

| Operation | Redis | ZNS-Slab | Target Improvement |
|-----------|-------|----------|-------------------|
| GET       | 200ns | 50ns     | 4x faster         |
| SET       | 300ns | 100ns    | 3x faster         |
| DELETE    | 250ns | 80ns     | 3x faster         |

### Throughput

| Workload | Redis | ZNS-Slab | Target Improvement |
|----------|-------|----------|-------------------|
| Read-heavy (90% GET) | 500K ops/s | 2M ops/s | 4x |
| Balanced (50/50) | 300K ops/s | 1M ops/s | 3.3x |
| Write-heavy (90% SET) | 200K ops/s | 500K ops/s | 2.5x |

### Memory Efficiency

| Metric | Redis | ZNS-Slab | Improvement |
|--------|-------|----------|-------------|
| 128B object overhead | 62% | 3% | 20x better |
| Objects per GB | 4M | 32M | 8x density |
| Fragmentation | High | Near-zero | 10x better |

**Workload Assumptions**: Benchmarks assume fixed-size objects, uniform access distribution, and pre-allocated slabs. Real-world performance will vary based on access patterns and key distribution.

---

## Benchmarking Strategy

### Microbenchmarks

```go
func BenchmarkGet(b *testing.B) {
    cache := NewZNSCache(128) // 128-byte objects
    // Pre-populate
    for i := 0; i < 1000000; i++ {
        key := fmt.Sprintf("key%d", i)
        cache.Set(key, randomData(128))
    }
    
    b.ResetTimer()
    b.RunParallel(func(pb *testing.PB) {
        for pb.Next() {
            key := fmt.Sprintf("key%d", rand.Intn(1000000))
            cache.Get(key)
        }
    })
}
```

### Real-World Workloads

1. **Session Store**: Small session tokens (256B), high read rate
2. **Cache Warming**: Bulk load 10M objects, measure time
3. **Zipf Distribution**: Realistic access patterns (80/20 rule)
4. **Thundering Herd**: 1000 concurrent clients, measure tail latency

---

## Tiered Storage Integration

### Architecture

```
┌─────────────────────────────────────────┐
│         In-Memory Slabs (DRAM)          │
│           Hot Data (L0 Cache)           │
│              ~1GB capacity              │
├─────────────────────────────────────────┤
│        Memory-Mapped Slabs (NVMe)       │
│          Warm Data (L1 Cache)           │
│             ~100GB capacity             │
├─────────────────────────────────────────┤
│      Compressed Archive (SSD/HDD)       │
│          Cold Data (L2 Cache)           │
│              ~10TB capacity             │
└─────────────────────────────────────────┘
```

**Design Goal**: ZNS-Slab optimizes for *predictable latency bands*, not absolute speed.

### Cost/Latency Model

| Tier | Latency | Cost/GB | Intended Use |
|------|---------|---------|-------------|
| DRAM | ~100ns | $$$$ | Hot, mutable data |
| PMEM | ~300ns | $$$ | Warm, stable data |
| NVMe | ~20µs | $$ | Read-heavy, append-only |
| HDD | ~5ms | $ | Cold archival |

**About ZNS**: ZNS (Zoned Namespace) NVMe enhances deletion efficiency through hardware zone resets and eliminates write amplification, but ZNS-Slab's core benefits (fragmentation elimination, cache locality) apply to conventional NVMe as well.

### Promotion/Demotion Logic

```go
type TieredCache struct {
    l0 *MemoryPool  // DRAM
    l1 *MmapPool    // NVMe
    l2 *ArchivePool // Disk
}

func (tc *TieredCache) PromoteIfHot(key string) {
    entry := tc.l1.Get(key)
    if entry.AccessCount > HOT_THRESHOLD {
        tc.l0.Set(key, entry.Data)
        tc.l1.Delete(key)
    }
}

func (tc *TieredCache) DemoteIfCold(key string) {
    entry := tc.l0.Get(key)
    if time.Since(entry.LastAccess) > COLD_THRESHOLD {
        tc.l1.Set(key, entry.Data)
        tc.l0.Delete(key)
    }
}
```

---

## API Design

### Core Interface

```go
type Cache interface {
    // Basic operations
    Get(key string) ([]byte, bool)
    Set(key string, value []byte) error
    Delete(key string) bool
    
    // Batch operations
    MultiGet(keys []string) map[string][]byte
    MultiSet(entries map[string][]byte) error
    
    // Metadata
    Size() uint64
    Stats() CacheStats
    
    // Lifecycle
    Flush() error
    Close() error
}

type CacheStats struct {
    HitRate       float64
    MissRate      float64
    TotalRequests uint64
    TotalHits     uint64
    TotalMisses   uint64
    EvictionCount uint64
    MemoryUsed    uint64
    ObjectCount   uint64
    AvgLatency    time.Duration
}
```

### Advanced Features

```go
// TTL support
func (c *Cache) SetWithTTL(key string, value []byte, ttl time.Duration) error

// Compare-and-swap
func (c *Cache) CAS(key string, old, new []byte) bool

// Atomic increment
func (c *Cache) Incr(key string, delta int64) (int64, error)

// Scan operations
func (c *Cache) Scan(pattern string) Iterator
```

---

## Testing Strategy

### Unit Tests
- Slab allocation/deallocation
- Bitmap operations (set, clear, find)
- Hash table operations
- Concurrency correctness

### Integration Tests
- Multi-threaded workloads
- Memory leak detection (valgrind)
- Crash recovery
- Tiered storage transitions

### Stress Tests
- Sustained high load (1M ops/s for 1 hour)
- Memory pressure (allocate until OOM)
- Concurrent churn (rapid alloc/free)

### Comparison Tests
- Redis benchmark suite
- Memcached benchmark suite
- Custom workloads (session store, counters)

---

## Deployment Considerations

### Configuration

```yaml
cache:
  sizeClasses: [64, 128, 256, 512]
  maxMemory: 1GB
  evictionPolicy: lru
  
  # Tiered storage
  tiers:
    l0:
      type: memory
      size: 1GB
    l1:
      type: nvme
      size: 100GB
      path: /mnt/nvme/cache
    l2:
      type: disk
      size: 1TB
      path: /mnt/disk/cache
  
  # Monitoring
  metrics:
    enabled: true
    port: 9090
    interval: 10s
```

### Monitoring

```
Metrics to expose:
- cache_hit_rate
- cache_miss_rate  
- cache_eviction_rate
- cache_memory_used_bytes
- cache_object_count
- cache_get_latency_seconds (histogram)
- cache_set_latency_seconds (histogram)
- cache_slab_utilization (by size class)
```

---

## Future Enhancements

### 1. Persistent Slabs
- Write slabs to disk on shutdown
- Memory-map on startup
- Instant cache warming

### 2. Distributed Mode
- Consistent hashing across nodes
- Replication for high availability
- RDMA for zero-copy transfers

### 3. Compression
- Compress cold slabs
- Transparent decompression
- Trade CPU for memory savings

### 4. Advanced Analytics
- Real-time query language
- Aggregation support
- Time-series optimizations

---

## Conclusion

ZNS-Slab represents a fundamental rethinking of cache-based databases for the modern hardware landscape. By specializing in small objects and optimizing for CPU cache behavior, it can achieve performance levels impossible for general-purpose systems.

**Key Innovation**: Perfect packing of sub-4KB objects into cache-aligned slabs, eliminating fragmentation and maximizing L1 cache hits.

**Market Position**: The high-performance cache for microservices, session stores, real-time counters, and AI inference serving.

**Resume Impact**: Demonstrates deep systems knowledge spanning memory hierarchies, concurrency, hardware optimization, and database internals.
