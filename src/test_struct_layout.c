#include <stdio.h>
#include <stddef.h>
#include "slab_alloc_internal.h"

int main() {
    printf("sizeof(Slab) = %zu\n", sizeof(Slab));
    printf("offsetof(Slab, magic) = %zu\n", offsetof(Slab, magic));
    printf("offsetof(Slab, version) = %zu\n", offsetof(Slab, version));
    printf("offsetof(Slab, object_size) = %zu\n", offsetof(Slab, object_size));
    printf("offsetof(Slab, object_count) = %zu\n", offsetof(Slab, object_count));
    printf("offsetof(Slab, free_count) = %zu\n", offsetof(Slab, free_count));
    printf("offsetof(Slab, list_id) = %zu\n", offsetof(Slab, list_id));
    printf("\n");
    printf("sizeof(SlabHandle) = %zu\n", sizeof(SlabHandle));
    printf("offsetof(SlabHandle, slab) = %zu\n", offsetof(SlabHandle, slab));
    printf("offsetof(SlabHandle, slot) = %zu\n", offsetof(SlabHandle, slot));
    printf("offsetof(SlabHandle, size_class) = %zu\n", offsetof(SlabHandle, size_class));
    printf("offsetof(SlabHandle, slab_version) = %zu\n", offsetof(SlabHandle, slab_version));
    return 0;
}
