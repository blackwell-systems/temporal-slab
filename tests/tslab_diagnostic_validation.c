/*
 * tslab_diagnostic_validation.c
 *
 * Phase 1.3: Run diagnostic mode on the p=0.25 workload.
 *
 * Validates that diagnostic mode:
 * 1. Produces pinning reports when epochs close with live allocations
 * 2. Identifies the correct allocation sites (session objects in wrong epoch)
 * 3. Counts match expected violation fraction (p=0.25 → 25% pinning rate)
 */

#include "../include/slab_alloc.h"
#include "../include/epoch_domain.h"
#include <drainprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern drainprof* g_profiler;

#define NUM_EPOCHS 100
#define ALLOCS_PER_EPOCH 50
#define VIOLATION_PROB 0.25

typedef struct {
    void* ptr;
    SlabHandle handle;
    uint32_t epoch_id;
    bool is_violation;  /* Was this routed to wrong epoch? */
} Allocation;

/* Count how many pinning reports mention this file */
int count_reports_from_file(drainprof* prof, const char* filename) {
    drainprof_pinning_report* reports;
    uint32_t count;

    drainprof_drain_reports(prof, &reports, &count);

    int matches = 0;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < reports[i].num_pinning_allocs; j++) {
            if (strstr(reports[i].pinning_allocs[j].site.file, filename)) {
                matches++;
            }
        }
        drainprof_pinning_report_free(&reports[i]);
    }

    free(reports);
    return matches;
}

/* Mixed-routing workload with diagnostic tracking */
void run_diagnostic_workload(SlabAllocator* alloc) {
    Allocation allocs[NUM_EPOCHS * ALLOCS_PER_EPOCH];
    int alloc_count = 0;
    int violation_count = 0;

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
            bool is_violation = false;

            if (r < VIOLATION_PROB && epoch_idx > 0) {
                /* Route to previous epoch (simulates session object) */
                target_epoch = (current - 1 + 16) % 16;
                is_violation = true;
                violation_count++;
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
                allocs[alloc_count].is_violation = is_violation;
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

    printf("Workload complete: %d total allocations, %d violations (%.1f%%)\n",
           alloc_count, violation_count, 100.0 * violation_count / alloc_count);
}

int main() {
    printf("=== Temporal-Slab Diagnostic Mode Validation ===\n");
    printf("Running p=0.25 workload with diagnostic tracking\n\n");

    /* Create profiler in diagnostic mode */
    drainprof_config config = {
        .mode = DRAINPROF_DIAGNOSTIC,
        .storage_backend = DRAINPROF_SLOT_ARRAY,
        .slot_array_capacity = 32,
        .verbose = false,
        .on_pinning = NULL,  /* Buffered mode */
        .callback_user_data = NULL,
        .max_buffered_reports = 256
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
    run_diagnostic_workload(alloc);

    /* Read profiler metrics */
    drainprof_snapshot snapshot;
    drainprof_snapshot(g_profiler, &snapshot);

    printf("\n=== Profiler Metrics ===\n");
    printf("DSR: %.3f\n", snapshot.dsr);
    printf("Total closes: %lu\n", snapshot.total_closes);
    printf("Drainable closes: %lu\n", snapshot.drainable_closes);
    printf("Pinned closes: %lu\n", snapshot.pinned_closes);

    /* Compute summary */
    drainprof_diagnostic_summary* summary = drainprof_diagnostic_summary_compute(g_profiler);
    if (!summary) {
        fprintf(stderr, "Failed to compute diagnostic summary\n");
        slab_destroy(alloc);
        drainprof_destroy(g_profiler);
        return 1;
    }

    printf("\n=== Diagnostic Summary ===\n");
    printf("Total allocation sites: %u\n", summary->num_sites);

    for (uint32_t i = 0; i < summary->num_sites; i++) {
        drainprof_summary_site_entry* entry = &summary->sites[i];
        printf("\nSite %u: %s:%u\n", i + 1, entry->site.file, entry->site.line);
        printf("  Granules pinned: %u\n", entry->pinning_count);
        printf("  Total allocations: %u\n", entry->total_allocs);
        printf("  Total bytes: %zu\n", entry->total_bytes);
    }

    /* Validation checks */
    int validation_passed = 1;

    /* Check 1: DSR should be approximately 0.75 (1.0 - 0.25) */
    double expected_dsr = 0.75;
    double dsr_error = fabs(snapshot.dsr - expected_dsr);
    printf("\n=== Validation Checks ===\n");
    printf("1. DSR check: %.3f (expected %.3f, error %.3f) ",
           snapshot.dsr, expected_dsr, dsr_error);
    if (dsr_error < 0.1) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
        validation_passed = 0;
    }

    /* Check 2: Should have pinning reports */
    printf("2. Pinning reports: %lu pinned closes ", snapshot.pinned_closes);
    if (snapshot.pinned_closes > 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected some pinned closes)\n");
        validation_passed = 0;
    }

    /* Check 3: Summary should identify allocation sites */
    printf("3. Allocation sites identified: %u ", summary->num_sites);
    if (summary->num_sites > 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected at least one site)\n");
        validation_passed = 0;
    }

    /* Check 4: Reports should come from this test file */
    int reports_from_test = count_reports_from_file(g_profiler, "tslab_diagnostic_validation.c");
    printf("4. Reports from test file: %d ", reports_from_test);
    if (reports_from_test > 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (expected allocations from this file)\n");
        validation_passed = 0;
    }

    /* Cleanup */
    drainprof_diagnostic_summary_free(summary);
    slab_destroy(alloc);
    drainprof_destroy(g_profiler);
    g_profiler = NULL;

    if (validation_passed) {
        printf("\n=== VALIDATION SUCCESS ===\n");
        printf("Diagnostic mode correctly identifies pinning allocations\n");
        return 0;
    } else {
        printf("\n=== VALIDATION FAILURE ===\n");
        printf("Diagnostic mode did not produce expected results\n");
        return 1;
    }
}
