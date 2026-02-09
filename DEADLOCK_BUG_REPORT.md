# Deadlock in temporal-slab allocator under contention (≥4 threads) within ~1000 ops

## Summary

Allocator hangs deterministically/probabilistically at 4+ threads. Benchmark ID collision bug was fixed; hang persists, so this is allocator-side.

## Reproduction

**Build:**
```bash
cd src
make benchmark_threads
```

**Run:**
```bash
./benchmark_threads 4
```

**Observed:** Hang within ~1000 operations/thread (usually in first few seconds)

## Minimal Evidence (Progress Stops)

Using instrumented debug build with logging every 1000 ops:

```
Creating allocator...
Creating 4 threads...
Created thread 0
[T0] Started, waiting for start signal
Created thread 1
[T1] Started, waiting for start signal
Created thread 2
[T2] Started, waiting for start signal
Created thread 3
All threads created, starting in 1 second...
[T3] Started, waiting for start signal
Starting benchmark...
Waiting for threads to complete...
Joining thread 0...
[T0] Begin allocations
[T0] Op 0
[T3] Begin allocations
[T3] Op 0
[T1] Begin allocations
[T1] Op 0
[T2] Begin allocations
[T2] Op 0
[T3] Op 1000
<hang forever - no further progress>
```

All threads enter allocation loop, T3 completes 1000 ops, then all threads freeze.

## Why This is Allocator-Side

1. **Benchmark bug fixed:** Replaced `pthread_self() % MAX_THREADS` indexing with explicit thread IDs (commit 20dddae)
2. **Hang still occurs** with corrected benchmark
3. **Hang occurs early** (~1000 ops), consistent with cyclic lock dependency rather than leak/slow creep
4. **Original benchmark also hung:** Testing with original buggy benchmark confirms hang existed before fix

## Thread Count Dependency

- **1 thread:** Works perfectly (735K ops/sec, completes in 136ms)
- **2 threads:** Unknown (not tested)
- **4+ threads:** Hangs consistently within seconds

## Suspected Causes (Most Likely First)

### 1. Lock Order Inversion

Involving:
- `sc->lock` vs `sc->cache_lock`
- Possibly label/epoch locks (`label_registry.lock`, `epoch_label_lock`) if those can be taken inside alloc/free paths

**Pattern:**
- Path A: `sc->lock` → `cache_lock`
- Path B: `cache_lock` → `sc->lock`

Results in circular wait when 2+ threads take paths A and B simultaneously.

### 2. List Mutation Under Insufficient Exclusion

- Slab moved between lists (full/partial/empty) while another thread assumes membership
- Double-remove / corrupt `next` pointers producing deadlock-like behavior inside list ops
- Recent "Option C" change (immediate empty slab reclamation) may have introduced new list manipulation race

### 3. CAS/Bitmap Path Holding Mutex While Spinning

- e.g., mutex held while retry loop waits for state another thread cannot reach
- Bitmap alloc/free uses `compare_exchange` in loops - if one holds a mutex while spinning, could block progress

## Investigation Constraints

- **Cannot use gdb attach / ptrace** in WSL environment (ptrace restrictions)
- **TSAN not usable** due to mmap/ASAN conflicts in WSL: `FATAL: ThreadSanitizer: unexpected memory mapping`
- **CI reproducible:** CI can hang for hours without timeouts (now mitigated with timeout in benchmark harness repo)

## Immediate Mitigation

✅ **Done in benchmark harness repo:**
- Added per-run `timeout 60s` around contention benchmark in CI
- Added job-level `timeout-minutes: 10`
- Prevents 6-hour workflow hangs while allocator bug is debugged

## Proposed Debugging Instrumentation (Low Overhead)

### Option A: Lock Rank Assertions

Give every lock a rank:
```c
#define RANK_CACHE_LOCK     10
#define RANK_SC_LOCK        20
#define RANK_EPOCH_LOCK     30
#define RANK_LABEL_LOCK     40
```

In lock acquisition macro:
```c
static __thread int highest_rank_held = 0;

#define LOCK_WITH_RANK(mutex, rank) do { \
  assert(rank >= highest_rank_held); \
  pthread_mutex_lock(mutex); \
  highest_rank_held = rank; \
} while (0)
```

This catches lock order inversion **the first time it happens**.

### Option B: Progress Heartbeat Watchdog

Add global atomic progress counter incremented every N ops:

```c
static _Atomic uint64_t progress_counter = 0;
```

Watchdog thread prints stacks if `progress_counter` stops changing for >2 seconds.

### Option C: Lock Trace Ring Buffer (Cheapest, Most Direct)

Per-thread ring buffer recording:
```c
enum LockPhase { TRY, ACQUIRED, RELEASED };

struct LockEvent {
  uint64_t timestamp_ns;
  const char* lock_name;
  const char* source_location;
  enum LockPhase phase;
};

static __thread struct LockEvent lock_trace[64];
static __thread int trace_idx = 0;
```

On hang, dump last 32 events per thread → immediate visibility into which lock each thread was trying to acquire when progress stopped.

## Current Lock Inventory

From `slab_alloc.c`:

1. **Per-size-class lock:** `sc->lock` (lines 755, 1597)
   - Protects slab lists (partial/full/empty)
   - Most frequently acquired

2. **Cache lock:** `sc->cache_lock` (lines 819, 846, 903, 1634)
   - Protects slab cache (free slab pool)
   - Acquired during slab allocation/return

3. **Label registry lock:** `label_registry.lock` (lines 738, 1996)
   - Protects label→ID mapping (bounded cardinality)
   - Acquired during epoch labeling

4. **Epoch label lock:** `epoch_label_lock` (lines 725, 2020)
   - Protects epoch metadata updates
   - Rarely written

5. **Registry lock:** `registry.lock` (lines 276, 302)
   - Protects allocator ID assignment
   - One-time acquisition per allocator

## Lock Acquisition Macros

Two variants:
```c
LOCK_WITH_PROBE(mutex, sc)       // Records contention metrics
LOCK_WITHOUT_PROBE(mutex)        // No metrics
```

Both wrap `pthread_mutex_lock()`.

## Next Steps

1. **Add lock rank assertions** (fastest path to catching inversion)
2. **Instrument with lock trace ring buffer** (shows exact sequence leading to deadlock)
3. **Manual code review** of lock acquisition order in:
   - `alloc_obj_epoch()` call chain
   - `free_obj()` call chain
   - `cache_pop()` / `cache_push()` paths
   - Recent Option C empty slab reclamation changes

## Related Commits

- **Benchmark fix:** 20dddae "bench: fix thread result indexing by passing explicit thread IDs"
- **Recent allocator change:** a909d38 "Reclaim empty slabs immediately (Option C fix)"
- **Diagnostic counters:** 778a577 "Add live_bytes/committed_bytes instrumentation"

## Environment

- **OS:** WSL2 (Linux 6.6.87.2-microsoft-standard-WSL2)
- **Compiler:** GCC (Ubuntu)
- **Allocator:** temporal-slab commit 0a6381a
- **Benchmark:** 100K ops/thread, 128-byte objects, epoch 0

## Contact

Reproduced by Claude Code during CI debugging session (2026-02-09).
Original issue: GitHub Actions contention test hanging for 6+ hours.
