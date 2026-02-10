# Slowpath Sampling - WSL2/VM Tail Latency Diagnosis

## Purpose

Distinguishes **real allocator work** from **WSL2/VM scheduling noise** via probabilistic end-to-end timing with wall vs CPU time measurement.

### The Problem

On WSL2, tail latency measurements can be misleading:
- Host-side preemption (Windows scheduler)
- VM clock jitter
- Cross-core contention amplified by virtualization

**Traditional threshold-based sampling doesn't validate the measurement itself** - "0 samples at 1µs" just means your workload is fast, not that the sampler works.

### The Solution (Phase 2.5)

**Probabilistic sampling (1/1024)** with **wall-clock vs thread CPU time split**:
- Samples 1 out of every 1024 allocations (regardless of latency)
- Validates measurement infrastructure works
- `wall >> cpu` → Scheduler preemption (WSL2/virtualization noise)
- `wall ≈ cpu` → Real allocator work (locks, CAS storms, repairs)

**Explicit zombie repair timing** with reason code attribution:
- Tracks repair frequency (invariant violation signal)
- Measures repair contribution to tail latency
- Attributes to: full_bitmap, list_mismatch, other

## Architecture

### Per-Thread TLS Counters (Zero Contention)

```c
typedef struct {
  /* Allocation samples (1/1024 probabilistic) */
  uint64_t alloc_samples;
  uint64_t alloc_wall_ns_sum;
  uint64_t alloc_cpu_ns_sum;
  uint64_t alloc_wall_ns_max;
  uint64_t alloc_cpu_ns_max;
  
  /* Zombie repair tracking */
  uint64_t repair_count;
  uint64_t repair_wall_ns_sum;
  uint64_t repair_cpu_ns_sum;
  uint64_t repair_wall_ns_max;
  uint64_t repair_cpu_ns_max;
  
  /* Repair reason attribution */
  uint64_t repair_reason_full_bitmap;    /* fc==0 && bitmap full */
  uint64_t repair_reason_list_mismatch;  /* list_id != expected */
  uint64_t repair_reason_other;
} ThreadStats;
```

### Sampling Points

1. **End-to-end allocation timing** - Entire `alloc_obj_epoch()` call
2. **Explicit repair timing** - Zombie partial fix (lines 1653-1689 in slab_alloc.c)
3. **All return paths instrumented** - Fast path, slow path, error exits

## Usage

### Build with Sampling Enabled

```bash
# Build all targets with sampling
make ENABLE_SLOWPATH_SAMPLING=1

# Or build specific target
gcc -DENABLE_SLOWPATH_SAMPLING -O2 -pthread -I../include \
    your_test.c slab_alloc.c slab_stats.c epoch_domain.c -o your_test
```

### Add Sampling Output to Your Test

```c
#include "slab_stats.h"

int main() {
    SlabAllocator* alloc = slab_allocator_create();
    
    // ... run your workload ...
    
#ifdef ENABLE_SLOWPATH_SAMPLING
    ThreadStats stats = slab_stats_thread();
    
    if (stats.alloc_samples > 0) {
        uint64_t avg_wall = stats.alloc_wall_ns_sum / stats.alloc_samples;
        uint64_t avg_cpu = stats.alloc_cpu_ns_sum / stats.alloc_samples;
        
        printf("\n=== Slowpath Sampling (1/1024) ===\n");
        printf("Samples: %lu\n", stats.alloc_samples);
        printf("Avg wall: %lu ns (max: %lu ns)\n", avg_wall, stats.alloc_wall_ns_max);
        printf("Avg CPU:  %lu ns (max: %lu ns)\n", avg_cpu, stats.alloc_cpu_ns_max);
        printf("Ratio: %.2fx\n", (double)avg_wall / avg_cpu);
        
        if (avg_wall > avg_cpu * 2) {
            printf("⚠ wall >> cpu: Scheduler interference\n");
        }
    }
    
    if (stats.repair_count > 0) {
        printf("\n=== Zombie Repairs ===\n");
        printf("Count: %lu\n", stats.repair_count);
        printf("Reasons:\n");
        printf("  Full bitmap:   %lu\n", stats.repair_reason_full_bitmap);
        printf("  List mismatch: %lu\n", stats.repair_reason_list_mismatch);
        printf("  Other:         %lu\n", stats.repair_reason_other);
    }
#endif
    
    slab_allocator_free(alloc);
    return 0;
}
```

### Example Output

```
=== Slowpath Sampling (1/1024) ===
Samples: 97
Avg wall: 4382 ns (max: 251618 ns)
Avg CPU:  1536 ns (max: 9293 ns)
Ratio: 2.85x
⚠ wall >> cpu: Scheduler interference

✓ No zombie repairs
```

## Interpretation

### Wall vs CPU Ratio

| Ratio | Meaning | Action |
|-------|---------|--------|
| < 1.5× | Clean measurement | Trust latency measurements |
| 1.5-2× | Moderate noise | Some WSL2/VM interference |
| > 2× | Heavy interference | Run on bare metal for accurate measurements |

### Max Latency Spikes

- **Max wall >> max cpu** (e.g., 251µs vs 9µs)
  - Indicates extreme preemption event
  - Not representative of allocator performance
  - Caused by host scheduler

- **Max wall ≈ max cpu**
  - Real allocator spike (lock contention, new slab allocation)
  - Investigate with lock contention metrics

### Zombie Repairs

**High repair count** (even if not contributing to tail latency) indicates:
1. Publication race (current_partial vs list state)
2. Stale view of slab fullness
3. Missing memory barrier around partial↔full transitions

**This is a correctness signal**, not just a performance issue.

## Testing: WSL2 vs Native Linux

Compare results:

```bash
# WSL2
./your_test
# Expected: wall/cpu ratio 2-3×, high max wall spikes

# Native Linux (EC2/bare metal)
./your_test  
# Expected: wall/cpu ratio ~1.1×, lower max spikes
```

If ratio drops dramatically on native Linux, confirms WSL2 was the bottleneck.

## Overhead

- **Zero overhead when disabled** - All code gated by `#ifdef ENABLE_SLOWPATH_SAMPLING`
- **Minimal overhead when enabled**:
  - 1/1024 operations sampled
  - ~200ns per sample (two `clock_gettime` calls)
  - < 0.02% overall impact (200ns × 1/1024 ≈ 0.2ns per alloc)
- **No atomic operations** - All TLS counters (per-thread)
- **No heap allocation** - Fixed-size TLS structure

## Differences from Old Threshold-Based Approach

| Old (Threshold) | New (Probabilistic) |
|-----------------|---------------------|
| Only samples slow operations | Samples 1/1024 regardless |
| Can't validate sampler works | Proves measurement works |
| Threshold must be tuned | No configuration needed |
| Misses fast-but-preempted | Catches all patterns |

## API Reference

```c
// Get current thread's sampling statistics
ThreadStats slab_stats_thread(void);

// Sampling rate (defined in slab_stats.h)
#define SAMPLE_RATE_MASK 1023u  // 1/1024

// Repair reason flags
#define REPAIR_REASON_FULL_BITMAP    (1u << 0)
#define REPAIR_REASON_LIST_MISMATCH  (1u << 1)
```

## Build Integration

Add to your Makefile:

```makefile
# Optional: Enable slowpath sampling
ifdef ENABLE_SLOWPATH_SAMPLING
  CFLAGS += -DENABLE_SLOWPATH_SAMPLING
endif

# Link slab_stats.o for sampling support
your_test: your_test.c slab_alloc.c slab_stats.c epoch_domain.c
	$(CC) $(CFLAGS) -o $@ $^ -pthread
```

## Related Documentation

- **include/slab_stats.h** - ThreadStats structure definition
- **src/slab_alloc.c:1330-1782** - Instrumented alloc_obj_epoch()
- **src/slab_alloc.c:1653-1689** - Instrumented zombie repair
- **docs/CONTENTION_RESULTS.md** - Multi-threaded contention analysis
