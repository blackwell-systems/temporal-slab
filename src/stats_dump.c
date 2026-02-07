/*
 * stats_dump - Observability example for temporal-slab
 * 
 * Demonstrates dual output pattern:
 * - JSON to stdout (stable contract for tooling, jq, Prometheus, CI diffs)
 * - Text to stderr (human-readable debugging without polluting pipes)
 * 
 * Usage:
 *   ./stats_dump [--json] [--text]
 *   Default: both enabled
 *   
 * Examples:
 *   ./stats_dump                    # Both outputs
 *   ./stats_dump | jq .             # JSON only (pipe-friendly)
 *   ./stats_dump --no-json          # Text only to stderr
 *   ./stats_dump 2>/dev/null        # JSON only to stdout
 */

#include <slab_alloc.h>
#include <slab_stats.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Command-line flags */
static bool flag_json = true;
static bool flag_text = true;

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
    } else {
      fprintf(stderr, "Usage: %s [--json] [--no-json] [--text] [--no-text]\n", argv[0]);
      exit(1);
    }
  }
}

/* ==================== JSON Output (stdout) ==================== */

static void print_json_global(SlabAllocator* alloc) {
  SlabGlobalStats gs;
  slab_stats_global(alloc, &gs);
  
  printf("{\n");
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
  
  /* Output stats */
  if (flag_json) {
    print_json_global(alloc);
  }
  
  if (flag_text) {
    print_text_global(alloc);
    for (uint32_t cls = 0; cls < 8; cls++) {
      print_text_class(alloc, cls);
    }
  }
  
  slab_allocator_free(alloc);
  return 0;
}
