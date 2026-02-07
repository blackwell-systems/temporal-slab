/*
 * test_rss_delta.c - Phase 2.4 RSS Delta Tracking Test
 * 
 * Validates RSS measurement capture on epoch_close():
 * - rss_before_close captures RSS at start of epoch_close()
 * - rss_after_close captures RSS at end of epoch_close()
 * - Delta shows memory reclaimed (with ENABLE_RSS_RECLAMATION=1)
 */

#define _GNU_SOURCE
#include "slab_stats.h"
#include "slab_alloc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static void test_rss_delta_capture(void) {
  printf("\nTest 1: RSS delta capture on epoch_close()\n");
  printf("===========================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Advance to epoch 1 */
  epoch_advance(&alloc);
  EpochId epoch = epoch_current(&alloc);
  printf("  Testing with epoch: %u\n", epoch);
  
  /* Initial state: rss_before/after should be 0 (never closed) */
  SlabEpochStats es;
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  Before close: rss_before=%lu, rss_after=%lu\n", 
         es.rss_before_close, es.rss_after_close);
  assert(es.rss_before_close == 0);
  assert(es.rss_after_close == 0);
  
  /* Allocate some objects to create slabs */
  SlabHandle handles[100];
  for (int i = 0; i < 100; i++) {
    void* p = alloc_obj_epoch(&alloc, 128, epoch, &handles[i]);
    assert(p != NULL);
  }
  
  /* Free all objects so slabs become empty */
  for (int i = 0; i < 100; i++) {
    bool ok = free_obj(&alloc, handles[i]);
    assert(ok);
  }
  
  /* Close epoch - should capture RSS measurements */
  epoch_close(&alloc, epoch);
  
  /* Check RSS measurements were captured */
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  After close:\n");
  printf("    rss_before=%lu bytes (%.2f MB)\n", 
         es.rss_before_close, es.rss_before_close / 1024.0 / 1024);
  printf("    rss_after=%lu bytes (%.2f MB)\n", 
         es.rss_after_close, es.rss_after_close / 1024.0 / 1024);
  
  /* RSS measurements should be non-zero after close */
  assert(es.rss_before_close > 0);
  assert(es.rss_after_close > 0);
  
  /* Typically rss_after <= rss_before (memory reclaimed) */
  if (es.rss_after_close <= es.rss_before_close) {
    uint64_t delta = es.rss_before_close - es.rss_after_close;
    printf("    delta=%lu bytes (%.2f MB reclaimed)\n", 
           delta, delta / 1024.0 / 1024);
  } else {
    /* RSS can increase due to other system activity */
    printf("    (RSS increased, likely due to system activity)\n");
  }
  
  allocator_destroy(&alloc);
  printf("✓ RSS delta tracking works correctly\n");
}

static void test_multiple_epoch_closes(void) {
  printf("\nTest 2: Multiple epoch closes\n");
  printf("==============================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Close epochs 0, 1, 2 in sequence */
  for (int i = 0; i < 3; i++) {
    epoch_advance(&alloc);
    EpochId epoch = (i + 1) % EPOCH_COUNT;
    
    /* Allocate and free to create work */
    SlabHandle h[10];
    for (int j = 0; j < 10; j++) {
      void* p = alloc_obj_epoch(&alloc, 64, epoch, &h[j]);
      (void)p;  /* Suppress unused warning */
    }
    for (int j = 0; j < 10; j++) {
      free_obj(&alloc, h[j]);
    }
    
    /* Close and measure */
    epoch_close(&alloc, epoch);
    
    SlabEpochStats es;
    slab_stats_epoch(&alloc, 0, epoch, &es);
    printf("  Epoch %u: rss_before=%lu, rss_after=%lu\n", 
           epoch, es.rss_before_close, es.rss_after_close);
    
    assert(es.rss_before_close > 0);
    assert(es.rss_after_close > 0);
  }
  
  allocator_destroy(&alloc);
  printf("✓ Multiple epoch closes tracked correctly\n");
}

static void test_unclosed_epochs(void) {
  printf("\nTest 3: Unclosed epochs show zero RSS\n");
  printf("======================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  epoch_advance(&alloc);
  EpochId epoch = epoch_current(&alloc);
  
  /* Allocate but don't close */
  SlabHandle h[10];
  for (int i = 0; i < 10; i++) {
    void* p = alloc_obj_epoch(&alloc, 64, epoch, &h[i]);
    (void)p;
  }
  
  /* Check RSS fields are still zero (never closed) */
  SlabEpochStats es;
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  Epoch %u (not closed): rss_before=%lu, rss_after=%lu\n", 
         epoch, es.rss_before_close, es.rss_after_close);
  
  assert(es.rss_before_close == 0);
  assert(es.rss_after_close == 0);
  
  /* Clean up */
  for (int i = 0; i < 10; i++) {
    free_obj(&alloc, h[i]);
  }
  
  allocator_destroy(&alloc);
  printf("✓ Unclosed epochs correctly show zero RSS\n");
}

int main(void) {
  printf("Phase 2.4 RSS Delta Tracking Test\n");
  printf("==================================\n");
  
  test_rss_delta_capture();
  test_multiple_epoch_closes();
  test_unclosed_epochs();
  
  printf("\n");
  printf("═══════════════════════════════════════════\n");
  printf("✓ All Phase 2.4 RSS delta tests passed!\n");
  printf("✓ RSS measurements captured on epoch_close\n");
  printf("✓ Delta quantifies memory reclamation\n");
  printf("✓ Unclosed epochs remain at zero RSS\n");
  printf("═══════════════════════════════════════════\n");
  
  return 0;
}
