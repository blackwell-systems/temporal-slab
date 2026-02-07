# Canonical Benchmark Harness

The `synthetic_bench` tool generates parameterized workloads to demonstrate temporal-slab allocator behavior in Grafana dashboards.

## Quick Start

```bash
# Build
cd /home/blackwd/code/ZNS-Slab/src
make synthetic_bench

# Run burst pattern (default)
cd /home/blackwd/code/ZNS-Slab/workloads
./synthetic_bench --allocator=tslab --pattern=burst --duration_s=60

# Compare with malloc
./synthetic_bench --allocator=malloc --pattern=burst --duration_s=60
```

## Pattern Presets

### 1. burst (Epoch-Bound Request Bursts)

**Dashboard signature:** RSS sawtooth + madvise spikes

**Configuration:**
- 2000 req/s per thread
- 80-200 objects per request
- 128-byte objects
- Free within request
- Per-request epoch (close on complete)

**What it demonstrates:**
- Deterministic RSS drops after `epoch_close()`
- Clean temporal boundaries
- madvise throughput spikes correlate with epoch close events
- Net slabs stays near zero in steady state

**Use case:** HTTP request handling, RPC calls, query execution

```bash
./synthetic_bench --pattern=burst --duration_s=60
```

### 2. steady (Steady-State Churn)

**Dashboard signature:** RSS plateau with stable cache reuse

**Configuration:**
- 5000 req/s per thread
- 50 objects per request
- 128-byte objects
- Free with 8-request lag
- Batch epoch policy (16 requests per epoch)

**What it demonstrates:**
- RSS stability under sustained pressure
- High cache hit rate (new_slab_count ≈ empty_slab_recycled)
- Slow-path rate stabilizes
- Disproves "allocators always grow" myth

**Use case:** Session caches, connection tracking, message buffers

```bash
./synthetic_bench --pattern=steady --duration_s=60
```

### 3. leak (Pathological Lifetime Mismatch)

**Dashboard signature:** Epoch age/refcount anomalies

**Configuration:**
- 2000 req/s per thread
- 80-200 objects per request
- 128-byte objects
- 1% leak rate
- Per-request epoch

**What it demonstrates:**
- Epoch refcount stays non-zero for stuck epochs
- Epoch age panel turns red for leaking epochs
- "Top epochs by RSS" highlights offenders
- RSS doesn't drop until leak resolves
- Semantic observability beats traditional allocators

**Use case:** Partial request cancellation, error paths, aborted jobs

```bash
./synthetic_bench --pattern=leak --duration_s=60
```

### 4. hotspot (Mixed Size-Class Hotspots)

**Dashboard signature:** Per-class slow-path hotspots

**Configuration:**
- 4000 req/s per thread
- 120 objects per request
- 128-byte objects (MVP: single size, multi-size coming)
- Free within request
- Per-request epoch

**What it demonstrates:**
- Per-class slow-path rate differences
- Per-class madvise throughput variations
- Clear signal which size class needs tuning
- Validates per-class accounting design

**Use case:** Metadata-heavy systems, ASTs, tokens, graph nodes

```bash
./synthetic_bench --pattern=hotspot --duration_s=60
```

### 5. kernel (Kernel Interaction Demonstration)

**Dashboard signature:** Strong madvise→RSS correlation

**Configuration:**
- 2000 req/s per thread (target, actual may vary due to pressure)
- 300 objects per request
- 256-byte objects
- Free within request
- Per-request epoch
- Aggressive epoch_close

**What it demonstrates:**
- RSS drops correlate exactly with madvise_bytes
- madvise_failures visible under pressure (should be 0)
- Clear causal chain: epoch_close → empty slabs → madvise → RSS drop
- Kernel-aware memory management

**Use case:** Proving allocator cooperates with kernel reclamation

```bash
./synthetic_bench --pattern=kernel --duration_s=60
```

## CLI Reference

### Required Flags

```bash
--allocator=<tslab|malloc>     # Backend allocator
--pattern=<burst|steady|leak|hotspot|kernel>  # Workload pattern
```

### Optional Flags (override pattern defaults)

```bash
--duration_s=N                 # Run duration in seconds (default: 60)
--threads=N                    # Worker threads (default: 1)
--req_rate=N                   # Requests/sec per thread (default: pattern-specific)
--objs_min=N                   # Min objects per request (default: pattern-specific)
--objs_max=N                   # Max objects per request (default: pattern-specific)
--size=N                       # Object size in bytes (default: 128)
--epoch_policy=<per_req|batch:N|manual>  # Epoch management
--free_policy=<within_req|lag:N|leak:pct>  # Free timing
--rss_sample_ms=N              # RSS sampling interval (0=disabled, not yet implemented)
```

### Examples

```bash
# Burst with larger objects
./synthetic_bench --pattern=burst --size=512 --duration_s=30

# Steady with higher rate
./synthetic_bench --pattern=steady --req_rate=10000 --duration_s=30

# Leak with 5% leak rate
./synthetic_bench --pattern=leak --free_policy=leak:5 --duration_s=30

# Custom configuration (not using preset)
./synthetic_bench --allocator=tslab \
  --req_rate=3000 \
  --objs_min=50 --objs_max=150 \
  --size=192 \
  --epoch_policy=batch:32 \
  --free_policy=lag:16 \
  --duration_s=60
```

## Integration with Observability Stack

The benchmark generates allocator activity that flows to Grafana dashboards via the observability stack.

### Setup

```bash
# Terminal 1: Start observability stack
cd /home/blackwd/code/temporal-slab-tools
./run-observability.sh

# Terminal 2: Start metrics push loop
./push-metrics.sh

# Terminal 3: Run benchmark
cd /home/blackwd/code/ZNS-Slab/workloads
./synthetic_bench --pattern=burst --duration_s=300

# Browser: Open Grafana
# http://localhost:3000 (admin/admin)
# Dashboard: "temporal-slab: allocator observability"
```

### What to Watch

**Burst pattern:**
- RSS vs estimated slab RSS panel: Sawtooth pattern
- madvise throughput: Spikes correlating with epoch closes
- Epoch-close telemetry: High call rate, good reclamation yield

**Steady pattern:**
- RSS vs estimated slab RSS: Plateau
- Net slabs: Stable value
- Per-class table: High cache hit rate

**Leak pattern:**
- Epoch age panel: Red (old) epochs
- Top epochs by RSS: Stuck epochs at top
- Refcount (CLOSING epochs): Non-zero values

**Hotspot pattern:**
- Top classes by slow-path rate: Clear winner
- Per-class madvise throughput: Variation visible

**Kernel pattern:**
- madvise throughput vs RSS: Strong correlation
- madvise failures: Should be 0

## Architecture

**Single-file design** (690 lines):
- Backend abstraction (tslab vs malloc)
- Request simulation with configurable timing
- Lag buffer for delayed frees
- Epoch policy management
- Pattern presets as configuration templates

**Lifecycle control:**
- Epoch policy: per-request, batch, manual
- Free policy: within-request, lag, leak
- Object allocation: configurable size and count

**Key design principle:** Implement first, refactor later. The tool works and produces dashboard metrics immediately without premature abstraction.

## Extending

### Adding a New Pattern

1. Add enum value to `WorkloadPattern`
2. Add case to `apply_pattern_preset()`
3. Set configuration parameters
4. Document expected dashboard signature

### Adding Multi-Size Support

Current MVP uses single object size. To add multi-size:

1. Add size distribution config (e.g., `--sizes=128:85,256:10,512:5`)
2. Update `simulate_request()` to sample from distribution
3. Test with `hotspot` pattern preset

## Performance Notes

**Request rate achieved vs target:**
- burst: ~2000 req/s (100% of target)
- steady: ~5000 req/s (100% of target)
- leak: ~2000 req/s (100% of target)
- hotspot: ~4000 req/s (100% of target)
- kernel: ~1400 req/s (70% of target due to larger objects + pressure)

Lower actual rates for kernel pattern are expected due to:
- Larger objects (256B) = more memory pressure
- Higher object count per request (300 vs 80-200)
- Aggressive epoch_close creates momentary pressure

This is correct behavior demonstrating allocator limits under stress.

## Troubleshooting

**Benchmark stops early:**
- Check for allocation failures in stderr
- Reduce `--req_rate` or `--objs_max`

**No metrics in Grafana:**
- Ensure `push-metrics.sh` is running
- Check Pushgateway: `curl http://localhost:9091/metrics | grep temporal_slab`
- Verify stats_dump is accessible from benchmark process

**Leak pattern shows 0% freed:**
- This was a bug in v1, fixed in commit a7ac3e8
- Rebuild: `cd src && make synthetic_bench`

## Related Documentation

- `/home/blackwd/code/ZNS-Slab/docs/stats_dump_reference.md` - Metrics definitions
- `/home/blackwd/code/temporal-slab-tools/README.md` - Observability stack setup
- `/home/blackwd/code/temporal-slab-tools/dashboards/` - Grafana dashboard JSON

## Future Work

- Multi-size distribution support (for hotspot pattern)
- Multi-threaded workloads (currently single-threaded MVP)
- Poisson/bursty request timing (currently fixed-rate)
- Internal RSS sampling output (currently relies on external stats_dump)
- jemalloc head-to-head comparison script
- Automated pattern verification (assert dashboard signatures match expected)
