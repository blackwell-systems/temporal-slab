#ifndef SLAB_STATS_H
#define SLAB_STATS_H

#include "slab_alloc.h"
#include <stdint.h>
#include <time.h>

/*
 * Observability API for temporal-slab
 * 
 * Provides snapshot-based statistics for diagnosing tail latency and RSS behavior.
 * 
 * VERSIONING:
 * - SLAB_STATS_VERSION increments on any struct field addition/removal
 * - Tools should check version and handle mismatches gracefully
 * - JSON output includes "version" field matching this constant
 * 
 * THREAD SAFETY:
 * - All functions are thread-safe
 * - Snapshots are not atomic across all fields (counters may increment during read)
 * - Brief locks are held to read list lengths and cache state
 * 
 * PERFORMANCE:
 * - Global stats: O(classes * epochs) = ~128 iterations + brief locks
 * - Class stats: O(epochs) = ~16 iterations + 2 brief locks  
 * - Epoch stats: O(partial_slabs) = scan to find reclaimable
 */

#define SLAB_STATS_VERSION 2  /* Phase 2.3: Added per-label contention maps */

/* ==================== Slowpath Sampling (Phase 2.5) ==================== */

/* Per-thread statistics for probabilistic end-to-end sampling
 * 
 * Answers: "Are tail latencies from allocator work or scheduler noise?"
 * 
 * DESIGN:
 * - TLS counters (no atomic contention)
 * - Probabilistic sampling (1/1024 operations)
 * - Wall vs CPU time split to detect WSL2/virtualization interference
 * - Explicit repair timing with reason codes
 * 
 * INTERPRETATION:
 * - wall >> cpu: Scheduler preemption (WSL2 noise)
 * - wall ≈ cpu: Real allocator work (locks, CAS storms, repairs)
 * - High repair_count: Invariant violations (publication races, stale views)
 */
#ifdef ENABLE_SLOWPATH_SAMPLING

typedef struct {
  /* End-to-end allocation sampling */
  uint64_t alloc_samples;          /* Number of sampled alloc_obj_epoch calls */
  uint64_t alloc_wall_ns_sum;      /* Total wall time of sampled allocs */
  uint64_t alloc_cpu_ns_sum;       /* Total CPU time of sampled allocs */
  uint64_t alloc_wall_ns_max;      /* Maximum wall time observed */
  uint64_t alloc_cpu_ns_max;       /* Maximum CPU time observed */
  
  /* Wait time (scheduler interference metric) */
  uint64_t alloc_wait_ns_sum;      /* Total wait time (wall - cpu) */
  uint64_t alloc_wait_ns_max;      /* Maximum wait time observed */
  
  /* Zombie repair timing */
  uint64_t repair_count;           /* Number of repairs performed */
  uint64_t repair_wall_ns_sum;     /* Total wall time in repairs */
  uint64_t repair_cpu_ns_sum;      /* Total CPU time in repairs */
  uint64_t repair_wall_ns_max;     /* Maximum wall time for single repair */
  uint64_t repair_cpu_ns_max;      /* Maximum CPU time for single repair */
  
  /* Repair wait time */
  uint64_t repair_wait_ns_sum;     /* Total repair wait (wall - cpu) */
  uint64_t repair_wait_ns_max;     /* Maximum repair wait observed */
  
  /* Repair reason attribution */
  uint64_t repair_reason_full_bitmap;    /* fc==0 && bitmap full */
  uint64_t repair_reason_list_mismatch;  /* list_id wrong */
  uint64_t repair_reason_other;          /* Other conditions */
} ThreadStats;

/* TLS storage (defined in slab_stats.c) */
extern __thread ThreadStats tls_stats;
extern __thread uint64_t tls_sample_ctr;

/* Sample rate (power of 2 for fast & mask) */
#define SAMPLE_RATE_MASK 1023u  /* 1/1024 sampling */

/* Helper to convert timespec to nanoseconds */
static inline uint64_t ns_from_ts(const struct timespec* t) {
  return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

/* Repair reason flags */
#define REPAIR_REASON_FULL_BITMAP    (1u << 0)
#define REPAIR_REASON_LIST_MISMATCH  (1u << 1)

/* Get current thread's sampling statistics
 * 
 * Returns a snapshot of TLS counters for the calling thread.
 * Call this at the end of a workload to print tail latency analysis.
 * 
 * THREAD SAFETY: Only safe to read from the same thread (TLS)
 * 
 * USAGE:
 *   ThreadStats stats = slab_stats_thread();
 *   if (stats.alloc_samples > 0) {
 *     uint64_t avg_wall = stats.alloc_wall_ns_sum / stats.alloc_samples;
 *     uint64_t avg_cpu = stats.alloc_cpu_ns_sum / stats.alloc_samples;
 *     if (avg_wall > avg_cpu * 2) {
 *       fprintf(stderr, "WARNING: wall >> cpu suggests scheduler interference\n");
 *     }
 *   }
 */
ThreadStats slab_stats_thread(void);

#endif /* ENABLE_SLOWPATH_SAMPLING */

/* ==================== Global Statistics ==================== */

/* Aggregate statistics across all size classes and epochs
 * 
 * Answers: "What's the overall allocator health?"
 */
typedef struct SlabGlobalStats {
  /* Version for forward compatibility */
  uint32_t version;  /* = SLAB_STATS_VERSION */
  
  /* Epoch state */
  uint32_t current_epoch;              /* Active epoch for new allocations */
  uint32_t active_epoch_count;         /* Epochs in ACTIVE state */
  uint32_t closing_epoch_count;        /* Epochs in CLOSING state */
  
  /* Slab lifecycle totals (sum across all classes) */
  uint64_t total_slabs_allocated;      /* Sum of new_slab_count */
  uint64_t total_slabs_recycled;       /* Sum of empty_slab_recycled */
  uint64_t net_slabs;                  /* allocated - recycled */
  
  /* RSS accounting */
  uint64_t rss_bytes_current;          /* Actual RSS from OS */
  uint64_t estimated_slab_rss_bytes;   /* net_slabs * 4096 */
  
  /* Performance totals */
  uint64_t total_slow_path_hits;       /* Sum across all classes */
  uint64_t total_cache_overflows;      /* Sum of empty_slab_overflowed */
  
  /* Slow-path attribution totals (Phase 2.0) */
  uint64_t total_slow_cache_miss;      /* New slab needed mmap */
  uint64_t total_slow_epoch_closed;    /* Allocation into CLOSING epoch */
  
  /* RSS reclamation totals (Phase 2.0) */
  uint64_t total_madvise_calls;
  uint64_t total_madvise_bytes;
  uint64_t total_madvise_failures;
  
  /* Phase 2.2: Lock-free contention totals */
  uint64_t total_bitmap_alloc_cas_retries;       /* Sum across all classes */
  uint64_t total_bitmap_free_cas_retries;
  uint64_t total_current_partial_cas_failures;
  uint64_t total_bitmap_alloc_attempts;          /* Denominators */
  uint64_t total_bitmap_free_attempts;
  uint64_t total_current_partial_cas_attempts;
} SlabGlobalStats;

/* ==================== Per-Class Statistics ==================== */

/* Statistics for a single size class
 * 
 * Answers: "Why is this size class slow/leaking?"
 */
typedef struct SlabClassStats {
  /* Version and identity */
  uint32_t version;                    /* = SLAB_STATS_VERSION */
  uint32_t class_index;                /* 0-7 */
  uint32_t object_size;                /* 64, 96, 128, 192, 256, 384, 512, 768 */
  
  /* Core perf counters (existing) */
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_null;
  uint64_t current_partial_full;
  uint64_t empty_slab_recycled;
  uint64_t empty_slab_overflowed;
  
  /* Slow-path attribution (Phase 2.0) */
  uint64_t slow_path_cache_miss;       /* Needed to allocate new slab from OS */
  uint64_t slow_path_epoch_closed;     /* Allocation rejected (epoch CLOSING) */
  
  /* RSS reclamation tracking (Phase 2.0) */
  uint64_t madvise_calls;              /* madvise(MADV_DONTNEED) invocations */
  uint64_t madvise_bytes;              /* Total bytes passed to madvise */
  uint64_t madvise_failures;           /* madvise returned error */
  
  /* Epoch-close telemetry (Phase 2.1) */
  uint64_t epoch_close_calls;          /* How many times epoch_close() called */
  uint64_t epoch_close_scanned_slabs;  /* Total slabs scanned for reclaimable */
  uint64_t epoch_close_recycled_slabs; /* Slabs actually recycled */
  uint64_t epoch_close_total_ns;       /* Total time spent in epoch_close() */
  
  /* Phase 2.2: Lock-free contention metrics */
  uint64_t bitmap_alloc_cas_retries;     /* CAS spins in allocation */
  uint64_t bitmap_free_cas_retries;      /* CAS spins in free */
  uint64_t current_partial_cas_failures; /* Fast-path pointer swap failures */
  
  /* Phase 2.2: Denominators for correct ratios */
  uint64_t bitmap_alloc_attempts;        /* Successful allocations */
  uint64_t bitmap_free_attempts;         /* Successful frees */
  uint64_t current_partial_cas_attempts; /* All current_partial CAS attempts */
  
  /* Phase 2.2: Lock contention (Tier 0 trylock probe, always-on) */
  uint64_t lock_fast_acquire;                /* Trylock succeeded (no contention) */
  uint64_t lock_contended;                   /* Trylock failed, had to wait */
  
  /* Phase 2.2+: Adaptive bitmap scanning observability */
  uint32_t scan_adapt_checks;                /* Total adaptation checks performed */
  uint32_t scan_adapt_switches;              /* Total mode switches (0<->1) */
  uint32_t scan_mode;                        /* Current mode (0=sequential, 1=randomized) */
  
  /* Phase 3: Live vs committed diagnostics (fragmentation detection) */
  uint64_t committed_bytes;                  /* Total bytes mmap'd for slabs */
  uint64_t live_bytes;                       /* Bytes currently allocated to live objects */
  uint64_t empty_slabs_count;                /* Number of slabs currently empty */
  
  /* Phase 2.2: Derived contention metrics */
  double avg_alloc_cas_retries_per_attempt;  /* retries / alloc_attempts */
  double avg_free_cas_retries_per_attempt;   /* retries / free_attempts */
  double current_partial_cas_failure_rate;   /* failures / cas_attempts */
  double lock_contention_rate;               /* contended / (fast + contended) */
  
#ifdef ENABLE_LABEL_CONTENTION
  /* Phase 2.3: Per-label contention attribution (compile-time optional)
   * 
   * These arrays map label_id (0-15) to contention counters.
   * - ID 0: unlabeled (no active domain)
   * - ID 1-15: registered semantic labels ("request", "frame", "gc", etc.)
   * 
   * To interpret label_ids, cross-reference with allocator->label_registry.labels[]
   */
  uint64_t lock_fast_acquire_by_label[16];         /* Per-label fast acquire counts */
  uint64_t lock_contended_by_label[16];            /* Per-label contention counts */
  uint64_t bitmap_alloc_cas_retries_by_label[16];  /* Per-label CAS retry counts */
  uint64_t bitmap_free_cas_retries_by_label[16];   /* Per-label free CAS retries */
#endif
  
  /* Cache state snapshot (requires cache_lock) */
  uint32_t cache_size;                 /* Slabs currently in array cache */
  uint32_t cache_capacity;             /* Max array cache size */
  uint32_t cache_overflow_len;         /* Slabs in overflow list */
  
  /* Slab distribution snapshot (requires sc->lock) */
  uint32_t total_partial_slabs;        /* Sum of partial.len across epochs */
  uint32_t total_full_slabs;           /* Sum of full.len across epochs */
  
  /* Derived metrics */
  double recycle_rate_pct;             /* 100 * recycled / (recycled + overflowed) */
  uint64_t net_slabs;                  /* new_slab_count - empty_slab_recycled */
  uint64_t estimated_rss_bytes;        /* net_slabs * 4096 */
} SlabClassStats;

/* ==================== Per-Epoch Statistics ==================== */

/* Statistics for a single epoch within a size class
 * 
 * Answers: "Which epoch is consuming memory?"
 */
typedef struct SlabEpochStats {
  /* Version and identity */
  uint32_t version;                    /* = SLAB_STATS_VERSION */
  uint32_t class_index;                /* 0-7 */
  uint32_t object_size;                /* Size class */
  EpochId epoch_id;                    /* Ring index (0-15) */
  uint64_t epoch_era;                  /* Monotonic generation (Phase 2.2) */
  EpochLifecycleState state;           /* ACTIVE or CLOSING */
  
  /* Phase 2.3: Rich metadata for debugging */
  uint64_t open_since_ns;              /* Timestamp when epoch became ACTIVE (0=never) */
  uint64_t alloc_count;                /* Number of live allocations in this epoch */
  char label[32];                      /* Semantic label (e.g., "request:abc", "frame:1234") */
  
  /* Phase 2.4: RSS delta tracking for reclamation quantification */
  uint64_t rss_before_close;           /* RSS at start of epoch_close() (0=never closed) */
  uint64_t rss_after_close;            /* RSS at end of epoch_close() (0=never closed) */
  
  /* Slab counts (requires sc->lock) */
  uint32_t partial_slab_count;
  uint32_t full_slab_count;
  
  /* Memory footprint */
  uint64_t estimated_rss_bytes;        /* (partial + full) * 4096 */
  
  /* Reclamation potential (requires scan) */
  uint32_t reclaimable_slab_count;     /* Slabs with free_count == object_count */
  uint64_t reclaimable_bytes;          /* reclaimable_slab_count * 4096 */
} SlabEpochStats;

/* ==================== Snapshot APIs ==================== */

/* Get global allocator statistics
 * 
 * Aggregates across all size classes and epochs.
 * 
 * COST: O(classes * epochs) iterations + brief locks (~100µs typical)
 * THREAD SAFETY: Safe to call concurrently
 * 
 * USAGE:
 *   SlabGlobalStats gs;
 *   slab_stats_global(alloc, &gs);
 *   fprintf(stderr, "Net RSS: %.2f MB\n", gs.estimated_slab_rss_bytes / 1024.0 / 1024);
 */
void slab_stats_global(SlabAllocator* alloc, SlabGlobalStats* out);

/* Get per-size-class statistics
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size_class - Class index (0=64B, 1=96B, ..., 7=768B)
 *   out        - Output buffer (must not be NULL)
 * 
 * COST: Brief locks on cache_lock + sc->lock (~10µs typical)
 * THREAD SAFETY: Safe to call concurrently
 * 
 * USAGE:
 *   SlabClassStats cs;
 *   slab_stats_class(alloc, 2, &cs);  // 128-byte class
 *   if (cs.madvise_failures > 0) {
 *     fprintf(stderr, "WARNING: madvise failing for class %u\n", cs.class_index);
 *   }
 */
void slab_stats_class(SlabAllocator* alloc, uint32_t size_class, SlabClassStats* out);

/* Get per-epoch statistics within a size class
 * 
 * PARAMETERS:
 *   alloc      - Allocator instance
 *   size_class - Class index (0-7)
 *   epoch      - Epoch ID (0-15)
 *   out        - Output buffer
 * 
 * COST: Brief lock on sc->lock + O(partial_slabs) scan (~50µs typical)
 * THREAD SAFETY: Safe to call concurrently
 * 
 * USAGE:
 *   SlabEpochStats es;
 *   slab_stats_epoch(alloc, 2, 3, &es);
 *   if (es.state == EPOCH_CLOSING && es.reclaimable_slab_count > 0) {
 *     fprintf(stderr, "Epoch %u has %u reclaimable slabs\n",
 *             es.epoch_id, es.reclaimable_slab_count);
 *   }
 */
void slab_stats_epoch(SlabAllocator* alloc, uint32_t size_class, EpochId epoch, SlabEpochStats* out);

#endif /* SLAB_STATS_H */
