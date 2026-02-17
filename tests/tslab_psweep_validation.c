/*
 * tslab_psweep_validation.c
 *
 * Phase 1.2: Reproduce the p-sweep through the profiler.
 *
 * Validates Theorem 3 from the drainability paper:
 *   R(t) >= p * m(t)
 *   Therefore: DSR = 1 - p (for mixed-routing workload)
 *
 * Test matrix:
 *   p=0.0  -> DSR=1.0 (perfect drainability)
 *   p=0.01 -> DSR=0.99
 *   p=0.05 -> DSR=0.95
 *   p=0.10 -> DSR=0.90
 *   p=0.25 -> DSR=0.75
 *   p=0.50 -> DSR=0.50
 *   p=1.0  -> DSR=0.0 (complete pinning)
 */

#include "../include/slab_alloc.h"
#include "../include/epoch_domain.h"
#include <drainprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

extern drainprof* g_profiler;

#define NUM_EPOCHS 100
#define ALLOCS_PER_EPOCH 50

typedef struct {
    void* ptr;
    SlabHandle handle;
    uint32_t epoch_id;
} Allocation;

/* Mixed-routing workload: allocate objects, some routed to wrong epoch */
void run_mixed_routing(SlabAllocator* alloc, double p) {
    Allocation allocs[NUM_EPOCHS * ALLOCS_PER_EPOCH];
    int alloc_count = 0;

    srand(42);  /* Deterministic seed */

    for (int epoch_idx = 0; epoch_idx < NUM_EPOCHS; epoch_idx++) {
        /* Advance to new epoch */
        epoch_advance(alloc);
        EpochId current = epoch_current(alloc);

        /* Allocate objects for this epoch */
        for (int i = 0; i < ALLOCS_PER_EPOCH; i++) {
            /* With probability p, route to wrong epoch (session leak) */
            double r = (double)rand() / RAND_MAX;
            EpochId target_epoch;

            if (r < p && epoch_idx > 0) {
                /* Route to previous epoch (simulates session object) */
                target_epoch = (current - 1 + 16) % 16;
            } else {
                /* Route correctly to current epoch */
                target_epoch = current;
            }

            SlabHandle handle;
            void* ptr = alloc_obj_epoch(alloc, 128, target_epoch, &handle);

            if (ptr) {
                allocs[alloc_count].ptr = ptr;
                allocs[alloc_count].handle = handle;
                allocs[alloc_count].epoch_id = target_epoch;
                alloc_count++;
            }
        }

        /* Close the oldest epoch (epoch that just aged out) */
        if (epoch_idx >= 8) {
            EpochId old_epoch = (current - 8 + 16) % 16;

            /* Free all allocations from that epoch */
            for (int i = 0; i < alloc_count; i++) {
                if (allocs[i].epoch_id == old_epoch && allocs[i].ptr) {
                    free_obj(alloc, allocs[i].handle);
                    allocs[i].ptr = NULL;
                }
            }

            /* Now close the epoch */
            epoch_close(alloc, old_epoch);
        }
    }

    /* Clean up remaining allocations */
    for (int i = 0; i < alloc_count; i++) {
        if (allocs[i].ptr) {
            free_obj(alloc, allocs[i].handle);
        }
    }
}

int main() {
    printf("=== Temporal-Slab P-Sweep Validation ===\n");
    printf("Testing Theorem 3: DSR = 1.0 - p\n\n");

    double p_values[] = {0.0, 0.01, 0.05, 0.10, 0.25, 0.50, 1.0};
    int num_tests = sizeof(p_values) / sizeof(p_values[0]);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < num_tests; i++) {
        double p = p_values[i];
        double expected_dsr = 1.0 - p;

        /* Create profiler */
        drainprof_config config = {
            .mode = DRAINPROF_PRODUCTION,
            .storage_backend = DRAINPROF_SLOT_ARRAY,
            .slot_array_capacity = 32,
            .verbose = false,
            .on_pinning = NULL,
            .callback_user_data = NULL,
            .max_buffered_reports = 0
        };

        g_profiler = drainprof_create_with_config(&config);
        if (!g_profiler) {
            fprintf(stderr, "Failed to create profiler\n");
            return 1;
        }

        /* Create allocator */
        SlabAllocator* alloc = slab_create();
        if (!alloc) {
            fprintf(stderr, "Failed to create allocator\n");
            drainprof_destroy(g_profiler);
            return 1;
        }

        /* Run workload */
        run_mixed_routing(alloc, p);

        /* Read profiler metrics */
        drainprof_snapshot snapshot;
        drainprof_snapshot(g_profiler, &snapshot);

        double actual_dsr = snapshot.dsr;
        double error = fabs(actual_dsr - expected_dsr);

        printf("p=%.2f: DSR=%.3f (expected %.3f, error %.3f) ",
               p, actual_dsr, expected_dsr, error);

        /* Allow 5% error margin for statistical variance */
        if (error < 0.05) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }

        printf("  Metrics: closes=%lu drainable=%lu pinned=%lu\n",
               snapshot.total_closes,
               snapshot.drainable_closes,
               snapshot.pinned_closes);

        /* Cleanup */
        slab_destroy(alloc);
        drainprof_destroy(g_profiler);
        g_profiler = NULL;

        printf("\n");
    }

    printf("=== Results ===\n");
    printf("Passed: %d/%d\n", passed, num_tests);
    printf("Failed: %d/%d\n", failed, num_tests);

    if (failed == 0) {
        printf("\nVALIDATION SUCCESS: Profiler correctly measures drainability\n");
        return 0;
    } else {
        printf("\nVALIDATION FAILURE: DSR measurements do not match theoretical predictions\n");
        return 1;
    }
}
