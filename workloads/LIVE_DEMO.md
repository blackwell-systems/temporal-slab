# Live Dashboard Demo Guide

This guide shows how to run benchmarks while watching real-time metrics in Grafana.

## Setup (One Time)

### Terminal 1: Start Observability Stack
```bash
cd /home/blackwd/code/temporal-slab-tools
./run-observability.sh
```

Wait for all 3 services to show "Up":
- Prometheus (port 9090)
- Grafana (port 3000)
- Pushgateway (port 9091)

### Terminal 2: Start Metrics Push Loop
```bash
cd /home/blackwd/code/temporal-slab-tools
./push-metrics.sh
```

You should see:
```
[2026-02-07 16:30:00] Metrics pushed successfully
```

### Browser: Open Grafana Dashboard

1. Navigate to: **http://localhost:3000**
2. Login: `admin` / `admin`
3. Click **Dashboards → Browse**
4. Select **"temporal-slab: allocator observability"**
5. Set time range to **"Last 5 minutes"** (top-right)
6. Set refresh to **"5s"** (top-right dropdown)

**Keep this browser window visible** while running benchmarks.

---

## Live Demonstration Workflows

### Demo 1: Burst Pattern (RSS Sawtooth)

**What to watch:**
- **"RSS vs estimated slab RSS"** panel (row 1)
- **"madvise throughput"** panel (row 3)
- **"epoch_close() call rate"** panel (row 4, Phase 2.1)

**Terminal 3:**
```bash
cd /home/blackwd/code/ZNS-Slab/workloads

# malloc baseline (60 seconds)
./synthetic_bench --allocator=malloc --pattern=burst --duration_s=60
```

**Expected dashboard behavior (malloc):**
- RSS grows steadily (no reclamation)
- No madvise activity (malloc doesn't call madvise)
- No slow-path metrics (malloc has no telemetry)

**Wait 10 seconds for metrics to stabilize, then run:**
```bash
# tslab comparison (60 seconds)
./synthetic_bench --allocator=tslab --pattern=burst --duration_s=60
```

**Expected dashboard behavior (tslab):**
- **RSS sawtooth pattern** (allocate → free → epoch_close → RSS drops)
- **madvise spikes** at 2000 calls/s (correlates with epoch close)
- **epoch_close_calls: 2000/s** (one per request)
- **Reclamation yield: 80-90%** (most scanned slabs recycled)
- **Slow-path rate: <1%** (99%+ fast path hits)

**Comparison insight:** Same throughput, but tslab returns memory to kernel (visible RSS drops).

---

### Demo 2: Leak Pattern (Epoch Age Detection)

**What to watch:**
- **"Epoch age (ACTIVE epochs only)"** panel (row 5)
- **"Top epochs by RSS"** panel (row 5)
- **"Refcount (CLOSING epochs)"** panel (row 5)

**Terminal 3:**
```bash
# malloc leak test (60 seconds)
./synthetic_bench --allocator=malloc --pattern=leak --duration_s=60
```

**Expected dashboard behavior (malloc):**
- No epoch metrics (malloc has no epochs)
- RSS grows (leaked objects accumulate)
- No attribution (can't see WHICH allocations leaked)

**Wait 10 seconds, then:**
```bash
# tslab leak test (60 seconds)
./synthetic_bench --allocator=tslab --pattern=leak --duration_s=60
```

**Expected dashboard behavior (tslab):**
- **Epoch age panel: Red/yellow lines** (old epochs not draining)
- **"Top epochs by RSS": Stuck epochs at top** (memory attribution)
- **"Refcount (CLOSING epochs): Non-zero values"** (should drain to 0, but doesn't due to leak)
- Can identify EXACTLY which epoch is leaking

**Comparison insight:** malloc shows "memory is leaking", tslab shows "epoch 7 has 2.3MB stuck for 45 seconds".

---

### Demo 3: Steady Pattern (RSS Plateau)

**What to watch:**
- **"Net slabs (global)"** panel (row 1)
- **"RSS vs estimated slab RSS"** panel (row 1)
- **"Per-class table"** panel (row 7)

**Terminal 3:**
```bash
# malloc steady state (60 seconds)
./synthetic_bench --allocator=malloc --pattern=steady --duration_s=60
```

**Expected dashboard behavior (malloc):**
- RSS may grow and not stabilize (depends on libc)
- No visibility into whether memory is reused
- No per-class breakdown

**Wait 10 seconds, then:**
```bash
# tslab steady state (60 seconds)
./synthetic_bench --allocator=tslab --pattern=steady --duration_s=60
```

**Expected dashboard behavior (tslab):**
- **Net slabs plateaus** (stable working set)
- **RSS stable** (not growing unboundedly)
- **Per-class cache hit rate visible** (table shows cache_size vs capacity)
- **Slow-path rate: Low and stable**

**Comparison insight:** malloc "trust me, it's fine", tslab "look at the plateau graph".

---

### Demo 4: Hotspot Pattern (Per-Class Analysis)

**What to watch:**
- **"Top classes by slow-path rate"** panel (row 6)
- **"Top classes by madvise throughput"** panel (row 6)
- **"Per-class table"** panel (row 7)

**Terminal 3:**
```bash
# malloc hotspot (60 seconds)
./synthetic_bench --allocator=malloc --pattern=hotspot --duration_s=60
```

**Expected dashboard behavior (malloc):**
- No per-class metrics
- Can't see which sizes are hot
- Blind to optimization opportunities

**Wait 10 seconds, then:**
```bash
# tslab hotspot (60 seconds)
./synthetic_bench --allocator=tslab --pattern=hotspot --duration_s=60
```

**Expected dashboard behavior (tslab):**
- **"Top classes by slow-path": Class 2 (128B) dominates**
- **"Per-class table": Cache sizes visible** (can tune hot classes)
- **Clear signal:** 128B class needs bigger cache

**Comparison insight:** malloc is blind, tslab shows exactly what to tune.

---

### Demo 5: Kernel Pattern (Reclamation Proof)

**What to watch:**
- **"madvise throughput"** panel (row 3)
- **"RSS vs estimated slab RSS"** panel (row 1)
- **"madvise failures"** panel (row 3)

**Terminal 3:**
```bash
# malloc kernel test (60 seconds)
./synthetic_bench --allocator=malloc --pattern=kernel --duration_s=60
```

**Expected dashboard behavior (malloc):**
- No madvise activity (malloc doesn't use it)
- RSS behavior opaque
- Can't prove memory returns to kernel

**Wait 10 seconds, then:**
```bash
# tslab kernel test (60 seconds)
./synthetic_bench --allocator=tslab --pattern=kernel --duration_s=60
```

**Expected dashboard behavior (tslab):**
- **madvise throughput: High activity** (bytes/sec visible)
- **RSS drops correlate with madvise spikes** (causal proof)
- **madvise failures: 0** (system healthy)
- Clear causality: epoch_close → madvise → RSS drop

**Comparison insight:** malloc "maybe returns memory?", tslab "proof with timing".

---

## Quick Demo Script (All 5 Patterns)

Run this to see all patterns back-to-back:

```bash
cd /home/blackwd/code/ZNS-Slab/workloads

# burst: RSS sawtooth
./synthetic_bench --pattern=burst --duration_s=30
sleep 10

# steady: RSS plateau
./synthetic_bench --pattern=steady --duration_s=30
sleep 10

# leak: epoch age anomalies
./synthetic_bench --pattern=leak --duration_s=30
sleep 10

# hotspot: per-class breakdown
./synthetic_bench --pattern=hotspot --duration_s=30
sleep 10

# kernel: madvise proof
./synthetic_bench --pattern=kernel --duration_s=30
```

**Total time:** ~3 minutes (5x30s + pauses)

**Dashboard panels to watch:**
1. Row 1: RSS behavior
2. Row 2: Slow-path tail latency
3. Row 3: Kernel reclamation
4. Row 4: Phase 2.1 epoch_close telemetry
5. Row 5: Epoch leak detection
6. Row 6: Per-class hotspots
7. Row 7: Detailed per-class table

---

## Side-by-Side malloc vs tslab Demo

**For maximum impact:** Run both allocators on same pattern and compare.

### Setup
- Split screen: Grafana dashboard (left) + terminal (right)
- Or: Two monitors (dashboard on one, terminal on other)

### Script
```bash
cd /home/blackwd/code/ZNS-Slab/workloads

echo "=== malloc burst (watch dashboard) ==="
./synthetic_bench --allocator=malloc --pattern=burst --duration_s=60

# Let metrics stabilize
sleep 10

echo "=== tslab burst (watch dashboard change) ==="
./synthetic_bench --allocator=tslab --pattern=burst --duration_s=60
```

**What to narrate:**
1. "malloc: RSS grows, no visibility" (during first run)
2. "tslab: RSS drops, madvise spikes, epoch metrics" (during second run)
3. "Same throughput, but tslab shows WHY and WHERE"

---

## Automated Full Comparison

To run all 5 patterns with both allocators and capture metrics:

```bash
cd /home/blackwd/code/ZNS-Slab/workloads
./run_comparison.sh
```

**This will:**
- Run 10 benchmarks (5 patterns × 2 allocators)
- 60 seconds each = 10 minutes total
- Capture metrics snapshots after each run
- Save to timestamped results directory

**While it runs:** Watch the dashboard to see behavior changes between malloc and tslab runs.

---

## Key Panels for Each Pattern

### burst
- **Row 1:** RSS vs estimated slab RSS (sawtooth)
- **Row 3:** madvise throughput (spikes)
- **Row 4:** epoch_close call rate (2000/s)

### steady
- **Row 1:** Net slabs (plateau, not growing)
- **Row 7:** Per-class table (cache hit rate)

### leak
- **Row 5:** Epoch age (red/yellow old epochs)
- **Row 5:** Top epochs by RSS (leakers at top)
- **Row 5:** Refcount CLOSING (non-zero = leak)

### hotspot
- **Row 6:** Top classes by slow-path rate
- **Row 7:** Per-class table (which class is hot)

### kernel
- **Row 3:** madvise throughput vs RSS (correlation)
- **Row 3:** madvise failures (should be 0)

---

## Tips for Best Demo Experience

1. **Set refresh to 5s** (top-right in Grafana)
2. **Use "Last 5 minutes" time range** (top-right)
3. **Full-screen the dashboard** (better visibility)
4. **Pause between runs** (10s for metrics to stabilize)
5. **Point out specific panels** as they change

---

## Troubleshooting

**Dashboard shows "No data":**
- Check push-metrics.sh is running: `ps aux | grep push-metrics`
- Verify Pushgateway has metrics: `curl http://localhost:9091/metrics | head`

**Metrics look stale:**
- Refresh browser (Cmd/Ctrl+R)
- Check refresh interval is set (5s recommended)
- Verify time range is recent (Last 5 minutes)

**benchmark runs but no dashboard changes:**
- Stats_dump must be working: `cd ../ZNS-Slab/src && ./stats_dump --no-text`
- Push-metrics.sh must be active (should see pushes every 15s)

---

## Recording the Demo

If you want to capture the demo for sharing:

**Screen recording:**
```bash
# Linux
ffmpeg -video_size 1920x1080 -framerate 30 -f x11grab -i :0.0 \
  -c:v libx264 -preset fast demo.mp4

# Or use OBS Studio
```

**Screenshots:**
1. Run each pattern
2. Wait 30 seconds (let metrics accumulate)
3. Screenshot each key panel
4. Annotate with arrows/labels

**Comparison screenshots:**
- Before: malloc (no metrics visible)
- After: tslab (rich metrics visible)
- Side-by-side for impact

---

## Current Status

**Running now:**
- Observability stack: UP
- push-metrics.sh: RUNNING
- Dashboard accessible: http://localhost:3000

**Ready to demo:**
```bash
cd /home/blackwd/code/ZNS-Slab/workloads
./synthetic_bench --pattern=burst --duration_s=60
# (watch dashboard while this runs)
```
