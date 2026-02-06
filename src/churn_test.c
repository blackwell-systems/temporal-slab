/*
 * churn_test.c - Phase 2.1 RSS Bounds Validation
 * 
 * Purpose: Validate that RSS stabilizes under churn (bounded memory growth)
 * 
 * Test strategy:
 *   1. Allocate objects to fill slabs
 *   2. Free objects in patterns that create empty slabs
 *   3. Track RSS over time - should stabilize (not grow unbounded)
 *   4. Verify empty_slab_recycled counters increase
 * 
 * Pass condition: RSS rises initially, then stabilizes or oscillates in a band
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "slab_alloc_internal.h"

/* Test parameters */
#define OBJECT_SIZE 128
#define NUM_OBJECTS (100 * 1000)    /* 100K live objects at steady state */
#define CHURN_CYCLES 1000           /* Number of churn cycles to run */
#define CHURN_SIZE (10 * 1000)      /* Free and reallocate 10K objects per cycle */
#define RSS_SAMPLE_INTERVAL 10      /* Sample RSS every 10 cycles */

/* CSV export */
static FILE* g_csv_file = NULL;

static void csv_write_header(void) {
  if (!g_csv_file) return;
  fprintf(g_csv_file, "allocator,cycle,rss_mib,slabs_allocated,slabs_recycled,slabs_overflowed\n");
}

static void csv_write_sample(int cycle, double rss_mib, uint64_t allocated, uint64_t recycled, uint64_t overflowed) {
  if (!g_csv_file) return;
  fprintf(g_csv_file, "temporal-slab,%d,%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
          cycle, rss_mib, allocated, recycled, overflowed);
}

/* Churn test: allocate/free in patterns that create empty slabs */
static void churn_test(void) {
  printf("\n=== Phase 2.1 Churn Test ===\n\n");
  printf("Configuration:\n");
  printf("  Object size:       %d bytes\n", OBJECT_SIZE);
  printf("  Steady state size: %d objects\n", NUM_OBJECTS);
  printf("  Churn per cycle:   %d objects\n", CHURN_SIZE);
  printf("  Total cycles:      %d\n\n", CHURN_CYCLES);

  SlabAllocator a;
  allocator_init(&a);

  /* Allocate handle array */
  SlabHandle* handles = (SlabHandle*)calloc(NUM_OBJECTS, sizeof(SlabHandle));
  if (!handles) {
    fprintf(stderr, "Failed to allocate handle array\n");
    exit(1);
  }

  /* Phase 1: Fill to steady state */
  printf("Phase 1: Filling to steady state (%d objects)...\n", NUM_OBJECTS);
  for (int i = 0; i < NUM_OBJECTS; i++) {
    void* p = alloc_obj(&a, OBJECT_SIZE, &handles[i]);
    if (!p) {
      fprintf(stderr, "Allocation failed at %d\n", i);
      exit(1);
    }
    ((uint8_t*)p)[0] = 1;  /* Touch memory */
  }

  uint64_t rss_initial = read_rss_bytes_linux();
  printf("RSS after initial fill: %.2f MiB\n\n", (double)rss_initial / (1024.0 * 1024.0));

  /* Phase 2: Churn - repeatedly free and reallocate objects */
  printf("Phase 2: Churning (%d cycles, sampling RSS every %d cycles)...\n", 
         CHURN_CYCLES, RSS_SAMPLE_INTERVAL);

  uint64_t rss_min = rss_initial;
  uint64_t rss_max = rss_initial;
  int sample_count = 0;

  /* Calculate objects per slab for concentrated frees */
  uint32_t objects_per_slab = slab_object_count(OBJECT_SIZE);
  printf("Objects per slab: %u (concentrating frees to create empty slabs)\n\n", objects_per_slab);

  for (int cycle = 0; cycle < CHURN_CYCLES; cycle++) {
    /* Concentrated free pattern: free entire slabs worth of objects contiguously
     * This maximizes the chance of creating completely empty slabs */
    int start_idx = (cycle * objects_per_slab) % NUM_OBJECTS;
    
    for (int i = 0; i < CHURN_SIZE; i++) {
      int idx = (start_idx + i) % NUM_OBJECTS;
      if (!free_obj(&a, handles[idx])) {
        fprintf(stderr, "Free failed at cycle %d, index %d\n", cycle, idx);
        exit(1);
      }
    }

    /* Reallocate in same pattern (may reuse recycled slabs) */
    for (int i = 0; i < CHURN_SIZE; i++) {
      int idx = (start_idx + i) % NUM_OBJECTS;
      void* p = alloc_obj(&a, OBJECT_SIZE, &handles[idx]);
      if (!p) {
        fprintf(stderr, "Reallocation failed at cycle %d, index %d\n", cycle, idx);
        exit(1);
      }
      ((uint8_t*)p)[0] = 1;  /* Touch memory */
    }

    /* Sample RSS periodically */
    if (cycle % RSS_SAMPLE_INTERVAL == 0 || cycle == CHURN_CYCLES - 1) {
      uint64_t rss = read_rss_bytes_linux();
      double rss_mib = (double)rss / (1024.0 * 1024.0);
      
      if (rss < rss_min) rss_min = rss;
      if (rss > rss_max) rss_max = rss;
      
      /* Get recycling stats for 128B size class (index 2) */
      PerfCounters pc;
      get_perf_counters(&a, 2, &pc);
      
      printf("  Cycle %4d: RSS = %.2f MiB\n", cycle, rss_mib);
      csv_write_sample(cycle, rss_mib, pc.new_slab_count, pc.empty_slab_recycled, pc.empty_slab_overflowed);
      sample_count++;
    }
  }

  uint64_t rss_final = read_rss_bytes_linux();
  
  printf("\n--- RSS Analysis ---\n");
  printf("RSS initial:  %.2f MiB\n", (double)rss_initial / (1024.0 * 1024.0));
  printf("RSS final:    %.2f MiB\n", (double)rss_final / (1024.0 * 1024.0));
  printf("RSS min:      %.2f MiB\n", (double)rss_min / (1024.0 * 1024.0));
  printf("RSS max:      %.2f MiB\n", (double)rss_max / (1024.0 * 1024.0));
  printf("RSS range:    %.2f MiB (max - min)\n", 
         (double)(rss_max - rss_min) / (1024.0 * 1024.0));
  
  double growth = (double)(rss_final - rss_initial) / (double)rss_initial * 100.0;
  printf("RSS growth:   %.1f%% (final vs initial)\n", growth);

  /* Get counters */
  PerfCounters counters;
  get_perf_counters(&a, 1, &counters);  /* 128B is size class 1 */

  printf("\n--- Phase 2.1 Recycling Counters ---\n");
  printf("New slabs allocated:        %" PRIu64 "\n", counters.new_slab_count);
  printf("Empty slabs recycled:       %" PRIu64 "\n", counters.empty_slab_recycled);
  printf("Empty slabs overflowed:     %" PRIu64 " (cache full)\n", counters.empty_slab_overflowed);
  
  uint64_t total_recycled = counters.empty_slab_recycled + counters.empty_slab_overflowed;
  if (total_recycled > 0) {
    double recycle_ratio = (double)total_recycled / (double)counters.new_slab_count * 100.0;
    printf("Recycling ratio:            %.1f%% (recycled / allocated)\n", recycle_ratio);
  }

  /* Pass/Fail Criteria */
  printf("\n--- Pass/Fail Criteria ---\n");
  
  bool rss_bounded = (growth < 50.0);  /* Allow up to 50% growth */
  printf("RSS growth < 50%%:           %s (%.1f%%)\n", 
         rss_bounded ? "PASS" : "FAIL", growth);
  
  bool recycling_works = (total_recycled > 0);
  printf("Empty slab recycling works: %s (%" PRIu64 " slabs recycled)\n",
         recycling_works ? "PASS" : "FAIL", total_recycled);

  if (rss_bounded && recycling_works) {
    printf("\n=== PASS: RSS bounded under churn ===\n");
  } else {
    printf("\n=== FAIL: RSS unbounded or recycling broken ===\n");
  }

  free(handles);
  allocator_destroy(&a);
}

int main(int argc, char** argv) {
  /* Parse --csv argument */
  const char* csv_path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
      csv_path = argv[i + 1];
      i++;
    }
  }
  
  /* Open CSV file if requested */
  if (csv_path) {
    g_csv_file = fopen(csv_path, "w");
    if (!g_csv_file) {
      fprintf(stderr, "Failed to open CSV file: %s\n", csv_path);
      return 1;
    }
    csv_write_header();
  }
  
  churn_test();
  
  if (g_csv_file) {
    fclose(g_csv_file);
    printf("\nCSV written to: %s\n", csv_path);
  }
  
  return 0;
}
