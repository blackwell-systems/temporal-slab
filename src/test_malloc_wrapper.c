#include <slab_alloc.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
  printf("=== Testing malloc/free wrapper ===\n\n");
  
  SlabAllocator* a = slab_allocator_create();
  assert(a && "allocator creation failed");
  
  printf("Test 1: Basic malloc/free...\n");
  void* p1 = slab_malloc_epoch(a, 64, 0);
  assert(p1 && "malloc failed");
  memset(p1, 0xAA, 64);
  slab_free(a, p1);
  printf("  PASS: malloc(64) + free\n");
  
  printf("\nTest 2: Multiple allocations...\n");
  void* ptrs[100];
  for (int i = 0; i < 100; i++) {
    ptrs[i] = slab_malloc_epoch(a, 128, 0);
    assert(ptrs[i] && "malloc failed in loop");
    *(int*)ptrs[i] = i;
  }
  for (int i = 0; i < 100; i++) {
    assert(*(int*)ptrs[i] == i && "data corruption");
    slab_free(a, ptrs[i]);
  }
  printf("  PASS: 100 allocs + data integrity + free\n");
  
  printf("\nTest 3: NULL and boundary cases...\n");
  slab_free(a, NULL);  /* Should not crash */
  void* p_zero = slab_malloc_epoch(a, 0, 0);
  assert(p_zero == NULL && "malloc(0) should return NULL");
  void* p_huge = slab_malloc_epoch(a, 505, 0);
  assert(p_huge == NULL && "malloc(505) should return NULL (max is 504)");
  void* p_max = slab_malloc_epoch(a, 504, 0);
  assert(p_max && "malloc(504) should succeed");
  slab_free(a, p_max);
  printf("  PASS: NULL free, malloc(0), oversized, max size\n");
  
  printf("\nTest 4: Mixed malloc and handle API...\n");
  void* pm = slab_malloc_epoch(a, 100, 0);
  SlabHandle h;
  void* ph = alloc_obj_epoch(a, 100, 0, &h);
  assert(pm && ph && "both APIs work");
  slab_free(a, pm);
  free_obj(a, h);
  printf("  PASS: malloc and handle APIs coexist\n");
  
  slab_allocator_free(a);
  
  printf("\n=== All malloc wrapper tests PASS ===\n");
  return 0;
}
