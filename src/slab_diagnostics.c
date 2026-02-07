/*
 * slab_diagnostics.c - Phase 3 Actionable Diagnostics
 * 
 * Transforms raw observability data into actionable insights.
 */

#include "slab_diagnostics.h"
#include "slab_alloc_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Helper: Get current time in seconds since epoch */
static uint64_t now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec;
}

/* Helper: Compare function for qsort (descending by estimated_rss_bytes) */
static int compare_leak_candidates(const void* a, const void* b) {
  const EpochLeakCandidate* ca = (const EpochLeakCandidate*)a;
  const EpochLeakCandidate* cb = (const EpochLeakCandidate*)b;
  
  if (ca->estimated_rss_bytes > cb->estimated_rss_bytes) return -1;
  if (ca->estimated_rss_bytes < cb->estimated_rss_bytes) return 1;
  return 0;
}

/* ==================== Epoch Leak Detection ==================== */

uint32_t slab_detect_epoch_leaks(SlabAllocator* alloc, uint32_t threshold_sec,
                                   uint32_t max_top, EpochLeakReport* out) {
  if (!alloc || !out) return 0;
  
  out->version = SLAB_DIAGNOSTICS_VERSION;
  out->threshold_sec = threshold_sec;
  out->candidate_count = 0;
  out->top_count = 0;
  out->candidates = NULL;
  
  /* Allocate temp buffer for all potential candidates (8 classes × 16 epochs = 128) */
  EpochLeakCandidate* temp = (EpochLeakCandidate*)calloc(8 * 16, sizeof(EpochLeakCandidate));
  if (!temp) return 0;
  
  uint64_t current_time_ns = now_ns();
  uint32_t count = 0;
  
  /* Scan all size classes and epochs for leak candidates */
  for (uint32_t cls = 0; cls < 8; cls++) {
    for (uint32_t epoch = 0; epoch < 16; epoch++) {
      SlabEpochStats es;
      slab_stats_epoch(alloc, cls, epoch, &es);
      
      /* Detection criteria */
      if (es.state != EPOCH_CLOSING) continue;          /* Must be CLOSING */
      if (es.alloc_count == 0) continue;                /* Must have live allocations */
      if (es.open_since_ns == 0) continue;              /* Must have been opened */
      if (es.estimated_rss_bytes == 0) continue;        /* Must be using memory */
      
      uint64_t age_ns = current_time_ns - es.open_since_ns;
      uint64_t age_sec = age_ns / 1000000000ULL;
      
      if (age_sec < threshold_sec) continue;            /* Must exceed age threshold */
      
      /* Found a leak candidate */
      EpochLeakCandidate* c = &temp[count++];
      c->class_index = cls;
      c->object_size = es.object_size;
      c->epoch_id = epoch;
      c->epoch_era = es.epoch_era;
      memcpy(c->label, es.label, sizeof(c->label));
      c->age_sec = age_sec;
      c->alloc_count = es.alloc_count;
      c->estimated_rss_bytes = es.estimated_rss_bytes;
      c->partial_slab_count = es.partial_slab_count;
      c->full_slab_count = es.full_slab_count;
      c->reclaimable_slab_count = es.reclaimable_slab_count;
    }
  }
  
  out->candidate_count = count;
  
  if (count == 0) {
    free(temp);
    return 0;
  }
  
  /* Sort by RSS impact (descending) */
  qsort(temp, count, sizeof(EpochLeakCandidate), compare_leak_candidates);
  
  /* Return top N */
  uint32_t return_count = (count < max_top) ? count : max_top;
  out->candidates = (EpochLeakCandidate*)malloc(return_count * sizeof(EpochLeakCandidate));
  if (out->candidates) {
    memcpy(out->candidates, temp, return_count * sizeof(EpochLeakCandidate));
    out->top_count = return_count;
  }
  
  free(temp);
  return count;
}

/* ==================== Slow-Path Root Cause Analysis ==================== */

void slab_analyze_slow_path(SlabAllocator* alloc, SlowPathReport* out) {
  if (!alloc || !out) return;
  
  out->version = SLAB_DIAGNOSTICS_VERSION;
  out->class_count = 8;
  out->classes = (SlowPathAttribution*)calloc(8, sizeof(SlowPathAttribution));
  if (!out->classes) {
    out->class_count = 0;
    return;
  }
  
  for (uint32_t cls = 0; cls < 8; cls++) {
    SlabClassStats cs;
    slab_stats_class(alloc, cls, &cs);
    
    SlowPathAttribution* attr = &out->classes[cls];
    attr->class_index = cls;
    attr->object_size = cs.object_size;
    
    /* Raw counters */
    attr->total_slow_path_hits = cs.slow_path_hits;
    attr->cache_miss_count = cs.slow_path_cache_miss;
    attr->epoch_closed_count = cs.slow_path_epoch_closed;
    attr->partial_null_count = cs.current_partial_null;
    attr->partial_full_count = cs.current_partial_full;
    
    /* Compute percentage breakdown 
     * 
     * NOTE: Counters overlap! cache_miss implies partial_null.
     * Show all for transparency, but recommendation uses hierarchy:
     * 1. epoch_closed (rejects allocation)
     * 2. cache_miss (root cause: needed new slab)
     * 3. partial_null/full (symptom: contention)
     */
    if (attr->total_slow_path_hits > 0) {
      attr->cache_miss_pct = 100.0 * attr->cache_miss_count / attr->total_slow_path_hits;
      attr->epoch_closed_pct = 100.0 * attr->epoch_closed_count / attr->total_slow_path_hits;
      attr->partial_null_pct = 100.0 * attr->partial_null_count / attr->total_slow_path_hits;
      attr->partial_full_pct = 100.0 * attr->partial_full_count / attr->total_slow_path_hits;
    } else {
      attr->cache_miss_pct = 0.0;
      attr->epoch_closed_pct = 0.0;
      attr->partial_null_pct = 0.0;
      attr->partial_full_pct = 0.0;
    }
    
    /* Generate actionable recommendation */
    if (attr->total_slow_path_hits == 0) {
      snprintf(attr->recommendation, sizeof(attr->recommendation),
               "No slow-path hits (all allocations fast)");
    } else if (attr->epoch_closed_pct > 50.0) {
      snprintf(attr->recommendation, sizeof(attr->recommendation),
               "%.0f%% allocations into CLOSING epochs - fix epoch rotation logic",
               attr->epoch_closed_pct);
    } else if (attr->cache_miss_pct > 50.0) {
      snprintf(attr->recommendation, sizeof(attr->recommendation),
               "%.0f%% cache misses - consider increasing cache_capacity from 32",
               attr->cache_miss_pct);
    } else if (attr->partial_null_pct > 50.0) {
      snprintf(attr->recommendation, sizeof(attr->recommendation),
               "%.0f%% null current_partial - high contention or empty cache",
               attr->partial_null_pct);
    } else if (attr->partial_full_pct > 50.0) {
      snprintf(attr->recommendation, sizeof(attr->recommendation),
               "%.0f%% current_partial exhausted - normal churn pattern",
               attr->partial_full_pct);
    } else {
      snprintf(attr->recommendation, sizeof(attr->recommendation),
               "Mixed causes - no dominant bottleneck (%.0f%% cache, %.0f%% epoch, %.0f%% null, %.0f%% full)",
               attr->cache_miss_pct, attr->epoch_closed_pct, 
               attr->partial_null_pct, attr->partial_full_pct);
    }
  }
}

/* ==================== Reclamation Effectiveness Report ==================== */

void slab_analyze_reclamation(SlabAllocator* alloc, ReclamationReport* out) {
  if (!alloc || !out) return;
  
  out->version = SLAB_DIAGNOSTICS_VERSION;
  
  /* Get global aggregate metrics */
  SlabGlobalStats gs;
  slab_stats_global(alloc, &gs);
  out->total_madvise_calls = gs.total_madvise_calls;
  out->total_madvise_bytes = gs.total_madvise_bytes;
  out->total_madvise_failures = gs.total_madvise_failures;
  
  /* Allocate buffer for per-epoch data (8 classes × 16 epochs = 128) */
  out->epoch_count = 0;
  out->epochs = (EpochReclamation*)calloc(8 * 16, sizeof(EpochReclamation));
  if (!out->epochs) return;
  
  uint32_t count = 0;
  
  /* Collect per-epoch reclamation data */
  for (uint32_t cls = 0; cls < 8; cls++) {
    for (uint32_t epoch = 0; epoch < 16; epoch++) {
      SlabEpochStats es;
      slab_stats_epoch(alloc, cls, epoch, &es);
      
      /* Only include epochs that were closed (have RSS measurements) */
      if (es.rss_before_close == 0 && es.rss_after_close == 0) continue;
      
      EpochReclamation* e = &out->epochs[count++];
      e->class_index = cls;
      e->epoch_id = epoch;
      e->epoch_era = es.epoch_era;
      
      /* Note: We don't have per-epoch slab recycling counts or madvise bytes.
       * Those are global. So we show RSS delta as the primary metric. */
      e->slabs_recycled = 0;  /* Not tracked per-epoch currently */
      e->bytes_madvised = 0;  /* Not tracked per-epoch currently */
      e->rss_before = es.rss_before_close;
      e->rss_after = es.rss_after_close;
      e->rss_delta = (int64_t)es.rss_after_close - (int64_t)es.rss_before_close;
      
      memcpy(e->label, es.label, sizeof(e->label));
      e->was_closed = true;  /* Presence of RSS measurements implies close */
    }
  }
  
  out->epoch_count = count;
}
