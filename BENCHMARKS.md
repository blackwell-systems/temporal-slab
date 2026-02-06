# ZNS-Slab: Benchmarks and Performance Claims

## Status: Phase 1 (Targets Only)

**Current Status**: All numbers below are **design targets** for Phase 1. No validated benchmarks exist yet. This document will be updated as measurements are completed.

---

## Canonical Slab Layout

All overhead calculations are based on this fixed layout:

```
Slab Structure (4096 bytes total):
├─ Header:  64 bytes  (magic, version, metadata)
├─ Bitmap: 128 bytes  (allocation tracking, up to 1024 bits)
└─ Data:  3904 bytes  (object storage)
```

### Size Classes

| Class  | Object Size | Objects/Slab | Bitmap Used | Data Used | Efficiency |
|--------|-------------|--------------|-------------|-----------|------------|
| Tiny   | 64B         | 61           | 8 bytes     | 3904B     | 95.3%      |
| Small  | 128B        | 30           | 4 bytes     | 3840B     | 93.8%      |
| Medium | 256B        | 15           | 2 bytes     | 3840B     | 93.8%      |
| Large  | 512B        | 7            | 1 byte      | 3584B     | 87.5%      |

**Calculation**: 
- `Objects/Slab = floor((4096 - 192) / Object Size)`
- `Efficiency = (Objects × Object Size) / 4096`
- `Per-object overhead = 192 / Objects`

**Per-object overhead**: 
- Tiny (64B): 192 / 61 = 3.1 bytes (4.9% overhead)
- Small (128B): 192 / 30 = 6.4 bytes (5.0% overhead)
- Medium (256B): 192 / 15 = 12.8 bytes (5.0% overhead)
- Large (512B): 192 / 7 = 27.4 bytes (5.4% overhead)

---

## Performance Targets (Phase 1)

### Memory Efficiency Targets

| Metric | Target | Measurement Criteria |
|--------|--------|---------------------|
| Overhead (128B objects) | <5% | (Header + Bitmap) / Total Slab Size |
| Objects per 4KB page | 30+ | Based on canonical layout |
| Fragmentation | 0% | Fixed-size slots, no holes |

**Workload**: 1M uniform 128-byte objects, pre-allocated slabs, no eviction.

### Latency Targets (Single-Threaded)

| Operation | Target (p50) | Target (p99) | Conditions |
|-----------|--------------|--------------|------------|
| Allocate | <50ns | <100ns | Slab with free slots |
| Free | <30ns | <80ns | Bitmap bit clear only |
| Get (direct) | <20ns | <50ns | In-process, no hash, pointer known |

**Conditions**: 
- Single-threaded micro-benchmark
- No network, no protocol parsing
- Key already hashed, slab pointer known
- CPU cache warm (L1 hit)

---

## Redis Baseline Analysis (For Comparison)

### Redis Memory Overhead (Estimated)

Based on Redis object model (version 7.x, default encoding):

```
Redis String Object (128-byte value):
├─ redisObject header:  16 bytes  (type, encoding, LRU, refcount)
├─ SDS header:          ~8 bytes  (len, alloc, flags)
├─ Value data:         128 bytes
├─ Dict entry:         ~24 bytes  (key ptr, val ptr, next ptr)
├─ Key string:         ~variable (not counted here)
└─ jemalloc overhead:  ~10-20%   (size class alignment)

Total (value only): ~176+ bytes
Overhead: ~37%+ (not including key)
```

**Note**: This is an **estimate** based on Redis source code inspection. Actual overhead depends on:
- Redis version
- Encoding (raw, embstr, int)
- Allocator (jemalloc, libc malloc)
- Key length and pattern
- Hash table load factor

**We will measure this empirically in Phase 1 benchmarks.**

### Previous "62% Overhead" Claim - RETRACTED

The "62% overhead" figure cited in early documentation was based on incomplete analysis. We are removing this claim until we have reproducible measurements.

**Revised claim**: Redis incurs significant per-object overhead (estimated 30-50%+ for small strings) due to object headers, encoding metadata, dict entries, and allocator alignment. ZNS-Slab targets <5% overhead through slab allocation.

---

## Benchmark Methodology (Planned)

### Phase 1: Memory Efficiency Benchmark

**Goal**: Measure actual memory overhead for 1M 128-byte objects.

**Test Cases**:
1. **Baseline - stdlib malloc**
   ```c
   for (i = 0; i < 1M; i++) {
       ptrs[i] = malloc(128);
       memset(ptrs[i], 0xAA, 128);
   }
   // Measure RSS via /proc/self/statm
   ```

2. **Baseline - Redis**
   ```bash
   # Load 1M 128-byte strings via redis-cli
   for i in {1..1000000}; do
       redis-cli SET "key$i" "$(head -c 128 /dev/urandom | base64)"
   done
   # Measure INFO memory used_memory
   ```

3. **ZNS-Slab**
   ```c
   slab_pool_t *pool = slab_pool_create(128); // 128B objects
   for (i = 0; i < 1M; i++) {
       ptrs[i] = slab_alloc(pool);
       memset(ptrs[i], 0xAA, 128);
   }
   // Measure RSS via /proc/self/statm
   ```

**Metrics**:
- RSS (Resident Set Size) in KB
- Actual bytes allocated
- Overhead percentage: `(RSS - 128MB) / 128MB * 100`

**Environment**:
- CPU: (TBD - will document actual test machine)
- RAM: (TBD)
- OS: Linux kernel 5.x+
- Allocator: glibc malloc 2.x / jemalloc 5.x
- Compiler: GCC/Clang with -O2

### Phase 1: Latency Micro-Benchmark

**Goal**: Measure allocation/free latency.

**Test Method**:
```c
uint64_t start = rdtsc();
for (int i = 0; i < 1M; i++) {
    void *p = slab_alloc(pool);
    latencies[i] = rdtsc() - start;
    start = rdtsc();
}
// Calculate p50, p99, p999
```

**Reported Metrics**:
- p50, p99, p999 latency in nanoseconds
- CPU cycles per operation
- Cache miss rate (via perf stat)

---

## Redis GET Latency Comparison

### Important Context

**Apples-to-oranges alert**: Redis GET and ZNS-Slab direct pointer access are NOT equivalent operations.

| Feature | Redis GET | ZNS-Slab (direct) |
|---------|-----------|-------------------|
| Network | TCP/Unix socket | None (in-process) |
| Protocol | RESP parsing | None |
| Hash lookup | Yes | Not included |
| Serialization | Yes | None |
| Concurrency | Multi-client | Single-thread only |

**Fair comparison** would be:
- Redis: `redis-benchmark -t GET` (includes all overhead)
- ZNS-Slab: Full KV store with hash table + protocol (Phase 3+)

**Unfair comparison** (what early docs implied):
- Redis: `redis-benchmark -t GET` (~200ns including network)
- ZNS-Slab: Raw slab pointer dereference (~20ns, no network/protocol)

### Revised Latency Claims

**Phase 1 (Allocator Only)**:
- Slab allocation: Target <100ns p99 (vs malloc ~150-300ns)
- This is 2-3x faster, not 4x

**Phase 3 (Full KV Store)**:
- GET operation (in-process, no network): Target <500ns p99
- Compare to Redis GET (Unix socket loopback): ~50-100μs
- This is 100-200x faster, but includes network elimination

**We will clearly separate "allocator" vs "full KV store" benchmarks.**

---

## Validated Results (Empty - Will Update)

### Phase 1 Memory Efficiency (Pending)

```
Test: 1M 128-byte objects
Date: (not yet run)
Platform: (TBD)

Results:
- malloc:    ??? MB RSS, ??% overhead
- Redis:     ??? MB used_memory, ??% overhead  
- ZNS-Slab:  ??? MB RSS, ??% overhead

Conclusion: (pending measurement)
```

### Phase 1 Latency (Pending)

```
Test: Allocation latency, 1M operations
Date: (not yet run)
Platform: (TBD)

Results:
- malloc:    p50 ???ns, p99 ???ns
- ZNS-Slab:  p50 ???ns, p99 ???ns

Conclusion: (pending measurement)
```

---

## Update Policy

This document will be updated as follows:

1. **Before Phase 1 implementation**: Targets and methodology only
2. **During Phase 1 development**: Preliminary results with "unstable" label
3. **Phase 1 completion**: Validated results with reproducible benchmarks
4. **Each subsequent phase**: Add new benchmark categories

**All claims in other documentation must reference this file.**

---

## Reproducibility

### Benchmark Repository

(Will be created in Phase 1)

```bash
benchmarks/
├── baseline/
│   ├── malloc_test.c          # stdlib malloc baseline
│   ├── redis_load.sh          # Redis memory test script
│   └── README.md              # Setup instructions
├── phase1/
│   ├── slab_alloc_test.c      # ZNS-Slab allocator test
│   ├── slab_latency_test.c    # Latency micro-benchmark
│   └── run_benchmarks.sh      # Automated test runner
└── results/
    └── (benchmark outputs will go here)
```

### Running Benchmarks

(Instructions will be added when implementation exists)

---

## FAQ: Numbers in Documentation

**Q: Why did you cite "62% overhead" earlier?**  
A: That was based on incomplete analysis. We've retracted it and will replace with measured data.

**Q: Can I cite the "4x faster" claim?**  
A: Not yet. Use "targeting 2-3x faster allocation" for Phase 1. Full KV comparison requires Phase 3.

**Q: What about "20x memory efficiency"?**  
A: This was aspirational. Realistic target for Phase 1 is 2-3x better than malloc, 3-5x better than Redis (for 128B objects). We'll measure and update.

**Q: Are the slab layout numbers accurate?**  
A: Yes. The canonical layout (64B header + 128B bitmap + 3904B data) is mathematically derived and implementation-independent.

---

## Commitment to Honesty

**We will not claim performance wins we cannot demonstrate.**

All future documentation updates will:
1. Clearly label targets vs. validated results
2. Provide reproducible benchmark code
3. Document exact test conditions
4. Acknowledge limitations and unfair comparisons

This is how serious systems projects build credibility.

---

*Last Updated: 2026-02-05 (Phase 1 - Targets Only)*
