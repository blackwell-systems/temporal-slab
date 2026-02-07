#ifndef SLAB_DIAGNOSTICS_H
#define SLAB_DIAGNOSTICS_H

#include "slab_alloc.h"
#include "slab_stats.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Actionable Diagnostics API - Phase 3
 * 
 * Transforms raw stats into actionable insights:
 * - Epoch leak detection (top offenders by RSS)
 * - Slow-path root cause analysis (% breakdown + recommendations)
 * - Reclamation effectiveness (honest context, not scores)
 * 
 * Design philosophy: Answer "what do I do?" not "what happened?"
 */

#define SLAB_DIAGNOSTICS_VERSION 1

/* ==================== Epoch Leak Detection ==================== */

/* Epoch leak candidate - epochs that should have drained but haven't
 * 
 * Detection criteria:
 * - state == EPOCH_CLOSING (no new allocations)
 * - alloc_count > 0 (objects still live)
 * - age_sec > threshold (been stuck for a while)
 * - estimated_rss_bytes > 0 (consuming memory)
 */
typedef struct EpochLeakCandidate {
  uint32_t class_index;              /* Size class index */
  uint32_t object_size;              /* Object size in bytes */
  EpochId epoch_id;                  /* Ring index (0-15) */
  uint64_t epoch_era;                /* Monotonic generation */
  char label[32];                    /* Semantic label (if set) */
  
  /* Leak indicators */
  uint64_t age_sec;                  /* Seconds since epoch opened */
  uint64_t alloc_count;              /* Live allocations (refcount) */
  uint64_t estimated_rss_bytes;      /* Memory footprint */
  
  /* Actionable context */
  uint32_t partial_slab_count;       /* Slabs with free space */
  uint32_t full_slab_count;          /* Slabs completely full */
  uint32_t reclaimable_slab_count;   /* Empty slabs ready to reclaim */
} EpochLeakCandidate;

/* Epoch leak report - top N candidates by RSS impact */
typedef struct EpochLeakReport {
  uint32_t version;                  /* = SLAB_DIAGNOSTICS_VERSION */
  uint32_t threshold_sec;            /* Age threshold used for detection */
  uint32_t candidate_count;          /* Number of leak candidates found */
  uint32_t top_count;                /* Number returned (min(candidate_count, max_top)) */
  EpochLeakCandidate* candidates;    /* Sorted by estimated_rss_bytes (desc) */
} EpochLeakReport;

/* Detect epoch leak candidates and return top N by RSS impact
 * 
 * PARAMETERS:
 *   alloc         - Allocator instance
 *   threshold_sec - Minimum age to consider (e.g., 60 = 1 minute)
 *   max_top       - Maximum candidates to return (e.g., 10)
 *   out           - Output buffer (caller must free out->candidates)
 * 
 * RETURNS: Number of candidates found (out->candidate_count)
 * 
 * USAGE:
 *   EpochLeakReport report;
 *   slab_detect_epoch_leaks(alloc, 60, 10, &report);
 *   for (uint32_t i = 0; i < report.top_count; i++) {
 *     EpochLeakCandidate* c = &report.candidates[i];
 *     printf("Leak: class=%u, epoch=%u, age=%lusec, rss=%.2fMB, label='%s'\n",
 *            c->class_index, c->epoch_id, c->age_sec, 
 *            c->estimated_rss_bytes / 1024.0 / 1024, c->label);
 *   }
 *   free(report.candidates);
 */
uint32_t slab_detect_epoch_leaks(SlabAllocator* alloc, uint32_t threshold_sec, 
                                   uint32_t max_top, EpochLeakReport* out);

/* ==================== Slow-Path Root Cause Analysis ==================== */

/* Slow-path attribution breakdown for a size class */
typedef struct SlowPathAttribution {
  uint32_t class_index;              /* Size class index */
  uint32_t object_size;              /* Object size in bytes */
  
  /* Raw counters */
  uint64_t total_slow_path_hits;     /* Total slow-path invocations */
  uint64_t cache_miss_count;         /* Needed new slab (mmap) */
  uint64_t epoch_closed_count;       /* Allocation rejected (CLOSING) */
  uint64_t partial_null_count;       /* No current_partial cached */
  uint64_t partial_full_count;       /* current_partial exhausted */
  
  /* Percentage breakdown (sums to ~100%) */
  double cache_miss_pct;             /* % due to cache miss */
  double epoch_closed_pct;           /* % due to epoch closed */
  double partial_null_pct;           /* % due to null current_partial */
  double partial_full_pct;           /* % due to full current_partial */
  
  /* Actionable recommendation */
  char recommendation[128];          /* Human-readable action */
} SlowPathAttribution;

/* Slow-path root cause report for all size classes */
typedef struct SlowPathReport {
  uint32_t version;                  /* = SLAB_DIAGNOSTICS_VERSION */
  uint32_t class_count;              /* Number of size classes analyzed */
  SlowPathAttribution* classes;      /* Per-class attribution (caller must free) */
} SlowPathReport;

/* Analyze slow-path root causes and generate recommendations
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   out   - Output buffer (caller must free out->classes)
 * 
 * USAGE:
 *   SlowPathReport report;
 *   slab_analyze_slow_path(alloc, &report);
 *   for (uint32_t i = 0; i < report.class_count; i++) {
 *     SlowPathAttribution* attr = &report.classes[i];
 *     if (attr->total_slow_path_hits > 0) {
 *       printf("Class %u (%uB): %.1f%% cache miss, %.1f%% epoch closed\n",
 *              attr->class_index, attr->object_size,
 *              attr->cache_miss_pct, attr->epoch_closed_pct);
 *       printf("  → %s\n", attr->recommendation);
 *     }
 *   }
 *   free(report.classes);
 */
void slab_analyze_slow_path(SlabAllocator* alloc, SlowPathReport* out);

/* ==================== Reclamation Effectiveness Report ==================== */

/* Per-epoch reclamation effectiveness data */
typedef struct EpochReclamation {
  uint32_t class_index;              /* Size class index */
  EpochId epoch_id;                  /* Ring index */
  uint64_t epoch_era;                /* Monotonic generation */
  
  /* Reclamation metrics */
  uint64_t slabs_recycled;           /* Slabs moved to cache on close */
  uint64_t bytes_madvised;           /* Bytes passed to madvise() */
  uint64_t rss_before;               /* RSS before epoch_close() */
  uint64_t rss_after;                /* RSS after epoch_close() */
  int64_t rss_delta;                 /* RSS change (negative = reclaimed) */
  
  /* Context */
  char label[32];                    /* Semantic label */
  bool was_closed;                   /* true if epoch_close() was called */
} EpochReclamation;

/* Reclamation effectiveness report - honest context, not scores */
typedef struct ReclamationReport {
  uint32_t version;                  /* = SLAB_DIAGNOSTICS_VERSION */
  
  /* Aggregate metrics */
  uint64_t total_madvise_calls;      /* Total madvise() invocations */
  uint64_t total_madvise_bytes;      /* Total bytes passed to madvise */
  uint64_t total_madvise_failures;   /* Total madvise() failures */
  
  /* Per-epoch breakdowns */
  uint32_t epoch_count;              /* Number of epochs analyzed */
  EpochReclamation* epochs;          /* Per-epoch data (caller must free) */
} ReclamationReport;

/* Analyze reclamation effectiveness across all epochs
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   out   - Output buffer (caller must free out->epochs)
 * 
 * USAGE:
 *   ReclamationReport report;
 *   slab_analyze_reclamation(alloc, &report);
 *   printf("Total madvise: %lu calls, %.2f MB, %lu failures\n",
 *          report.total_madvise_calls,
 *          report.total_madvise_bytes / 1024.0 / 1024,
 *          report.total_madvise_failures);
 *   
 *   for (uint32_t i = 0; i < report.epoch_count; i++) {
 *     EpochReclamation* e = &report.epochs[i];
 *     if (e->was_closed && e->rss_delta < 0) {
 *       printf("Epoch %u: Reclaimed %.2f MB\n", 
 *              e->epoch_id, -e->rss_delta / 1024.0 / 1024);
 *     }
 *   }
 *   free(report.epochs);
 */
void slab_analyze_reclamation(SlabAllocator* alloc, ReclamationReport* out);

#endif /* SLAB_DIAGNOSTICS_H */
