/*
 * test_epoch_metadata.c - Phase 2.3 Epoch Metadata Test
 * 
 * Validates:
 * - open_since_ns is set on epoch_advance() and reads as valid timestamp
 * - alloc_count increments on allocations and decrements on frees
 * - alloc_count accurately tracks live allocations per epoch
 * - Metadata is correctly exposed via slab_stats_epoch()
 */

#define _GNU_SOURCE
#include "slab_stats.h"
#include "slab_alloc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

/* Test utilities - use system now_ns() instead */

static void test_open_since_timestamp(void) {
  printf("\nTest 1: open_since_ns timestamp tracking\n");
  printf("=========================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Initial epoch (0) should have open_since_ns = 0 (never explicitly opened) */
  SlabEpochStats es;
  slab_stats_epoch(&alloc, 0, 0, &es);
  printf("  Epoch 0 (initial): open_since_ns=%lu (should be 0)\n", es.open_since_ns);
  assert(es.open_since_ns == 0);
  
  /* Advance to epoch 1 - should set open_since_ns to current time */
  uint64_t before = now_ns();
  epoch_advance(&alloc);
  uint64_t after = now_ns();
  
  slab_stats_epoch(&alloc, 0, 1, &es);
  printf("  Epoch 1 (after advance): open_since_ns=%lu\n", es.open_since_ns);
  printf("    Expected range: [%lu, %lu]\n", before, after);
  
  assert(es.open_since_ns >= before);
  assert(es.open_since_ns <= after);
  
  /* Advance several epochs and verify each gets timestamp */
  for (int i = 0; i < 5; i++) {
    before = now_ns();
    epoch_advance(&alloc);
    after = now_ns();
    
    uint32_t epoch = (2 + i) % EPOCH_COUNT;
    slab_stats_epoch(&alloc, 0, epoch, &es);
    
    printf("  Epoch %u: open_since_ns=%lu (valid range)\n", epoch, es.open_since_ns);
    assert(es.open_since_ns >= before);
    assert(es.open_since_ns <= after);
  }
  
  allocator_destroy(&alloc);
  printf("✓ open_since_ns tracking works correctly\n");
}

static void test_alloc_count_tracking(void) {
  printf("\nTest 2: alloc_count refcount tracking\n");
  printf("======================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  epoch_advance(&alloc);
  EpochId epoch = epoch_current(&alloc);
  
  /* Initial state: alloc_count should be 0 */
  SlabEpochStats es;
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  Initial: epoch=%u, alloc_count=%lu\n", epoch, es.alloc_count);
  assert(es.alloc_count == 0);
  
  /* Allocate 10 objects - alloc_count should increment */
  SlabHandle handles[10];
  for (int i = 0; i < 10; i++) {
    void* p = alloc_obj_epoch(&alloc, 64, epoch, &handles[i]);
    assert(p != NULL);
  }
  
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  After 10 allocs: alloc_count=%lu\n", es.alloc_count);
  assert(es.alloc_count == 10);
  
  /* Free 5 objects - alloc_count should decrement */
  for (int i = 0; i < 5; i++) {
    bool ok = free_obj(&alloc, handles[i]);
    assert(ok);
  }
  
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  After 5 frees: alloc_count=%lu\n", es.alloc_count);
  assert(es.alloc_count == 5);
  
  /* Free remaining 5 - alloc_count should reach 0 */
  for (int i = 5; i < 10; i++) {
    bool ok = free_obj(&alloc, handles[i]);
    assert(ok);
  }
  
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  After all frees: alloc_count=%lu\n", es.alloc_count);
  assert(es.alloc_count == 0);
  
  allocator_destroy(&alloc);
  printf("✓ alloc_count tracking works correctly\n");
}

static void test_multi_epoch_isolation(void) {
  printf("\nTest 3: Multi-epoch alloc_count isolation\n");
  printf("==========================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Allocate in epoch 0 */
  SlabHandle h0[5];
  for (int i = 0; i < 5; i++) {
    void* p = alloc_obj_epoch(&alloc, 64, 0, &h0[i]);
    assert(p != NULL);
  }
  
  /* Advance to epoch 1 */
  epoch_advance(&alloc);
  EpochId epoch1 = epoch_current(&alloc);
  
  /* Allocate in epoch 1 */
  SlabHandle h1[3];
  for (int i = 0; i < 3; i++) {
    void* p = alloc_obj_epoch(&alloc, 64, epoch1, &h1[i]);
    assert(p != NULL);
  }
  
  /* Check counts are isolated */
  SlabEpochStats es0, es1;
  slab_stats_epoch(&alloc, 0, 0, &es0);
  slab_stats_epoch(&alloc, 0, epoch1, &es1);
  
  printf("  Epoch 0: alloc_count=%lu (expected 5)\n", es0.alloc_count);
  printf("  Epoch 1: alloc_count=%lu (expected 3)\n", es1.alloc_count);
  
  assert(es0.alloc_count == 5);
  assert(es1.alloc_count == 3);
  
  /* Free from epoch 0 - should not affect epoch 1 */
  for (int i = 0; i < 5; i++) {
    bool ok = free_obj(&alloc, h0[i]);
    assert(ok);
  }
  
  slab_stats_epoch(&alloc, 0, 0, &es0);
  slab_stats_epoch(&alloc, 0, epoch1, &es1);
  
  printf("  After freeing epoch 0:\n");
  printf("    Epoch 0: alloc_count=%lu (expected 0)\n", es0.alloc_count);
  printf("    Epoch 1: alloc_count=%lu (expected 3, unchanged)\n", es1.alloc_count);
  
  assert(es0.alloc_count == 0);
  assert(es1.alloc_count == 3);
  
  /* Free from epoch 1 */
  for (int i = 0; i < 3; i++) {
    bool ok = free_obj(&alloc, h1[i]);
    assert(ok);
  }
  
  slab_stats_epoch(&alloc, 0, epoch1, &es1);
  printf("  After freeing epoch 1: alloc_count=%lu\n", es1.alloc_count);
  assert(es1.alloc_count == 0);
  
  allocator_destroy(&alloc);
  printf("✓ Multi-epoch isolation works correctly\n");
}

static void test_metadata_reset_on_wraparound(void) {
  printf("\nTest 4: Metadata reset on epoch wraparound\n");
  printf("===========================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  printf("  Initial epoch after init: %u\n", epoch_current(&alloc));
  
  /* Epoch starts at 0, advance once to epoch 1 for allocation */
  epoch_advance(&alloc);
  EpochId test_epoch = epoch_current(&alloc);
  printf("  Starting test with epoch: %u\n", test_epoch);
  
  /* Allocate in current epoch */
  SlabHandle h[5];
  for (int i = 0; i < 5; i++) {
    void* p = alloc_obj_epoch(&alloc, 64, test_epoch, &h[i]);
    assert(p != NULL);
  }
  
  SlabEpochStats es;
  slab_stats_epoch(&alloc, 0, test_epoch, &es);
  printf("  Epoch %u (first time): alloc_count=%lu, open_since_ns=%lu\n", 
         test_epoch, es.alloc_count, es.open_since_ns);
  assert(es.alloc_count == 5);
  uint64_t first_open_time = es.open_since_ns;
  
  /* Free all objects */
  for (int i = 0; i < 5; i++) {
    free_obj(&alloc, h[i]);
  }
  
  /* Advance 16 more times to wrap back to test_epoch */
  printf("  Advancing 16 more epochs to wrap back to test_epoch...\n");
  for (int i = 0; i < 16; i++) {
    epoch_advance(&alloc);
    usleep(1000);  /* 1ms delay to ensure timestamp changes */
  }
  
  uint32_t current_raw = epoch_current(&alloc);
  uint32_t current = current_raw % EPOCH_COUNT;  /* epoch_current returns raw counter */
  printf("  Current epoch after wraparound: raw=%u, mod=%u (expected %u)\n", 
         current_raw, current, test_epoch);
  assert(current == test_epoch);
  
  /* Check test_epoch metadata was reset */
  slab_stats_epoch(&alloc, 0, test_epoch, &es);
  printf("  Epoch %u (after wraparound):\n", test_epoch);
  printf("    alloc_count=%lu (should be 0, reset)\n", es.alloc_count);
  printf("    open_since_ns=%lu (should be different from %lu)\n", 
         es.open_since_ns, first_open_time);
  
  assert(es.alloc_count == 0);
  assert(es.open_since_ns > first_open_time);
  
  allocator_destroy(&alloc);
  printf("✓ Metadata reset on wraparound works correctly\n");
}

static void test_label_initialization(void) {
  printf("\nTest 5: Label initialization and clearing\n");
  printf("==========================================\n");
  
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Check initial label is empty */
  SlabEpochStats es;
  slab_stats_epoch(&alloc, 0, 0, &es);
  printf("  Epoch 0 (initial): label='%s' (should be empty)\n", es.label);
  assert(es.label[0] == '\0');
  
  /* Advance and check new epoch has empty label */
  epoch_advance(&alloc);
  EpochId epoch = epoch_current(&alloc);
  slab_stats_epoch(&alloc, 0, epoch, &es);
  printf("  Epoch %u (after advance): label='%s' (should be empty)\n", epoch, es.label);
  assert(es.label[0] == '\0');
  
  /* Note: We don't test setting labels here because the API for that
   * hasn't been implemented yet. This test validates that labels are
   * properly initialized and cleared. */
  
  allocator_destroy(&alloc);
  printf("✓ Label initialization works correctly\n");
}

int main(void) {
  printf("Phase 2.3 Epoch Metadata Test\n");
  printf("==============================\n");
  
  test_open_since_timestamp();
  test_alloc_count_tracking();
  test_multi_epoch_isolation();
  test_metadata_reset_on_wraparound();
  test_label_initialization();
  
  printf("\n");
  printf("═══════════════════════════════════════════\n");
  printf("✓ All Phase 2.3 metadata tests passed!\n");
  printf("✓ Timestamps track epoch lifetime\n");
  printf("✓ Refcounts track live allocations\n");
  printf("✓ Metadata is epoch-isolated\n");
  printf("✓ Metadata resets on wraparound\n");
  printf("═══════════════════════════════════════════\n");
  
  return 0;
}
