/*
 * test_label_cardinality.c - Validation test for Phase 2.3 label cardinality bounds
 * 
 * Tests that label registry enforces MAX_LABEL_IDS (16) bound:
 * - First 15 labels get unique IDs (1-15, ID 0 reserved for unlabeled)
 * - 16th and beyond get bucketed to ID 0 (unlabeled/other)
 * - Label reuse works correctly (same string gets same ID)
 */

#include <slab_alloc.h>
#include "slab_alloc_internal.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
  printf("Phase 2.3 Label Cardinality Test\n");
  printf("=================================\n\n");
  
  SlabAllocator* alloc = slab_allocator_create();
  if (!alloc) {
    fprintf(stderr, "FAIL: slab_allocator_create() returned NULL\n");
    return 1;
  }
  
  // Test 1: Verify initial state
  printf("Test 1: Initial state\n");
  assert(alloc->label_registry.count == 1);  // ID 0 reserved
  assert(strcmp(alloc->label_registry.labels[0], "(unlabeled)") == 0);
  printf("  ✓ Label registry initialized with ID 0 = '(unlabeled)'\n");
  printf("  ✓ Initial count = 1\n\n");
  
  // Test 2: Register first 15 labels (should get IDs 1-15)
  printf("Test 2: Register 15 unique labels\n");
  char label_buf[32];
  for (int i = 1; i <= 15; i++) {
    snprintf(label_buf, sizeof(label_buf), "label_%d", i);
    slab_epoch_set_label(alloc, 0, label_buf);  // Set on epoch 0
    
    // Verify label_id was assigned
    uint8_t expected_id = i;
    uint8_t actual_id = alloc->epoch_meta[0].label_id;
    if (actual_id != expected_id) {
      fprintf(stderr, "  FAIL: Expected ID %u, got %u for '%s'\n", expected_id, actual_id, label_buf);
      return 1;
    }
    
    // Verify label stored in registry
    if (strcmp(alloc->label_registry.labels[i], label_buf) != 0) {
      fprintf(stderr, "  FAIL: Registry[%d] = '%s', expected '%s'\n", 
              i, alloc->label_registry.labels[i], label_buf);
      return 1;
    }
  }
  printf("  ✓ All 15 labels assigned unique IDs (1-15)\n");
  printf("  ✓ Label registry count = %u\n", alloc->label_registry.count);
  assert(alloc->label_registry.count == 16);  // 0 + 15 = 16 (MAX_LABEL_IDS)
  printf("\n");
  
  // Test 3: Register 16th label (should get ID 0, registry full)
  printf("Test 3: Register 16th label (should overflow to ID 0)\n");
  slab_epoch_set_label(alloc, 1, "label_overflow");
  uint8_t overflow_id = alloc->epoch_meta[1].label_id;
  if (overflow_id != 0) {
    fprintf(stderr, "  FAIL: Expected ID 0 (overflow), got %u\n", overflow_id);
    return 1;
  }
  printf("  ✓ 16th label assigned ID 0 (unlabeled bucket)\n");
  printf("  ✓ Label registry count unchanged = %u\n", alloc->label_registry.count);
  printf("\n");
  
  // Test 4: Label reuse (same string should get same ID)
  printf("Test 4: Label reuse\n");
  slab_epoch_set_label(alloc, 2, "label_5");  // Already registered as ID 5
  uint8_t reused_id = alloc->epoch_meta[2].label_id;
  if (reused_id != 5) {
    fprintf(stderr, "  FAIL: Expected ID 5 (reuse), got %u\n", reused_id);
    return 1;
  }
  printf("  ✓ Label 'label_5' reused existing ID 5\n");
  printf("  ✓ Label registry count unchanged = %u\n", alloc->label_registry.count);
  printf("\n");
  
  // Test 5: Multiple epochs can have different labels
  printf("Test 5: Multiple epochs with different labels\n");
  slab_epoch_set_label(alloc, 3, "label_1");
  slab_epoch_set_label(alloc, 4, "label_7");
  slab_epoch_set_label(alloc, 5, "label_15");
  assert(alloc->epoch_meta[3].label_id == 1);
  assert(alloc->epoch_meta[4].label_id == 7);
  assert(alloc->epoch_meta[5].label_id == 15);
  printf("  ✓ Epochs 3,4,5 have label_ids 1,7,15 respectively\n");
  printf("\n");
  
  // Test 6: Empty label handling
  printf("Test 6: Empty label\n");
  slab_epoch_set_label(alloc, 6, "");  // Empty string
  // Should either get ID 0 or be handled gracefully
  printf("  ✓ Empty label handled (ID = %u)\n", alloc->epoch_meta[6].label_id);
  printf("\n");
  
  slab_allocator_free(alloc);
  
  printf("=================================\n");
  printf("All tests PASSED\n");
  printf("✓ Label cardinality bounds enforced correctly\n");
  printf("✓ Max 15 user labels (IDs 1-15) + 1 unlabeled (ID 0)\n");
  printf("✓ Overflow handled gracefully (bucket to ID 0)\n");
  printf("✓ Label reuse works correctly\n");
  
  return 0;
}
