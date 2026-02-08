# stats_dump JSON Reference

**Version:** 1.0  
**Status:** Stable contract for external tooling

---

## Overview

The `stats_dump` tool provides a machine-readable JSON snapshot of the temporal-slab allocator's internal state. This output is designed for:

1. **Performance monitoring** - Track allocation behavior, slow paths, and memory usage over time
2. **Debugging** - Identify stuck epochs, cache effectiveness issues, and reclamation problems
3. **Alerting** - Detect anomalies in allocation patterns or RSS growth
4. **Capacity planning** - Understand workload characteristics and size class distribution

The JSON format is versioned and stable. External tools can safely parse this output without linking to the allocator library.

---

## Output Modes

```bash
./stats_dump               # Both JSON (stdout) and text (stderr)
./stats_dump --json        # JSON only to stdout
./stats_dump --no-text     # JSON only (suppress text)
./stats_dump --doctor      # Actionable diagnostics (Phase 3)
```

**Recommended for tooling:** Use `--no-text` to get clean JSON without human-readable diagnostics.

---

## Top-Level Schema

```json
{
  "schema_version": 1,
  "timestamp_ns": 1770486521361625486,
  "pid": 74916,
  "page_size": 4096,
  "epoch_count": 16,
  "version": 1,
  "current_epoch": 10,
  "active_epoch_count": 6,
  "closing_epoch_count": 10,
  "total_slabs_allocated": 5,
  "total_slabs_recycled": 50,
  "net_slabs": 0,
  "rss_bytes_current": 1441792,
  "estimated_slab_rss_bytes": 0,
  "total_slow_path_hits": 50,
  "total_cache_overflows": 0,
  "total_slow_cache_miss": 5,
  "total_slow_epoch_closed": 0,
  "total_madvise_calls": 50,
  "total_madvise_bytes": 204800,
  "total_madvise_failures": 0,
  "classes": [ /* array of 8 class objects */ ]
}
```

---

## Field Definitions

### Snapshot Metadata

#### `schema_version` (uint32)

**Type:** Version number  
**Semantics:** Increments when top-level JSON structure changes (fields added/removed/renamed)

**Usage:**
- External tools should check this field first
- Current version: `1`
- Future versions will maintain backward compatibility where possible
- Breaking changes will be documented in CHANGELOG.md

---

#### `timestamp_ns` (uint64)

**Type:** Nanoseconds since UNIX epoch (CLOCK_REALTIME)  
**Semantics:** Exact time when snapshot was taken

**Usage:**
- **Diffing snapshots:** Compute time delta between two snapshots to calculate rates
- **Correlation:** Match allocator behavior with external events (request spikes, deployments)
- **Determinism:** Multiple snapshots from same process should have monotonically increasing timestamps

**Example calculation (rate):**
```python
delta_slow_paths = snapshot2.total_slow_path_hits - snapshot1.total_slow_path_hits
delta_time_sec = (snapshot2.timestamp_ns - snapshot1.timestamp_ns) / 1e9
slow_path_rate = delta_slow_paths / delta_time_sec
```

---

#### `pid` (int32)

**Type:** Process ID (Linux PID)  
**Semantics:** The process ID of the allocator instance

**Usage:**
- **Multi-process monitoring:** Distinguish snapshots from different processes
- **RSS validation:** Cross-reference with `/proc/{pid}/statm` for external RSS verification
- **Process lifecycle:** Detect restarts (PID changes)

---

#### `page_size` (int64)

**Type:** System page size in bytes (from `sysconf(_SC_PAGESIZE)`)  
**Semantics:** The page size used for mmap/madvise operations

**Usage:**
- **RSS interpretation:** RSS values are rounded to page boundaries
- **Slab sizing:** Each slab is exactly one page (4KB on most systems)
- **Platform compatibility:** Helps interpret metrics across different architectures

**Typical values:**
- x86_64 Linux: 4096 bytes
- ARM64 Linux: 4096 or 65536 bytes

---

#### `epoch_count` (uint32)

**Type:** Configuration constant  
**Semantics:** Total number of epoch slots (currently hardcoded to 16)

**Usage:**
- **Epoch rotation understanding:** Epochs are numbered 0-15 and wrap around
- **Capacity planning:** Maximum number of concurrent epoch domains
- **Warning threshold:** If `active_epoch_count` approaches `epoch_count`, increase epoch count

---

### Global State

#### `version` (uint32)

**Type:** Stats struct version  
**Semantics:** Version of the `SlabGlobalStats` struct (independent of `schema_version`)

**Usage:**
- **Struct evolution:** Tracks internal struct changes even if JSON serialization stays compatible
- **Debugging:** Helps identify which allocator version produced the snapshot
- Current version: `1`

---

#### `current_epoch` (uint32)

**Type:** Epoch ID (0-15)  
**Semantics:** The currently active epoch for new allocations

**Usage:**
- **Allocation tracking:** New `slab_malloc_epoch()` calls without explicit epoch use this
- **Epoch rotation monitoring:** Value increments on `epoch_advance()` (wraps at 16)
- **Stall detection:** If this value doesn't change over multiple snapshots, no new epochs are being created

**Expected behavior:**
- Increments regularly in active workloads
- Wraps: 0 → 1 → ... → 15 → 0

---

#### `active_epoch_count` (uint32)

**Type:** Count (0-16)  
**Semantics:** Number of epochs in ACTIVE state (accepting allocations)

**Usage:**
- **Concurrency indicator:** Shows how many request contexts are currently active
- **Resource usage:** More active epochs = more memory fragmentation potential
- **Warning threshold:** If this approaches `epoch_count` (16), you may run out of epochs

**Typical values:**
- Low-concurrency: 1-4
- High-concurrency: 8-12
- **Alert if:** > 14 (near capacity)

---

#### `closing_epoch_count` (uint32)

**Type:** Count (0-16)  
**Semantics:** Number of epochs in CLOSING state (draining allocations)

**Usage:**
- **Drainage effectiveness:** High count suggests objects aren't being freed promptly
- **Memory leak detection:** Persistently high values indicate stuck epochs (potential leak)
- **Reclamation backlog:** CLOSING epochs hold memory until fully drained

**Expected behavior:**
- Transient spikes (1-5) are normal during request processing
- Sustained high values (>10) indicate a problem

**Alert conditions:**
```
if closing_epoch_count > 10:
    # Possible leak: epochs not draining
    investigate_epoch_leak_detection()
```

---

### Slab Allocation Tracking

#### `total_slabs_allocated` (uint64)

**Type:** Monotonic counter  
**Semantics:** Cumulative count of slabs allocated from OS via `mmap()` since process start

**Usage:**
- **Memory growth rate:** Diff between snapshots shows allocation velocity
- **Capacity planning:** High rate suggests working set doesn't fit in cache
- **Steady state:** Should stabilize after warmup in stable workloads

**Interpretation:**
```
rate = delta(total_slabs_allocated) / delta(time)
steady_state = (rate ≈ 0)  # No new slabs needed
growth_phase = (rate > 0)  # Expanding working set
```

---

#### `total_slabs_recycled` (uint64)

**Type:** Monotonic counter  
**Semantics:** Cumulative count of empty slabs returned to per-class cache (not unmapped)

**Usage:**
- **Cache effectiveness:** Compare with `total_slabs_allocated` to measure reuse
- **Reclamation success:** Shows how often slabs become fully empty and reusable
- **Churn detection:** High recycle rate with high allocation rate = high churn

**Healthy ratio:**
```
recycle_ratio = total_slabs_recycled / total_slabs_allocated
# Good: recycle_ratio > 0.9 (90%+ reuse)
# Warning: recycle_ratio < 0.5 (most slabs not reused)
```

---

#### `net_slabs` (uint64)

**Type:** Derived gauge (allocated - recycled)  
**Semantics:** Current count of slabs in active use (not fully empty)

**Usage:**
- **Memory footprint:** Each slab = 4KB, so `net_slabs * 4096` ≈ allocator overhead
- **Leak detection:** If this grows without bound → memory leak
- **Steady state:** Should plateau in stable workloads

**Expected behavior:**
```
steady_state: net_slabs oscillates around constant
leak:         net_slabs monotonically increases
regression:   net_slabs suddenly jumps after code change
```

---

### RSS Measurements

#### `rss_bytes_current` (uint64)

**Type:** Gauge (sampled from `/proc/self/statm`)  
**Semantics:** **Process-wide** resident set size in bytes (includes allocator + heap + stacks)

**Usage:**
- **Memory consumption:** Total physical memory used by process
- **Reclamation verification:** Should decrease after `madvise()` calls on idle slabs
- **Alerting:** Sharp increases may indicate external memory leaks (not allocator)

**Important caveats:**
- **Not allocator-specific:** Includes libc heap, thread stacks, shared libraries
- **Noisy baseline:** Background allocations affect this value
- Use `estimated_slab_rss_bytes` for allocator-specific measurement

**Example alerting:**
```python
if rss_bytes_current > 2 * estimated_slab_rss_bytes:
    # Process using 2x more memory than allocator alone
    # Investigate non-allocator memory usage
```

---

#### `estimated_slab_rss_bytes` (uint64)

**Type:** Calculated gauge  
**Semantics:** Estimated RSS contribution from allocator's slabs only (sum across all classes)

**Usage:**
- **Allocator-specific memory:** Isolates allocator's footprint from process RSS
- **Per-class analysis:** Sum of all `classes[].estimated_rss_bytes`
- **Leak detection:** Should track with `net_slabs * 4096` (page-aligned)

**Calculation:**
```
estimated_slab_rss_bytes = sum(classes[i].estimated_rss_bytes)
```

**Why it differs from `rss_bytes_current`:**
- Doesn't include libc heap allocations
- Doesn't include thread stacks
- Doesn't include shared library mappings
- More accurate for allocator-specific memory tracking

---

### Slow Path Counters

#### `total_slow_path_hits` (uint64)

**Type:** Monotonic counter  
**Semantics:** Total number of allocation requests that took the slow path (cache miss or contention)

**Usage:**
- **Performance indicator:** Higher values = more cache misses or contention
- **Baseline metric:** Calculate slow path rate = `delta(hits) / delta(time)`
- **Optimization target:** Reducing this improves allocation latency

**Expected values:**
- **Good:** < 1% of total allocations
- **Warning:** 5-10% slow path rate
- **Critical:** > 20% (cache undersized or high contention)

**Root cause breakdown:**
```
slow_path_rate = total_slow_path_hits / total_allocations
cache_miss_pct = total_slow_cache_miss / total_slow_path_hits
epoch_closed_pct = total_slow_epoch_closed / total_slow_path_hits
```

---

#### `total_cache_overflows` (uint64)

**Type:** Monotonic counter  
**Semantics:** Count of empty slabs that couldn't fit in per-class cache (overflow list)

**Usage:**
- **Cache sizing:** High values suggest `cache_capacity` (32) is too small
- **Memory efficiency:** Overflow slabs can't be quickly reused (not in fast cache)
- **Tuning knob:** If high, consider increasing per-class cache size

**Healthy behavior:**
```
overflow_rate = total_cache_overflows / total_slabs_recycled
# Good: overflow_rate < 0.01 (1% overflow)
# Warning: overflow_rate > 0.1 (10%+ overflows)
```

---

#### `total_slow_cache_miss` (uint64)

**Type:** Monotonic counter  
**Semantics:** Slow paths caused by empty per-class cache (required new slab from OS)

**Usage:**
- **Cache effectiveness:** Shows how often cache couldn't satisfy allocation
- **Allocation cost:** Each miss triggers `mmap()` system call (~µs overhead)
- **Working set size:** High rate suggests working set exceeds cache capacity

**Overlaps with other counters:**
- A cache miss **implies** `current_partial_null` (no cached slab available)
- Part of `total_slow_path_hits` breakdown

---

#### `total_slow_epoch_closed` (uint64)

**Type:** Monotonic counter  
**Semantics:** Allocations that failed because target epoch was CLOSING (rejects new allocations)

**Usage:**
- **Epoch rotation correctness:** Should be **zero** in correct usage
- **Bug detection:** Non-zero values indicate application is allocating into closed epochs
- **Actionable error:** Applications should use `epoch_current()` or check state before allocating

**Alert condition:**
```
if total_slow_epoch_closed > 0:
    # BUG: Application allocating into CLOSING epochs
    # Fix: Call epoch_advance() before continuing allocations
```

---

### Reclamation Metrics

#### `total_madvise_calls` (uint64)

**Type:** Monotonic counter  
**Semantics:** Total `madvise(MADV_DONTNEED)` system calls made to return memory to OS

**Usage:**
- **Reclamation activity:** Shows how often allocator returns memory to kernel
- **RSS reduction:** Each successful call should decrease `rss_bytes_current`
- **Overhead tracking:** System calls have cost (~µs per call)

**Expected behavior:**
```
madvise_rate = delta(total_madvise_calls) / delta(time)
# Correlates with epoch close rate in steady state
```

---

#### `total_madvise_bytes` (uint64)

**Type:** Monotonic counter  
**Semantics:** Total bytes passed to `madvise()` (sum of all call sizes)

**Usage:**
- **Memory returned:** Shows how much memory returned to OS (before re-fault)
- **Efficiency metric:** Compare with `estimated_slab_rss_bytes` to see reclamation effectiveness
- **RSS verification:** `rss_bytes_current` should decrease by approximately this amount

**Calculation:**
```
avg_madvise_size = total_madvise_bytes / total_madvise_calls
# Expected: multiples of 4096 (page size)
```

---

#### `total_madvise_failures` (uint64)

**Type:** Monotonic counter  
**Semantics:** Count of `madvise()` system calls that returned error

**Usage:**
- **System health:** Non-zero values indicate kernel rejecting reclamation
- **Debugging:** Check `errno` values (permissions, invalid address, unsupported flags)
- **Alert condition:** Should always be zero in production

**Common causes:**
- Incorrect address alignment
- Invalid memory range
- Kernel not supporting `MADV_DONTNEED`
- Permission issues (SELinux, seccomp)

**Alert:**
```
if total_madvise_failures > 0:
    # System configuration issue
    # Check: kernel version, permissions, seccomp policies
```

---

## Per-Class Stats Array

The `classes` array contains 8 objects (one per size class). Each object has identical structure.

### Size Classes

| Index | Object Size | Objects/Slab |
|-------|-------------|--------------|
| 0     | 64B         | 63           |
| 1     | 96B         | 42           |
| 2     | 128B        | 31           |
| 3     | 192B        | 21           |
| 4     | 256B        | 15           |
| 5     | 384B        | 10           |
| 6     | 512B        | 7            |
| 7     | 768B        | 5            |

---

## Class Object Schema

```json
{
  "version": 1,
  "class_index": 3,
  "object_size": 192,
  "slow_path_hits": 50,
  "new_slab_count": 5,
  "list_move_partial_to_full": 50,
  "list_move_full_to_partial": 50,
  "current_partial_null": 50,
  "current_partial_full": 0,
  "empty_slab_recycled": 50,
  "empty_slab_overflowed": 0,
  "slow_path_cache_miss": 5,
  "slow_path_epoch_closed": 0,
  "madvise_calls": 50,
  "madvise_bytes": 204800,
  "madvise_failures": 0,
  "epoch_close_calls": 10,
  "epoch_close_scanned_slabs": 20,
  "epoch_close_recycled_slabs": 18,
  "epoch_close_total_ns": 250000,
  "cache_size": 5,
  "cache_capacity": 32,
  "cache_overflow_len": 0,
  "total_partial_slabs": 0,
  "total_full_slabs": 0,
  "recycle_rate_pct": 100.00,
  "net_slabs": 0,
  "estimated_rss_bytes": 0
}
```

---

### Class Metadata

#### `version` (uint32)
Per-class stats struct version (tracks `SlabClassStats` evolution).

#### `class_index` (uint32)
Size class index (0-7). Matches array position.

#### `object_size` (uint32)
Object size in bytes for this class (64, 96, 128, 192, 256, 384, 512, 768).

---

### Slow Path Attribution

#### `slow_path_hits` (uint64)

**Type:** Monotonic counter  
**Semantics:** Allocations from this class that took slow path

**Usage:**
- Per-class performance tracking
- Identify which sizes cause most contention
- Sum across classes equals `total_slow_path_hits`

---

#### `new_slab_count` (uint64)

**Type:** Monotonic counter  
**Semantics:** New slabs allocated from OS for this class

**Usage:**
- Growth indicator for specific size
- Working set expansion tracking
- Correlates with `slow_path_cache_miss`

---

#### `slow_path_cache_miss` (uint64)

**Type:** Monotonic counter  
**Semantics:** Slow paths caused by empty cache for this class

**Usage:**
- Root cause: cache undersized for this class
- **Overlaps:** Implies `current_partial_null` was incremented
- Optimization: increase `cache_capacity` if high

---

#### `slow_path_epoch_closed` (uint64)

**Type:** Monotonic counter  
**Semantics:** Allocations rejected because epoch was CLOSING

**Usage:**
- Bug detection for this class
- Should be zero
- Non-zero = application error

---

### Slab List Management

#### `list_move_partial_to_full` (uint64)

**Type:** Monotonic counter  
**Semantics:** Count of slabs moved from partial list to full list

**Usage:**
- Allocation activity indicator
- Shows slabs becoming fully allocated
- High churn = many slabs cycling between states

---

#### `list_move_full_to_partial` (uint64)

**Type:** Monotonic counter  
**Semantics:** Count of slabs moved from full list to partial list

**Usage:**
- Deallocation activity indicator
- Shows slabs becoming partially free
- Balance with `list_move_partial_to_full` for steady state

---

#### `current_partial_null` (uint64)

**Type:** Monotonic counter  
**Semantics:** Allocations where `current_partial` slab pointer was NULL

**Usage:**
- Contention indicator (another thread took the slab)
- Cache miss indicator (no slab in cache)
- **Overlaps:** Incremented when `slow_path_cache_miss` increments

**Note:** This is a *symptom* counter. Root cause is either cache miss or contention.

---

#### `current_partial_full` (uint64)

**Type:** Monotonic counter  
**Semantics:** Allocations where `current_partial` slab was exhausted (no free slots)

**Usage:**
- Normal churn indicator
- Shows slab being filled completely
- High values = rapid allocation rate

---

### Slab Recycling

#### `empty_slab_recycled` (uint64)

**Type:** Monotonic counter  
**Semantics:** Slabs that became fully empty and were returned to cache

**Usage:**
- Reclamation effectiveness for this class
- Shows complete epoch drainage
- Feeds into `recycle_rate_pct` calculation

---

#### `empty_slab_overflowed` (uint64)

**Type:** Monotonic counter  
**Semantics:** Empty slabs that couldn't fit in cache (moved to overflow list)

**Usage:**
- Cache sizing for this class
- Overflow slabs are slower to reuse
- Tuning: increase `cache_capacity` if high

---

### Cache State

#### `cache_size` (uint32)

**Type:** Gauge  
**Semantics:** Current number of empty slabs in per-class cache array

**Usage:**
- Available fast-path allocations
- Cache utilization = `cache_size / cache_capacity`
- **Good:** 5-20 (enough buffer, not wasteful)
- **Warning:** 0 (no cache) or 32 (cache full)

---

#### `cache_capacity` (uint32)

**Type:** Configuration constant  
**Semantics:** Maximum cache size (currently hardcoded to 32)

**Usage:**
- Cache sizing reference
- If `cache_size` frequently hits this, increase capacity
- Tradeoff: memory overhead vs allocation speed

---

#### `cache_overflow_len` (uint32)

**Type:** Gauge  
**Semantics:** Current count of slabs in overflow list (beyond cache capacity)

**Usage:**
- Cache overflow indicator
- High values = cache undersized
- Overflow slabs slower to reuse (not in fast array)

---

### Current Slab Distribution

#### `total_partial_slabs` (uint32)

**Type:** Gauge  
**Semantics:** Current count of slabs in partial list (has free slots)

**Usage:**
- Available allocation capacity without slow path
- High values = fragmentation (many partially-used slabs)
- Low values = efficient packing

---

#### `total_full_slabs` (uint32)

**Type:** Gauge  
**Semantics:** Current count of slabs in full list (no free slots)

**Usage:**
- Fully-utilized slab count
- Doesn't contribute to allocation capacity
- Tracks with allocation pressure

---

### Derived Metrics

#### `recycle_rate_pct` (double)

**Type:** Calculated percentage  
**Semantics:** `(empty_slab_recycled / new_slab_count) * 100`

**Usage:**
- Reuse effectiveness for this class
- **Good:** > 90% (most slabs reused)
- **Warning:** < 50% (poor reuse)
- **Zero:** New allocations only (no drainage yet)

---

#### `net_slabs` (uint64)

**Type:** Calculated gauge  
**Semantics:** `new_slab_count - empty_slab_recycled` (slabs in active use)

**Usage:**
- Per-class memory footprint
- Memory contribution: `net_slabs * 4096 bytes`
- Tracks active working set for this size

---

#### `estimated_rss_bytes` (uint64)

**Type:** Calculated gauge  
**Semantics:** `net_slabs * page_size` (estimated RSS contribution)

**Usage:**
- Per-class RSS estimate
- Sum across classes = `estimated_slab_rss_bytes` (global)
- Memory attribution by size class

---

### Reclamation Metrics (Per-Class)

#### `madvise_calls` (uint64)

**Type:** Monotonic counter  
**Semantics:** `madvise()` calls for this class

**Usage:**
- Per-class reclamation activity
- Shows which sizes generate most reclamation
- Sum across classes = `total_madvise_calls`

---

#### `madvise_bytes` (uint64)

**Type:** Monotonic counter  
**Semantics:** Bytes passed to `madvise()` for this class

**Usage:**
- Memory returned to OS for this size
- Per-class reclamation effectiveness
- Sum across classes = `total_madvise_bytes`

---

#### `madvise_failures` (uint64)

**Type:** Monotonic counter  
**Semantics:** Failed `madvise()` calls for this class

**Usage:**
- Per-class error tracking
- Should always be zero
- Non-zero = system configuration issue

---

### Epoch-Close Telemetry (Phase 2.1)

#### `epoch_close_calls` (uint64)

**Type:** Monotonic counter  
**Semantics:** Number of times `epoch_close()` was called for this class

**Usage:**
- Epoch lifecycle tracking
- Shows reclamation attempt frequency
- High rate indicates active epoch rotation

**Interpretation:**
```
epoch_close_rate = delta(epoch_close_calls) / delta(time)
# Should correlate with application request completion rate
```

---

#### `epoch_close_scanned_slabs` (uint64)

**Type:** Monotonic counter  
**Semantics:** Total slabs examined during `epoch_close()` for this class

**Usage:**
- Reclamation workload indicator
- Shows how many slabs needed inspection
- Higher values = more work per epoch_close

**Analysis:**
```
avg_scanned_per_close = epoch_close_scanned_slabs / epoch_close_calls
# High average suggests many partial slabs per epoch
```

---

#### `epoch_close_recycled_slabs` (uint64)

**Type:** Monotonic counter  
**Semantics:** Slabs successfully recycled during `epoch_close()` for this class

**Usage:**
- Reclamation effectiveness
- Shows how many empty slabs were reclaimed
- Compare with `scanned_slabs` for yield ratio

**Key metric:**
```
reclamation_yield = epoch_close_recycled_slabs / epoch_close_scanned_slabs
# Good: > 0.8 (80%+ of scanned slabs are recyclable)
# Warning: < 0.3 (low reclamation efficiency)
```

---

#### `epoch_close_total_ns` (uint64)

**Type:** Monotonic counter (nanoseconds)  
**Semantics:** Total time spent in `epoch_close()` for this class

**Usage:**
- Performance overhead tracking
- Shows reclamation latency cost
- Measured using `CLOCK_MONOTONIC`

**Latency calculation:**
```
avg_epoch_close_latency_ms = (epoch_close_total_ns / epoch_close_calls) / 1e6
# Expected: < 1ms for typical workloads
# Alert if: > 10ms (indicates scanning bottleneck)
```

**Note:** This measures `epoch_close()` duration *including* slab scanning and recycling, but *excluding* the `madvise()` system call (which happens outside the lock).

---

## Usage Examples

### 1. Calculate Slow Path Rate

```python
def slow_path_rate(snap1, snap2):
    delta_hits = snap2.total_slow_path_hits - snap1.total_slow_path_hits
    delta_time = (snap2.timestamp_ns - snap1.timestamp_ns) / 1e9
    return delta_hits / delta_time

rate = slow_path_rate(snapshot_before, snapshot_after)
print(f"Slow path rate: {rate:.2f} hits/sec")
```

---

### 2. Detect Memory Leaks

```python
def detect_leak(snapshots):
    # Linear growth in net_slabs suggests leak
    net_slabs = [s.net_slabs for s in snapshots]
    if all(net_slabs[i] < net_slabs[i+1] for i in range(len(net_slabs)-1)):
        return True  # Monotonically increasing = leak
    return False
```

---

### 3. Identify Hot Size Classes

```python
def hot_classes(snapshot):
    return sorted(
        snapshot.classes,
        key=lambda c: c.slow_path_hits,
        reverse=True
    )[:3]  # Top 3 by slow path activity
```

---

### 4. Cache Effectiveness

```python
def cache_hit_rate(snapshot):
    total_fast = sum(c.new_slab_count - c.slow_path_cache_miss 
                     for c in snapshot.classes)
    total_allocs = sum(c.new_slab_count for c in snapshot.classes)
    return total_fast / total_allocs if total_allocs > 0 else 1.0
```

---

### 5. Epoch-Close Reclamation Metrics (Phase 2.1)

```python
def epoch_close_metrics(snap1, snap2):
    """Calculate epoch_close performance between two snapshots."""
    delta_time = (snap2.timestamp_ns - snap1.timestamp_ns) / 1e9
    
    # Global metrics across all classes
    delta_calls = sum(c.epoch_close_calls for c in snap2.classes) - \
                  sum(c.epoch_close_calls for c in snap1.classes)
    delta_scanned = sum(c.epoch_close_scanned_slabs for c in snap2.classes) - \
                    sum(c.epoch_close_scanned_slabs for c in snap1.classes)
    delta_recycled = sum(c.epoch_close_recycled_slabs for c in snap2.classes) - \
                     sum(c.epoch_close_recycled_slabs for c in snap1.classes)
    delta_ns = sum(c.epoch_close_total_ns for c in snap2.classes) - \
               sum(c.epoch_close_total_ns for c in snap1.classes)
    
    return {
        "epoch_close_rate": delta_calls / delta_time,  # calls/sec
        "avg_latency_ms": (delta_ns / delta_calls / 1e6) if delta_calls > 0 else 0,
        "reclamation_yield": (delta_recycled / delta_scanned) if delta_scanned > 0 else 0,
        "slabs_recycled_rate": delta_recycled / delta_time,  # slabs/sec
    }

# Example usage
metrics = epoch_close_metrics(snapshot_before, snapshot_after)
print(f"Epoch close rate: {metrics['epoch_close_rate']:.2f} calls/sec")
print(f"Average latency: {metrics['avg_latency_ms']:.3f} ms")
print(f"Reclamation yield: {metrics['reclamation_yield']:.1%}")
```

---

### 6. Multi-Threading Contention Metrics (Phase 2.2)

Phase 2.2 adds HFT-safe contention observability via Tier 0 probe pattern (zero jitter, <0.1% overhead).

**New per-class fields:**

| Field | Type | Description |
|-------|------|-------------|
| `bitmap_alloc_cas_retries` | uint64 | Total CAS retry loops during bitmap allocation (not attempts) |
| `bitmap_free_cas_retries` | uint64 | Total CAS retry loops during bitmap free (not attempts) |
| `current_partial_cas_failures` | uint64 | Pointer-swap CAS failures on current_partial |
| `bitmap_alloc_attempts` | uint64 | Successful bitmap allocations (denominator for retries/op) |
| `bitmap_free_attempts` | uint64 | Successful bitmap frees (denominator for retries/op) |
| `current_partial_cas_attempts` | uint64 | All current_partial CAS attempts (denominator for failure rate) |
| `lock_fast_acquire` | uint64 | Lock acquisitions where trylock succeeded (no blocking) |
| `lock_contended` | uint64 | Lock acquisitions where trylock failed first (had to block) |
| `avg_alloc_cas_retries_per_attempt` | float64 | Derived: `bitmap_alloc_cas_retries / bitmap_alloc_attempts` |
| `avg_free_cas_retries_per_attempt` | float64 | Derived: `bitmap_free_cas_retries / bitmap_free_attempts` |
| `lock_contention_rate` | float64 | Derived: `lock_contended / (lock_fast_acquire + lock_contended)` |

**Tier 0 Probe Pattern:**
- **Zero jitter:** No clock syscalls (clock_gettime costs ~60ns, unpredictable)
- **Low overhead:** ~2ns per lock acquisition (single trylock instruction)
- **Production-safe:** Always-on for HFT systems (variance >> mean requirement)

**Example JSON:**
```json
{
  "classes": [
    {
      "class_index": 2,
      "object_size": 128,
      "bitmap_alloc_cas_retries": 1500,
      "bitmap_free_cas_retries": 20,
      "current_partial_cas_failures": 85000,
      "bitmap_alloc_attempts": 100000,
      "bitmap_free_attempts": 98000,
      "current_partial_cas_attempts": 110000,
      "lock_fast_acquire": 250000,
      "lock_contended": 50000,
      "avg_alloc_cas_retries_per_attempt": 0.015,
      "avg_free_cas_retries_per_attempt": 0.0002,
      "lock_contention_rate": 0.1667
    }
  ]
}
```

**Diagnostic patterns:**

```python
def contention_diagnostics(snap1, snap2):
    """Analyze multi-threading contention between snapshots."""
    delta_time = (snap2.timestamp_ns - snap1.timestamp_ns) / 1e9
    
    for cls in snap2.classes:
        cls1 = snap1.classes[cls.class_index]
        
        # Lock contention rate
        delta_fast = cls.lock_fast_acquire - cls1.lock_fast_acquire
        delta_contended = cls.lock_contended - cls1.lock_contended
        lock_rate = delta_contended / (delta_fast + delta_contended) if (delta_fast + delta_contended) > 0 else 0
        
        # CAS retry rate
        delta_retries = cls.bitmap_alloc_cas_retries - cls1.bitmap_alloc_cas_retries
        delta_attempts = cls.bitmap_alloc_attempts - cls1.bitmap_alloc_attempts
        cas_retry_rate = delta_retries / delta_attempts if delta_attempts > 0 else 0
        
        print(f"Class {cls.object_size}B:")
        print(f"  Lock contention: {lock_rate:.1%}")
        print(f"  CAS retries/op: {cas_retry_rate:.4f}")
        
        # Alert thresholds
        if lock_rate > 0.20:
            print(f"  ⚠️  HIGH: Consider per-thread caches")
        if cas_retry_rate > 0.05:
            print(f"  ⚠️  HIGH: Consider larger slabs or reduce thread count")
```

**Healthy ranges (validated 1→16 threads):**
- **Lock contention:** <20% (plateaus, not exponential)
- **CAS retries (alloc):** <0.05 retries/op (excellent lock-free design)
- **CAS retries (free):** <0.001 retries/op (free path is uncontended)
- **current_partial failures:** 80-100% (NORMAL - includes expected state mismatches)

---

## Schema Evolution

### Adding Fields (Backward Compatible)

New fields can be added without breaking parsers:
- Parsers ignore unknown fields
- New fields must have sensible zero-value defaults
- Document in this reference

### Removing Fields (Breaking Change)

Requires `schema_version` increment:
- Deprecated fields set to sentinel values first
- Removal only after 2+ versions
- Tooling must handle old versions

---

## Related Documentation

- **Phase 3 Diagnostics:** See `--doctor` mode for human-readable analysis
- **Internal Structs:** `include/slab_stats.h` for C API
- **External Tools:** `temporal-slab-tools` repo for Go CLI utilities

---

**Last Updated:** 2025-02-07  
**Schema Version:** 1  
**Allocator Version:** Phase 3 (Actionable Diagnostics)
