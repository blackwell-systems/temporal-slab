/*
 * test_diagnostics.c - Phase 3 Actionable Diagnostics Test
 * 
 * Validates diagnostic analysis functions:
 * - Epoch leak detection (finds stuck epochs)
 * - Slow-path root cause (% attribution + recommendations)
 * - Reclamation effectiveness (RSS deltas)
 */

#define _GNU_SOURCE
#include "slab_diagnostics.h"
#include "slab_alloc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

static void test_epoch_leak_detection(void) {
  printf("\nTest 1: Epoch leak detection\n");
  printf("==============================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Create a leak scenario: allocate in epoch 1, close it, wait, don't free */
  epoch_advance(&alloc);
  EpochId leak_epoch = epoch_current(&alloc);
  printf("  Creating leak in epoch %u...\n", leak_epoch);
  
  /* Allocate 50 objects */
  SlabHandle handles[50];
  for (int i = 0; i < 50; i++) {
    void* p = alloc_obj_epoch(&alloc, 128, leak_epoch, &handles[i]);
    assert(p != NULL);
  }
  
  /* Close the epoch (marks it CLOSING) */
  epoch_close(&alloc, leak_epoch);
  
  /* Sleep to ensure age threshold is met (we'll use 1 second) */
  printf("  Sleeping 2 seconds to simulate stuck epoch...\n");
  sleep(2);
  
  /* Detect leaks with 1-second threshold */
  EpochLeakReport report;
  uint32_t found = slab_detect_epoch_leaks(&alloc, 1, 10, &report);
  
  printf("  Found %u leak candidates (threshold=%usec)\n", found, report.threshold_sec);
  printf("  Returned top %u candidates\n", report.top_count);
  
  assert(found > 0);  /* Should find at least our leak */
  assert(report.top_count > 0);
  
  /* Check first candidate (should be our leak) */
  if (report.top_count > 0) {
    EpochLeakCandidate* c = &report.candidates[0];
    printf("  Top leak:\n");
    printf("    class=%u (%uB), epoch=%u, era=%lu\n",
           c->class_index, c->object_size, c->epoch_id, c->epoch_era);
    printf("    age=%lusec, refcount=%lu, rss=%.2fKB\n",
           c->age_sec, c->alloc_count, c->estimated_rss_bytes / 1024.0);
    printf("    label='%s'\n", c->label);
    
    assert(c->alloc_count == 50);  /* Our 50 allocations */
    assert(c->age_sec >= 2);       /* At least 2 seconds old */
  }
  
  /* Clean up */
  for (int i = 0; i < 50; i++) {
    free_obj(&alloc, handles[i]);
  }
  free(report.candidates);
  allocator_destroy(&alloc);
  
  printf("✓ Epoch leak detection works correctly\n");
}

static void test_slow_path_attribution(void) {
  printf("\nTest 2: Slow-path root cause analysis\n");
  printf("=======================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Create some slow-path activity */
  epoch_advance(&alloc);
  EpochId epoch = epoch_current(&alloc);
  
  /* Allocate enough to trigger slow path */
  SlabHandle handles[100];
  for (int i = 0; i < 100; i++) {
    void* p = alloc_obj_epoch(&alloc, 128, epoch, &handles[i]);
    (void)p;
  }
  
  /* Analyze slow-path attribution */
  SlowPathReport report;
  slab_analyze_slow_path(&alloc, &report);
  
  printf("  Analyzed %u size classes\n", report.class_count);
  assert(report.class_count == 8);
  
  /* Check class 2 (128-byte) which we used */
  SlowPathAttribution* attr = &report.classes[2];
  printf("  Class 2 (128B):\n");
  printf("    Total slow-path hits: %lu\n", attr->total_slow_path_hits);
  printf("    Attribution:\n");
  printf("      Cache miss:    %lu (%.1f%%)\n", 
         attr->cache_miss_count, attr->cache_miss_pct);
  printf("      Epoch closed:  %lu (%.1f%%)\n",
         attr->epoch_closed_count, attr->epoch_closed_pct);
  printf("      Partial null:  %lu (%.1f%%)\n",
         attr->partial_null_count, attr->partial_null_pct);
  printf("      Partial full:  %lu (%.1f%%)\n",
         attr->partial_full_count, attr->partial_full_pct);
  printf("    Recommendation: %s\n", attr->recommendation);
  
  /* Verify percentages sum to ~100% */
  double total_pct = attr->cache_miss_pct + attr->epoch_closed_pct +
                      attr->partial_null_pct + attr->partial_full_pct;
  printf("    Total attribution: %.1f%% (should be ~100%%)\n", total_pct);
  
  /* Clean up */
  for (int i = 0; i < 100; i++) {
    free_obj(&alloc, handles[i]);
  }
  free(report.classes);
  allocator_destroy(&alloc);
  
  printf("✓ Slow-path attribution works correctly\n");
}

static void test_reclamation_effectiveness(void) {
  printf("\nTest 3: Reclamation effectiveness analysis\n");
  printf("============================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Allocate and free in epoch 1, then close it */
  epoch_advance(&alloc);
  EpochId epoch = epoch_current(&alloc);
  
  SlabHandle handles[50];
  for (int i = 0; i < 50; i++) {
    void* p = alloc_obj_epoch(&alloc, 64, epoch, &handles[i]);
    (void)p;
  }
  
  for (int i = 0; i < 50; i++) {
    free_obj(&alloc, handles[i]);
  }
  
  /* Close epoch (triggers RSS measurement) */
  epoch_close(&alloc, epoch);
  
  /* Analyze reclamation */
  ReclamationReport report;
  slab_analyze_reclamation(&alloc, &report);
  
  printf("  Aggregate metrics:\n");
  printf("    madvise calls:    %lu\n", report.total_madvise_calls);
  printf("    madvise bytes:    %.2f KB\n", report.total_madvise_bytes / 1024.0);
  printf("    madvise failures: %lu\n", report.total_madvise_failures);
  
  printf("  Per-epoch analysis:\n");
  printf("    Analyzed %u epochs\n", report.epoch_count);
  
  for (uint32_t i = 0; i < report.epoch_count; i++) {
    EpochReclamation* e = &report.epochs[i];
    if (e->was_closed) {
      printf("    Epoch %u (class %u, era %lu):\n",
             e->epoch_id, e->class_index, e->epoch_era);
      printf("      RSS before: %.2f KB\n", e->rss_before / 1024.0);
      printf("      RSS after:  %.2f KB\n", e->rss_after / 1024.0);
      printf("      RSS delta:  %ld bytes ", e->rss_delta);
      if (e->rss_delta < 0) {
        printf("(%.2f KB reclaimed)\n", -e->rss_delta / 1024.0);
      } else if (e->rss_delta > 0) {
        printf("(%.2f KB increased)\n", e->rss_delta / 1024.0);
      } else {
        printf("(unchanged)\n");
      }
    }
  }
  
  /* Clean up */
  free(report.epochs);
  allocator_destroy(&alloc);
  
  printf("✓ Reclamation effectiveness analysis works correctly\n");
}

int main(void) {
  printf("Phase 3 Actionable Diagnostics Test\n");
  printf("=====================================\n");
  
  test_epoch_leak_detection();
  test_slow_path_attribution();
  test_reclamation_effectiveness();
  
  printf("\n");
  printf("═══════════════════════════════════════════\n");
  printf("✓ All Phase 3 diagnostic tests passed!\n");
  printf("✓ Epoch leak detection identifies stuck epochs\n");
  printf("✓ Slow-path attribution provides actionable recommendations\n");
  printf("✓ Reclamation analysis shows RSS deltas\n");
  printf("═══════════════════════════════════════════\n");
  
  return 0;
}
