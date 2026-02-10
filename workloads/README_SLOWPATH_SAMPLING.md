# Slowpath Sampling - WSL2/VM Tail Latency Diagnosis

## Purpose

Distinguishes **real allocator work** from **WSL2/VM scheduling noise** in deep tail measurements (p9999/p99999).

### The Problem

On WSL2, p99999 outliers can be caused by:
- Host-side preemption (Windows scheduler)
- VM clock jitter
- Cross-core contention amplified by virtualization

### The Solution

Compare **wall-clock time** vs **thread CPU time**:
- `wall >> cpu` → Thread was preempted (scheduling noise)
- `wall ≈ cpu` → Real allocator work (lock contention, mmap, etc.)

## Usage

### Build with Sampling Enabled

```bash
cd src/
make clean
make CFLAGS="-DENABLE_SLOWPATH_SAMPLING=1 -DSLOWPATH_THRESHOLD_NS=5000 -I../include"
```

### Rebuild Benchmarks

```bash
gcc -O3 -pthread -I../include -DENABLE_SLOWPATH_SAMPLING=1 -DSLOWPATH_THRESHOLD_NS=5000 \
    ../workloads/locality_bench.c slab_lib.o -o ../workloads/locality_bench_sampled
```

### Run and Analyze

```bash
./workloads/locality_bench_sampled
```

Output includes:
```
=== Slowpath Samples ===
Total samples: 127 (threshold: 5000ns)

Classification:
  Preempted (wall>3×cpu): 89 (70.1%) - WSL2/VM scheduling noise
  Real work (wall≈cpu):    38 (29.9%) - Actual allocator slowpath

Latency statistics:
  Wall-clock: avg=23us max=257us
  Thread CPU: avg=7us max=31us

Reason breakdown:
  Lock wait:      15
  New slab:       8
  Zombie repair:  5
  Cache overflow: 0
  CAS retry:      10
========================
```

## Interpretation

### If 70%+ samples are "Preempted"
→ p9999/p99999 is dominated by WSL2 noise, not allocator issues

### If 70%+ samples are "Real work"
→ p9999/p99999 is true allocator tail - investigate reason breakdown

### High "Lock wait" counts
→ Contention on `sc->lock` - consider reducing thread count or object size distribution

### High "Zombie repair" counts
→ Benign self-heal, but indicates frequent "published partial becomes full" race

## Testing on Native Linux

Compare WSL2 vs bare metal:
```bash
# WSL2 (with sampling)
./locality_bench_sampled

# Native Linux EC2/bare metal (same command)
./locality_bench_sampled
```

If "Preempted %" drops dramatically on native Linux, confirms WSL2 was the issue.

## Overhead

- **Per sample**: ~100ns (only triggered when allocation exceeds threshold)
- **Memory**: 320KB ring buffer (10K samples × 32 bytes)
- **Typical impact**: <0.1% on overall throughput (samples are rare)
