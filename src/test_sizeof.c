#include "slab_alloc_internal.h"
#include <stdio.h>
int main() {
  printf("sizeof(SlabAllocator) = %zu\n", sizeof(SlabAllocator));
  printf("sizeof(SizeClassAlloc) = %zu\n", sizeof(SizeClassAlloc));
  return 0;
}
