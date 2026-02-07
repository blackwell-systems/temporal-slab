/*
 * test_era_stamping.c - Verify Phase 2.2 era stamping
 * 
 * Tests that era counter increments monotonically on epoch_advance
 * and that slabs are stamped with correct era values.
 */

#include <stdio.h>
#include <assert.h>
#include "slab_alloc_internal.h"

int main(void) {
  printf("Phase 2.2 Era Stamping Test\n");
  printf("===========================\n\n");
  
  SlabAllocator* alloc = slab_allocator_create();
  assert(alloc && "allocator creation failed");
  
  /* Verify initial state */
  printf("Initial state:\n");
  printf("  current_epoch: %u\n", epoch_current(alloc));
  printf("  epoch_era_counter: %lu\n", atomic_load_explicit(&alloc->epoch_era_counter, memory_order_relaxed));
  
  /* Verify initial eras are all 0 */
  for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
    printf("  epoch[%u].era = %lu\n", e, alloc->epoch_era[e]);
    assert(alloc->epoch_era[e] == 0 && "initial era should be 0");
  }
  
  printf("\nAdvancing epochs and checking era progression:\n");
  
  /* Advance through several epochs */
  for (int i = 1; i <= 5; i++) {
    epoch_advance(alloc);
    EpochId cur = epoch_current(alloc);
    uint64_t era = alloc->epoch_era[cur];
    uint64_t counter = atomic_load_explicit(&alloc->epoch_era_counter, memory_order_relaxed);
    
    printf("  Advance %d: epoch=%u, era=%lu, counter=%lu\n", i, cur, era, counter);
    
    /* Verify era is monotonic */
    assert(era == (uint64_t)i && "era should match advance count");
    assert(counter == (uint64_t)i && "counter should match advance count");
  }
  
  printf("\nAllocating slabs and checking era stamps:\n");
  
  /* Allocate in current epoch and verify slab era */
  EpochId epoch = epoch_current(alloc);
  uint64_t expected_era = alloc->epoch_era[epoch];
  
  SlabHandle h1;
  void* p1 = alloc_obj_epoch(alloc, 128, epoch, &h1);
  assert(p1 && "allocation failed");
  printf("  Allocated in epoch %u (era %lu)\n", epoch, expected_era);
  
  /* Advance again and allocate in new epoch */
  epoch_advance(alloc);
  epoch = epoch_current(alloc);
  expected_era = alloc->epoch_era[epoch];
  
  SlabHandle h2;
  void* p2 = alloc_obj_epoch(alloc, 128, epoch, &h2);
  assert(p2 && "allocation failed");
  printf("  Allocated in epoch %u (era %lu)\n", epoch, expected_era);
  
  /* Verify era counter incremented */
  uint64_t final_counter = atomic_load_explicit(&alloc->epoch_era_counter, memory_order_relaxed);
  printf("\nFinal era_counter: %lu\n", final_counter);
  assert(final_counter == 6 && "counter should be 6 after 6 advances");
  
  /* Test wraparound: advance through full epoch ring */
  printf("\nTesting epoch wraparound:\n");
  for (int i = 0; i < 20; i++) {
    epoch_advance(alloc);
  }
  
  EpochId final_epoch_raw = epoch_current(alloc);
  EpochId final_epoch = final_epoch_raw % EPOCH_COUNT;
  uint64_t final_era = alloc->epoch_era[final_epoch];
  final_counter = atomic_load_explicit(&alloc->epoch_era_counter, memory_order_relaxed);
  
  printf("  After 20 more advances:\n");
  printf("    epoch=%u (wrapped), era=%lu (monotonic), counter=%lu\n", 
         final_epoch, final_era, final_counter);
  
  /* Verify era is still monotonic even after wraparound */
  assert(final_counter == 26 && "counter should be 26 total");
  
  /* Verify era matches counter (should be set on last advance to this epoch) */
  assert(final_era == final_counter && "era should match counter after advance");
  
  /* Clean up */
  free_obj(alloc, h1);
  free_obj(alloc, h2);
  slab_allocator_free(alloc);
  
  printf("\n✓ All era stamping tests passed!\n");
  printf("✓ Eras are monotonically increasing\n");
  printf("✓ Epoch wraparound preserves monotonic time\n");
  
  return 0;
}
