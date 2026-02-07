/*
 * stats_dump - Observability tool for temporal-slab
 * 
 * Demonstrates dual output pattern:
 * - JSON to stdout (stable contract for tooling, jq, Prometheus, CI diffs)
 * - Text to stderr (human-readable debugging without polluting pipes)
 * 
 * Phase 3: Added --doctor mode for actionable diagnostics
 * 
 * Usage:
 *   ./stats_dump [--json] [--text] [--doctor]
 *   Default: --json --text (both outputs)
 *   
 * Examples:
 *   ./stats_dump                    # Both outputs
 *   ./stats_dump | jq .             # JSON only (pipe-friendly)
 *   ./stats_dump --no-json          # Text only to stderr
 *   ./stats_dump 2>/dev/null        # JSON only to stdout
 *   ./stats_dump --doctor           # Actionable diagnostics (Phase 3)
 */

#include <slab_alloc.h>
#include <slab_stats.h>
#include <slab_diagnostics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

/* Command-line flags */
static bool flag_json = true;
static bool flag_text = true;
static bool flag_doctor = false;

static void parse_args(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) {
      flag_json = true;
    } else if (strcmp(argv[i], "--no-json") == 0) {
      flag_json = false;
    } else if (strcmp(argv[i], "--text") == 0) {
      flag_text = true;
    } else if (strcmp(argv[i], "--no-text") == 0) {
      flag_text = false;
    } else if (strcmp(argv[i], "--doctor") == 0) {
      flag_doctor = true;
      flag_json = false;  /* Doctor mode replaces JSON */
      flag_text = false;  /* Doctor mode replaces text */
    } else {
      fprintf(stderr, "Usage: %s [--json] [--no-json] [--text] [--no-text] [--doctor]\n", argv[0]);
      exit(1);
    }
  }
}

/* ==================== JSON Output (stdout) ==================== */

/* JSON string escaper (prevents invalid JSON from special characters in labels) */
static void print_json_string(const char* s) {
  if (!s) {
    printf("\"\"");
    return;
  }
  
  printf("\"");
  for (const char* p = s; *p && (p - s) < 31; p++) {
    switch (*p) {
      case '"':  printf("\\\""); break;
      case '\\': printf("\\\\"); break;
      case '\n': printf("\\n"); break;
      case '\r': printf("\\r"); break;
      case '\t': printf("\\t"); break;
      default:
        if (*p >= 32 && *p <= 126) {
          putchar(*p);
        } else {
          printf("\\u%04x", (unsigned char)*p);
        }
    }
  }
  printf("\"");
}

static void print_json_global(SlabAllocator* alloc) {
  SlabGlobalStats gs;
  slab_stats_global(alloc, &gs);
  
  /* Snapshot metadata for deterministic analysis */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  pid_t pid = getpid();
  long page_size = sysconf(_SC_PAGESIZE);
  
  printf("{\n");
  printf("  \"schema_version\": 1,\n");
  printf("  \"timestamp_ns\": %lu,\n", timestamp_ns);
  printf("  \"pid\": %d,\n", (int)pid);
  printf("  \"page_size\": %ld,\n", page_size);
  printf("  \"epoch_count\": 16,\n");  /* Hardcoded: allocator doesn't expose epoch_count publicly */
  printf("  \"version\": %u,\n", gs.version);
  printf("  \"current_epoch\": %u,\n", gs.current_epoch);
  printf("  \"active_epoch_count\": %u,\n", gs.active_epoch_count);
  printf("  \"closing_epoch_count\": %u,\n", gs.closing_epoch_count);
  printf("  \"total_slabs_allocated\": %lu,\n", gs.total_slabs_allocated);
  printf("  \"total_slabs_recycled\": %lu,\n", gs.total_slabs_recycled);
  printf("  \"net_slabs\": %lu,\n", gs.net_slabs);
  printf("  \"rss_bytes_current\": %lu,\n", gs.rss_bytes_current);
  printf("  \"estimated_slab_rss_bytes\": %lu,\n", gs.estimated_slab_rss_bytes);
  printf("  \"total_slow_path_hits\": %lu,\n", gs.total_slow_path_hits);
  printf("  \"total_cache_overflows\": %lu,\n", gs.total_cache_overflows);
  printf("  \"total_slow_cache_miss\": %lu,\n", gs.total_slow_cache_miss);
  printf("  \"total_slow_epoch_closed\": %lu,\n", gs.total_slow_epoch_closed);
  printf("  \"total_madvise_calls\": %lu,\n", gs.total_madvise_calls);
  printf("  \"total_madvise_bytes\": %lu,\n", gs.total_madvise_bytes);
  printf("  \"total_madvise_failures\": %lu,\n", gs.total_madvise_failures);
  printf("  \"classes\": [\n");
  
  for (uint32_t cls = 0; cls < 8; cls++) {
    SlabClassStats cs;
    slab_stats_class(alloc, cls, &cs);
    
    printf("    {\n");
    printf("      \"version\": %u,\n", cs.version);
    printf("      \"class_index\": %u,\n", cs.class_index);
    printf("      \"object_size\": %u,\n", cs.object_size);
    printf("      \"slow_path_hits\": %lu,\n", cs.slow_path_hits);
    printf("      \"new_slab_count\": %lu,\n", cs.new_slab_count);
    printf("      \"list_move_partial_to_full\": %lu,\n", cs.list_move_partial_to_full);
    printf("      \"list_move_full_to_partial\": %lu,\n", cs.list_move_full_to_partial);
    printf("      \"current_partial_null\": %lu,\n", cs.current_partial_null);
    printf("      \"current_partial_full\": %lu,\n", cs.current_partial_full);
    printf("      \"empty_slab_recycled\": %lu,\n", cs.empty_slab_recycled);
    printf("      \"empty_slab_overflowed\": %lu,\n", cs.empty_slab_overflowed);
    printf("      \"slow_path_cache_miss\": %lu,\n", cs.slow_path_cache_miss);
    printf("      \"slow_path_epoch_closed\": %lu,\n", cs.slow_path_epoch_closed);
    printf("      \"madvise_calls\": %lu,\n", cs.madvise_calls);
    printf("      \"madvise_bytes\": %lu,\n", cs.madvise_bytes);
    printf("      \"madvise_failures\": %lu,\n", cs.madvise_failures);
    printf("      \"cache_size\": %u,\n", cs.cache_size);
    printf("      \"cache_capacity\": %u,\n", cs.cache_capacity);
    printf("      \"cache_overflow_len\": %u,\n", cs.cache_overflow_len);
    printf("      \"total_partial_slabs\": %u,\n", cs.total_partial_slabs);
    printf("      \"total_full_slabs\": %u,\n", cs.total_full_slabs);
    printf("      \"recycle_rate_pct\": %.2f,\n", cs.recycle_rate_pct);
    printf("      \"net_slabs\": %lu,\n", cs.net_slabs);
    printf("      \"estimated_rss_bytes\": %lu\n", cs.estimated_rss_bytes);
    printf("    }%s\n", cls < 7 ? "," : "");
  }
  
  printf("  ],\n");  /* Changed: added comma for epochs array */
  
  /* Phase 2.3: Epochs array (optional field for backward compat) */
  printf("  \"epochs\": [\n");
  
  for (uint32_t epoch_id = 0; epoch_id < 16; epoch_id++) {
    /* Aggregate epoch stats across all size classes */
    uint64_t total_partial = 0;
    uint64_t total_full = 0;
    uint64_t total_reclaimable = 0;
    
    for (uint32_t cls = 0; cls < 8; cls++) {
      SlabEpochStats es;
      slab_stats_epoch(alloc, cls, epoch_id, &es);
      
      total_partial += es.partial_slab_count;
      total_full += es.full_slab_count;
      total_reclaimable += es.reclaimable_slab_count;
    }
    
    /* Get metadata */
    SlabEpochStats es;
    slab_stats_epoch(alloc, 0, epoch_id, &es);  /* Metadata same across classes */
    
    /* Calculate age using CLOCK_MONOTONIC */
    uint64_t age_sec = 0;
    if (es.open_since_ns > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
      if (now_ns >= es.open_since_ns) {
        age_sec = (now_ns - es.open_since_ns) / 1000000000ULL;
      }
    }
    
    printf("    {\n");
    printf("      \"epoch_id\": %u,\n", epoch_id);
    printf("      \"epoch_era\": %lu,\n", es.epoch_era);
    printf("      \"state\": \"%s\",\n", es.state == EPOCH_ACTIVE ? "ACTIVE" : "CLOSING");
    printf("      \"age_sec\": %lu,\n", age_sec);
    printf("      \"refcount\": %lu,\n", es.alloc_count);  /* Now tracks domain_refcount */
    printf("      \"label\": ");
    print_json_string(es.label);
    printf(",\n");
    printf("      \"total_partial_slabs\": %lu,\n", total_partial);
    printf("      \"total_full_slabs\": %lu,\n", total_full);
    printf("      \"total_reclaimable_slabs\": %lu,\n", total_reclaimable);
    printf("      \"estimated_rss_bytes\": %lu\n", (total_partial + total_full) * 4096);
    printf("    }%s\n", epoch_id < 15 ? "," : "");
  }
  
  printf("  ]\n");
  printf("}\n");
}

/* ==================== Text Output (stderr) ==================== */

static void print_text_global(SlabAllocator* alloc) {
  SlabGlobalStats gs;
  slab_stats_global(alloc, &gs);
  
  fprintf(stderr, "=== temporal-slab Stats Snapshot ===\n\n");
  
  fprintf(stderr, "Global:\n");
  fprintf(stderr, "  Current epoch: %u\n", gs.current_epoch);
  fprintf(stderr, "  Active epochs: %u | Closing: %u\n",
          gs.active_epoch_count, gs.closing_epoch_count);
  fprintf(stderr, "  \n");
  fprintf(stderr, "  Total slabs: %lu allocated, %lu recycled (net: %lu = %.2f MB)\n",
          gs.total_slabs_allocated, gs.total_slabs_recycled, gs.net_slabs,
          gs.net_slabs * 4096.0 / 1024 / 1024);
  fprintf(stderr, "  RSS: %.2f MB actual | %.2f MB estimated\n",
          gs.rss_bytes_current / 1024.0 / 1024,
          gs.estimated_slab_rss_bytes / 1024.0 / 1024);
  fprintf(stderr, "  \n");
  fprintf(stderr, "  Slow path: %lu hits\n", gs.total_slow_path_hits);
  fprintf(stderr, "    cache miss: %lu | epoch closed: %lu\n",
          gs.total_slow_cache_miss, gs.total_slow_epoch_closed);
  fprintf(stderr, "  Cache overflows: %lu\n", gs.total_cache_overflows);
  fprintf(stderr, "  \n");
  fprintf(stderr, "  RSS reclamation:\n");
  fprintf(stderr, "    madvise calls: %lu (%.2f MB reclaimed, %lu failures)\n",
          gs.total_madvise_calls,
          gs.total_madvise_bytes / 1024.0 / 1024,
          gs.total_madvise_failures);
  fprintf(stderr, "\n");
}

static void print_text_class(SlabAllocator* alloc, uint32_t cls) {
  SlabClassStats cs;
  slab_stats_class(alloc, cls, &cs);
  
  /* Skip classes with no activity */
  if (cs.new_slab_count == 0 && cs.total_partial_slabs == 0 && cs.total_full_slabs == 0) {
    return;
  }
  
  fprintf(stderr, "Size Class %u (%u bytes):\n", cls, cs.object_size);
  
  /* Slow path attribution */
  if (cs.slow_path_hits > 0) {
    fprintf(stderr, "  Slow path: %lu hits\n", cs.slow_path_hits);
    fprintf(stderr, "    cache miss: %lu (%.1f%%)\n",
            cs.slow_path_cache_miss,
            100.0 * cs.slow_path_cache_miss / (cs.slow_path_hits + 1));
    fprintf(stderr, "    epoch closed: %lu (%.1f%%)\n",
            cs.slow_path_epoch_closed,
            100.0 * cs.slow_path_epoch_closed / (cs.slow_path_hits + 1));
    fprintf(stderr, "    partial null: %lu | partial full: %lu\n",
            cs.current_partial_null, cs.current_partial_full);
  }
  
  /* Slab distribution */
  fprintf(stderr, "  Slabs: %u partial, %u full (%.2f KB RSS)\n",
          cs.total_partial_slabs, cs.total_full_slabs,
          cs.estimated_rss_bytes / 1024.0);
  
  /* Cache effectiveness */
  fprintf(stderr, "  Cache: %u/%u array, %u overflow (%.1f%% recycle rate)\n",
          cs.cache_size, cs.cache_capacity, cs.cache_overflow_len,
          cs.recycle_rate_pct);
  
  /* RSS reclamation */
  if (cs.madvise_calls > 0) {
    fprintf(stderr, "  madvise: %lu calls, %.2f KB reclaimed, %lu failures\n",
            cs.madvise_calls,
            cs.madvise_bytes / 1024.0,
            cs.madvise_failures);
  }
  
  fprintf(stderr, "\n");
}

/* ==================== Simulated Workload ==================== */

static void run_workload(SlabAllocator* alloc) {
  /* Simulate request processing with epoch domains */
  for (int cycle = 0; cycle < 10; cycle++) {
    EpochId epoch = epoch_current(alloc);
    
    /* Allocate objects in current epoch */
    void* objects[100];
    for (int i = 0; i < 100; i++) {
      objects[i] = slab_malloc_epoch(alloc, 128, epoch);
    }
    
    /* Free half of them (partial drainage) */
    for (int i = 0; i < 50; i++) {
      slab_free(alloc, objects[i]);
    }
    
    /* Advance epoch */
    epoch_advance(alloc);
    
    /* Free rest after epoch close */
    for (int i = 50; i < 100; i++) {
      slab_free(alloc, objects[i]);
    }
    
    /* Close the old epoch */
    epoch_close(alloc, epoch);
  }
}

/* ==================== Doctor Mode (Phase 3) ==================== */

static void print_doctor_diagnostics(SlabAllocator* alloc) {
  fprintf(stderr, "\n");
  fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
  fprintf(stderr, "  ALLOCATOR DIAGNOSTICS (Phase 3 --doctor mode)\n");
  fprintf(stderr, "═══════════════════════════════════════════════════════════════\n\n");
  
  /* 1. Epoch Leak Detection */
  fprintf(stderr, "━━━ 1. EPOCH LEAK DETECTION ━━━\n\n");
  
  EpochLeakReport leak_report;
  uint32_t leak_count = slab_detect_epoch_leaks(alloc, 60, 10, &leak_report);
  
  if (leak_count == 0) {
    fprintf(stderr, "  ✓ No epoch leaks detected (threshold: %usec)\n\n", leak_report.threshold_sec);
  } else {
    fprintf(stderr, "  ⚠ Found %u leak candidate(s) (showing top %u):\n\n", 
            leak_count, leak_report.top_count);
    
    for (uint32_t i = 0; i < leak_report.top_count; i++) {
      EpochLeakCandidate* c = &leak_report.candidates[i];
      fprintf(stderr, "  [%u] Class %u (%uB), Epoch %u (era %lu)\n",
              i + 1, c->class_index, c->object_size, c->epoch_id, c->epoch_era);
      fprintf(stderr, "      Age:       %lu seconds (stuck!)\n", c->age_sec);
      fprintf(stderr, "      Refcount:  %lu live allocations\n", c->alloc_count);
      fprintf(stderr, "      RSS:       %.2f MB\n", c->estimated_rss_bytes / 1024.0 / 1024);
      fprintf(stderr, "      Slabs:     %u partial, %u full, %u reclaimable\n",
              c->partial_slab_count, c->full_slab_count, c->reclaimable_slab_count);
      
      if (c->label[0] != '\0') {
        fprintf(stderr, "      Label:     '%s'\n", c->label);
      }
      
      fprintf(stderr, "      → ACTION: Investigate why objects from this epoch haven't been freed\n");
      if (c->reclaimable_slab_count > 0) {
        fprintf(stderr, "                (Note: %u slabs are empty but not recycled yet)\n",
                c->reclaimable_slab_count);
      }
      fprintf(stderr, "\n");
    }
    
    free(leak_report.candidates);
  }
  
  /* 2. Slow-Path Root Cause Analysis */
  fprintf(stderr, "━━━ 2. SLOW-PATH ROOT CAUSE ANALYSIS ━━━\n\n");
  
  SlowPathReport slow_report;
  slab_analyze_slow_path(alloc, &slow_report);
  
  bool found_slow_path = false;
  for (uint32_t i = 0; i < slow_report.class_count; i++) {
    SlowPathAttribution* attr = &slow_report.classes[i];
    if (attr->total_slow_path_hits > 0) {
      found_slow_path = true;
      
      fprintf(stderr, "  Class %u (%uB): %lu slow-path hits\n",
              attr->class_index, attr->object_size, attr->total_slow_path_hits);
      fprintf(stderr, "    Attribution breakdown:\n");
      fprintf(stderr, "      Cache miss:    %lu (%.1f%%) - needed new slab from OS\n",
              attr->cache_miss_count, attr->cache_miss_pct);
      fprintf(stderr, "      Epoch closed:  %lu (%.1f%%) - allocation into CLOSING epoch\n",
              attr->epoch_closed_count, attr->epoch_closed_pct);
      fprintf(stderr, "      Partial null:  %lu (%.1f%%) - no cached current_partial\n",
              attr->partial_null_count, attr->partial_null_pct);
      fprintf(stderr, "      Partial full:  %lu (%.1f%%) - current_partial exhausted\n",
              attr->partial_full_count, attr->partial_full_pct);
      fprintf(stderr, "    → %s\n\n", attr->recommendation);
    }
  }
  
  if (!found_slow_path) {
    fprintf(stderr, "  ✓ No significant slow-path activity (all allocations fast)\n\n");
  }
  
  free(slow_report.classes);
  
  /* 3. Reclamation Effectiveness Report */
  fprintf(stderr, "━━━ 3. RECLAMATION EFFECTIVENESS ━━━\n\n");
  
  ReclamationReport reclaim_report;
  slab_analyze_reclamation(alloc, &reclaim_report);
  
  fprintf(stderr, "  Aggregate reclamation:\n");
  fprintf(stderr, "    madvise() calls:    %lu\n", reclaim_report.total_madvise_calls);
  fprintf(stderr, "    madvise() bytes:    %.2f MB\n", 
          reclaim_report.total_madvise_bytes / 1024.0 / 1024);
  fprintf(stderr, "    madvise() failures: %lu\n", reclaim_report.total_madvise_failures);
  
  if (reclaim_report.total_madvise_failures > 0) {
    fprintf(stderr, "    ⚠ madvise failures detected - check permissions or kernel config\n");
  }
  fprintf(stderr, "\n");
  
  if (reclaim_report.epoch_count == 0) {
    fprintf(stderr, "  (No epochs have been closed yet - no RSS deltas to report)\n\n");
  } else {
    fprintf(stderr, "  Per-epoch RSS deltas (%u closed epochs):\n\n", reclaim_report.epoch_count);
    
    for (uint32_t i = 0; i < reclaim_report.epoch_count; i++) {
      EpochReclamation* e = &reclaim_report.epochs[i];
      
      fprintf(stderr, "    Epoch %u (class %u, era %lu):\n",
              e->epoch_id, e->class_index, e->epoch_era);
      fprintf(stderr, "      RSS before: %.2f MB\n", e->rss_before / 1024.0 / 1024);
      fprintf(stderr, "      RSS after:  %.2f MB\n", e->rss_after / 1024.0 / 1024);
      
      if (e->rss_delta < 0) {
        fprintf(stderr, "      Delta:      %.2f MB reclaimed ✓\n", 
                -e->rss_delta / 1024.0 / 1024);
      } else if (e->rss_delta > 0) {
        fprintf(stderr, "      Delta:      +%.2f MB (increased - system activity?)\n",
                e->rss_delta / 1024.0 / 1024);
      } else {
        fprintf(stderr, "      Delta:      unchanged\n");
      }
      
      if (e->label[0] != '\0') {
        fprintf(stderr, "      Label:      '%s'\n", e->label);
      }
      fprintf(stderr, "\n");
    }
  }
  
  free(reclaim_report.epochs);
  
  fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
  fprintf(stderr, "  END DIAGNOSTICS\n");
  fprintf(stderr, "═══════════════════════════════════════════════════════════════\n");
}

/* ==================== Main ==================== */

int main(int argc, char** argv) {
  parse_args(argc, argv);
  
  SlabAllocator* alloc = slab_allocator_create();
  if (!alloc) {
    fprintf(stderr, "Failed to create allocator\n");
    return 1;
  }
  
  /* Run simulated workload */
  run_workload(alloc);
  
  /* Output mode selection */
  if (flag_doctor) {
    print_doctor_diagnostics(alloc);
  } else {
    /* Standard dual-output mode */
    if (flag_json) {
      print_json_global(alloc);
    }
    
    if (flag_text) {
      print_text_global(alloc);
      for (uint32_t cls = 0; cls < 8; cls++) {
        print_text_class(alloc, cls);
      }
    }
  }
  
  slab_allocator_free(alloc);
  return 0;
}
