# Production Hardening Checklist

This document provides a checklist for deploying temporal-slab in production environments, including compile-time flags, performance tuning, and operational recommendations.

---

## Compile-Time Configuration

### 1. Disable Diagnostic Counters (Performance)

**Default:** Disabled (`ENABLE_DIAGNOSTIC_COUNTERS=0`)  
**Performance impact:** ~1-2% latency overhead when enabled

The diagnostic counters (`live_bytes`, `committed_bytes`, `empty_slabs`) provide mathematical proof of bounded RSS but add atomic operations to every allocation/free on the hot path.

**Production build (recommended):**
```bash
make  # Diagnostic counters disabled by default
```

**Development/debugging build:**
```bash
make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1"
```

**When to enable:**
- Actively debugging RSS behavior
- Proving bounded memory for validation/certification
- Investigating suspected memory leaks
- Running sustained stress tests (e.g., 2000-cycle phase shifts)

**When to disable:**
- Production deployments prioritizing latency
- Benchmarking p50/p99/p999 metrics
- After RSS behavior has been validated

**Verification:**
```c
// Check if counters are available at runtime
SlabClassStats stats;
slab_stats_class(alloc, 0, &stats);
if (stats.committed_bytes == 0 && stats.live_bytes == 0) {
  // Counters disabled at compile time
}
```

---

## Performance Tuning

### 2. Size Class Selection

**Fixed size classes:** 64, 96, 128, 192, 256, 384, 512, 768 bytes

**Recommendation:** Profile your allocation sizes and ensure they align with these classes to minimize internal fragmentation.

**Check fragmentation:**
```c
SlabClassStats stats;
slab_stats_class(alloc, class_index, &stats);
double fragmentation = (double)stats.committed_bytes / stats.live_bytes;
// Target: <2.0× for well-aligned workloads
```

### 3. Epoch Management

**Ring buffer size:** 16 epochs (hardcoded in `EPOCH_COUNT`)

**Recommendation:** Rotate epochs frequently enough that old epochs drain before wraparound.

**Guidelines:**
- Web services (100k req/s): Advance every 10-1000 requests
- Game engines (60 FPS): Advance every frame or every N frames
- Batch processors: Advance per batch

**Anti-pattern:** Never advancing epochs causes all allocations to land in epoch 0, defeating temporal grouping.

### 4. Cache Tuning

**Default cache:** 32 slabs per size class (128 KB total per class)

**Consideration:** Current implementation hardcodes cache size. Future work may expose this as a tunable parameter.

---

## Operational Best Practices

### 5. Monitoring & Observability

**Key metrics to track:**

| Metric | Target | Action if violated |
|--------|--------|-------------------|
| `slow_path_hits / total_allocs` | <5% | Increase cache size |
| `empty_slab_overflowed` | <1% of recycled | Cache is saturated |
| `madvise_bytes` | Matches expected reclamation | Verify epoch_close() calls |
| `avg_alloc_cas_retries_per_attempt` | <0.01 | Lock-free path healthy |
| `lock_contended / lock_acquisitions` | <15% | Normal contention |

**Tools:**
- `stats_dump` - JSON snapshot for automation
- `tslab watch` - Live monitoring (temporal-slab-tools repo)
- `tslabd` - Prometheus exporter for production

### 6. Epoch Close Discipline

**Critical:** Call `epoch_close()` at phase boundaries to enable RSS reclamation.

**Patterns:**
```c
// Web service: per-request
EpochDomain* domain = epoch_domain_create();
// ... allocations ...
epoch_domain_destroy(domain);  // Calls epoch_close() internally

// Manual management: per-phase
EpochId old_epoch = epoch_current(alloc);
epoch_advance(alloc);
// ... new allocations use new epoch ...
epoch_close(alloc, old_epoch);  // Reclaim old epoch when drained
```

**Without epoch_close():** Allocator maintains bounded RSS within each epoch, but never reclaims across epoch boundaries. RSS will stabilize at `(# active epochs) × (per-epoch footprint)`.

### 7. Thread Safety Considerations

**Lock-free fast path:** Allocation from `current_partial` slab uses atomic CAS (no locks).

**Mutex-protected slow path:**
- Slab selection from PARTIAL list
- List management (partial ↔ full)
- Cache operations

**Recommendation:** Monitor `lock_contended` counter. Values >20% indicate high contention on size class locks.

**Mitigation for high contention:**
- Reduce thread count per size class
- Use per-thread allocators with periodic consolidation
- Consider jemalloc for highly contended general-purpose workloads

### 8. Handle vs Malloc-Style API

**Handle API (zero overhead):**
```c
SlabHandle h;
void* p = alloc_obj_epoch(alloc, 128, epoch, &h);
free_obj(alloc, h);
```

**Malloc-style API (8-byte overhead):**
```c
void* p = slab_malloc_epoch(alloc, 128, epoch);
slab_free(alloc, p);
```

**Production recommendation:**
- Use handle API for maximum performance (no header overhead)
- Use malloc-style API only for drop-in compatibility or when handles are inconvenient

---

## Validation & Testing

### 9. Stress Testing

**Recommended tests before production:**

```bash
cd src/
make

# Correctness
./smoke_tests
./test_epochs
./test_malloc_wrapper

# RSS bounds (with diagnostic counters)
make clean
make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1"
./churn_test  # Validates 0% RSS growth

# Latency benchmarking (production build, counters disabled)
make clean
make
./benchmark_accurate  # Measure p50/p99/p999
```

**Sustained load test:**
```bash
# From temporal-slab-allocator-bench repo
./workloads/sustained_phase_shifts 2000 10 20 256 64
# Validates bounded RSS across 2000 cycles with 256 MB memory pressure
```

### 10. Pre-Deployment Checklist

- [ ] Compile with diagnostic counters **disabled** (`ENABLE_DIAGNOSTIC_COUNTERS=0`)
- [ ] Profile allocation sizes to ensure alignment with fixed size classes
- [ ] Verify `epoch_close()` is called at appropriate phase boundaries
- [ ] Stress test with realistic workload for 2+ hours
- [ ] Monitor RSS stability (expect <5% variation after warmup)
- [ ] Benchmark p99/p999 latency (target: <200ns p99, <500ns p999)
- [ ] Verify slow-path hit rate <5% under normal load
- [ ] Set up monitoring for key metrics (`slow_path_hits`, `madvise_bytes`, contention)
- [ ] Document expected RSS footprint and epoch rotation policy

---

## Common Issues & Troubleshooting

### Issue: High p99 Latency (>500ns)

**Possible causes:**
1. Diagnostic counters enabled at compile time (+1-2% overhead)
2. High cache miss rate (check `slow_path_cache_miss`)
3. Lock contention (check `lock_contended`)

**Resolution:**
1. Rebuild with `ENABLE_DIAGNOSTIC_COUNTERS=0`
2. Increase cache size (requires code change)
3. Reduce thread count or use per-thread allocators

### Issue: RSS Growing Unbounded

**Possible causes:**
1. `epoch_close()` never called → memory held in old epochs
2. True leak (live_bytes increasing)

**Resolution:**
1. Enable diagnostic counters: `make CFLAGS="-DENABLE_DIAGNOSTIC_COUNTERS=1"`
2. Run workload and check:
   ```c
   SlabClassStats stats;
   slab_stats_class(alloc, class_index, &stats);
   printf("Committed: %lu, Live: %lu\n", stats.committed_bytes, stats.live_bytes);
   ```
3. If `committed_bytes` grows but `live_bytes` stays flat → missing `epoch_close()` calls
4. If `live_bytes` grows → true leak, audit allocation/free pairs

### Issue: High Slow-Path Rate (>10%)

**Possible causes:**
1. Cache too small for workload
2. Excessive epoch churn (advancing too frequently)

**Resolution:**
1. Monitor `slow_path_cache_miss` vs `slow_path_epoch_closed`
2. If cache misses dominate → increase cache size
3. If epoch closed dominates → reduce epoch advance frequency

---

## Future Hardening Work

**Phase 3 (Handle Indirection + munmap):**
- Add `handle → slab_table → slab` indirection layer
- Enable safe `munmap()` with generation-checked crash-proof frees
- Requires architecture changes (deferred until current design proven)

**Phase 4 (NUMA Awareness):**
- Per-NUMA-node size class allocators
- Reduces cross-node traffic in multi-socket systems

**Phase 5 (Tunable Cache Size):**
- Expose `SLAB_CACHE_SIZE` as runtime or compile-time parameter
- Enable per-workload cache tuning

---

## Summary

**Production-ready configuration:**
```bash
# Build with optimal settings
make CFLAGS="-O3 -march=native"  # Diagnostic counters disabled by default

# Verify counters are disabled
./smoke_tests  # Should show committed_bytes=0 in stats output

# Run stress test
./churn_test  # Validates RSS bounds
```

**Key takeaways:**
1. Disable diagnostic counters in production (default behavior)
2. Enable counters only for active RSS debugging
3. Call `epoch_close()` at phase boundaries for RSS reclamation
4. Monitor slow-path hit rate (<5% target)
5. Profile allocation sizes to ensure size class alignment

**Contact:**
- GitHub Issues: Report production issues or tuning questions
- Documentation: See `docs/` for architecture and design rationale
