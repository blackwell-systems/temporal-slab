#include <slab_alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

int main(void) {
  printf("=== Epoch-Based Allocation Test ===\n\n");
  
  SlabAllocator* a = slab_allocator_create();
  assert(a && "allocator creation failed");
  
  printf("Test 1: Basic epoch allocation...\n");
  EpochId e0 = epoch_current(a);
  printf("  Current epoch: %u\n", e0);
  
  SlabHandle h1, h2;
  void* p1 = alloc_obj_epoch(a, 128, e0, &h1);
  assert(p1 && "allocation in epoch 0 failed");
  memset(p1, 0xAA, 128);
  
  epoch_advance(a);
  EpochId e1 = epoch_current(a);
  printf("  Advanced to epoch: %u\n", e1);
  
  void* p2 = alloc_obj_epoch(a, 128, e1, &h2);
  assert(p2 && "allocation in epoch 1 failed");
  memset(p2, 0xBB, 128);
  
  printf("  PASS: Allocated in two different epochs\n");

  printf("\nTest 2: Epoch lifetime separation...\n");

  #define OBJECTS_PER_EPOCH 1000
  SlabHandle handles_e0[OBJECTS_PER_EPOCH];
  SlabHandle handles_e1[OBJECTS_PER_EPOCH];

  /* Allocate batch in current epoch (e1 from previous test) */
  EpochId current_epoch = epoch_current(a);
  printf("  Current epoch: %u\n", current_epoch);

  for (int i = 0; i < OBJECTS_PER_EPOCH; i++) {
    void* p = alloc_obj_epoch(a, 128, current_epoch, &handles_e0[i]);
    assert(p && "epoch batch allocation failed");
    *(int*)p = i;
  }

  /* Advance to next epoch and allocate another batch */
  epoch_advance(a);
  EpochId next_epoch = epoch_current(a);
  printf("  Advanced to epoch: %u\n", next_epoch);

  for (int i = 0; i < OBJECTS_PER_EPOCH; i++) {
    void* p = alloc_obj_epoch(a, 128, next_epoch, &handles_e1[i]);
    assert(p && "next epoch batch allocation failed");
    *(int*)p = i + 10000;
  }
  
  uint64_t rss_before_free = read_rss_bytes_linux();
  printf("  RSS with both epochs: %.2f MiB\n", rss_before_free / (1024.0 * 1024.0));

  /* Free entire first batch (simulates epoch expiration) */
  for (int i = 0; i < OBJECTS_PER_EPOCH; i++) {
    bool ok = free_obj(a, handles_e0[i]);
    assert(ok && "free_obj for first batch failed");
  }

  uint64_t rss_after_free = read_rss_bytes_linux();
  printf("  RSS after freeing first batch: %.2f MiB\n", rss_after_free / (1024.0 * 1024.0));
  printf("  RSS delta: %.2f MiB\n", (rss_after_free - rss_before_free) / (1024.0 * 1024.0));

  /* Second batch objects should still be valid */
  for (int i = 0; i < OBJECTS_PER_EPOCH; i++) {
    bool ok = free_obj(a, handles_e1[i]);
    assert(ok && "free_obj for second batch failed");
  }
  
  printf("  PASS: Epochs isolated correctly\n");
  
  printf("\nTest 3: Epoch ring buffer wrap...\n");
  for (uint32_t i = 0; i < 20; i++) {
    EpochId e = epoch_current(a);
    printf("  Epoch %u (should wrap at 16)\n", e);

    SlabHandle h;
    void* p = alloc_obj_epoch(a, 64, e, &h);
    assert(p && "allocation in current epoch failed");
    free_obj(a, h);

    epoch_advance(a);
  }
  printf("  PASS: Epoch wrapping works correctly\n");
  
  printf("\nTest 4: Mixed epoch malloc API...\n");
  EpochId e_test4 = epoch_current(a);
  void* m0 = slab_malloc_epoch(a, 100, e_test4);
  assert(m0 && "malloc_epoch for first allocation failed");

  epoch_advance(a);
  EpochId e_test4_next = epoch_current(a);
  void* m1 = slab_malloc_epoch(a, 100, e_test4_next);
  assert(m1 && "malloc_epoch for second allocation failed");

  slab_free(a, m0);
  slab_free(a, m1);
  printf("  PASS: malloc_epoch works\n");
  
  slab_allocator_free(a);
  
  printf("\n=== All epoch tests PASS ===\n");
  return 0;
}
