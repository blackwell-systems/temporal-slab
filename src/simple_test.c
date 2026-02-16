#include <stdio.h>
#include "slab_alloc_internal.h"
#include "slab_stats.h"

int main() {
    SlabAllocator* alloc = slab_allocator_create();
    if (!alloc) {
        printf("Failed to create allocator\n");
        return 1;
    }
    
    // Do lots of allocations to trigger sampling
    printf("Running 100K allocations to trigger probabilistic sampling (1/1024)...\n");
    for (int i = 0; i < 100000; i++) {
        SlabHandle handle;
        void* ptr = alloc_obj_epoch(alloc, 128, 0, &handle);
        if (!ptr) {
            printf("Allocation %d FAILED\n", i);
            return 1;
        }
        free_obj(alloc, handle);
    }
    
    printf("All allocations succeeded\n\n");
    
#if ENABLE_SLOWPATH_SAMPLING
    ThreadStats stats = slab_stats_thread();
    printf("=== Slowpath Sampling Results ===\n");
    printf("Expected samples: ~%d (100K / 1024)\n", 100000 / 1024);
    printf("Actual samples:   %lu\n", stats.alloc_samples);
    
    if (stats.alloc_samples > 0) {
        uint64_t avg_wall = stats.alloc_wall_ns_sum / stats.alloc_samples;
        uint64_t avg_cpu = stats.alloc_cpu_ns_sum / stats.alloc_samples;
        printf("\nAllocation timing:\n");
        printf("  Avg wall: %lu ns  (max: %lu ns)\n", avg_wall, stats.alloc_wall_ns_max);
        printf("  Avg CPU:  %lu ns  (max: %lu ns)\n", avg_cpu, stats.alloc_cpu_ns_max);
        printf("  Ratio:    %.2fx\n", (double)avg_wall / avg_cpu);
        
        if (avg_wall > avg_cpu * 2) {
            printf("  ⚠ wall >> cpu: Scheduler interference detected\n");
        } else {
            printf("  ✓ Clean measurement\n");
        }
    }
    
    if (stats.repair_count > 0) {
        printf("\n⚠ Zombie repairs: %lu\n", stats.repair_count);
    } else {
        printf("\n✓ No zombie repairs\n");
    }
#else
    printf("(ENABLE_SLOWPATH_SAMPLING not defined - no sampling data)\n");
#endif
    
    slab_allocator_free(alloc);
    printf("\nTest passed\n");
    return 0;
}
