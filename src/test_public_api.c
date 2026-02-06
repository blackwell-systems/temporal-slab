/*
 * test_public_api.c - Verify public API works for external users
 * 
 * This file ONLY includes the public header and uses the opaque API.
 * It should compile and run without accessing internal structs.
 */

#include <slab_alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  printf("Testing public opaque API...\n\n");
  
  /* Use opaque API */
  SlabAllocator* alloc = slab_allocator_create();
  if (!alloc) {
    fprintf(stderr, "Failed to create allocator\n");
    return 1;
  }
  
  /* Allocate some objects */
  const int N = 1000;
  SlabHandle handles[N];
  void* ptrs[N];
  
  for (int i = 0; i < N; i++) {
    ptrs[i] = alloc_obj(alloc, 128, &handles[i]);
    if (!ptrs[i]) {
      fprintf(stderr, "Allocation failed at %d\n", i);
      return 1;
    }
    memset(ptrs[i], (unsigned char)i, 128);
  }
  
  printf("Allocated %d objects successfully\n", N);
  
  /* Free half */
  for (int i = 0; i < N / 2; i++) {
    if (!free_obj(alloc, handles[i])) {
      fprintf(stderr, "Free failed at %d\n", i);
      return 1;
    }
  }
  
  printf("Freed %d objects successfully\n", N / 2);
  
  /* Get performance counters */
  PerfCounters counters;
  get_perf_counters(alloc, 1, &counters);  /* 128B = size class 1 */
  printf("\nPerformance counters (128B class):\n");
  printf("  Slow path hits: %lu\n", counters.slow_path_hits);
  printf("  New slabs:      %lu\n", counters.new_slab_count);
  
  /* Clean up */
  slab_allocator_free(alloc);
  
  printf("\n✓ Public API test complete!\n");
  return 0;
}
