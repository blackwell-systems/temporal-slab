/*
 * slab_slowpath_sampling.c - Tail latency instrumentation for WSL2/VM diagnosis
 *
 * Distinguishes real allocator slowpath work from scheduling artifacts by
 * comparing wall-clock time vs thread CPU time for operations above threshold.
 *
 * Key insight: If wall_time >> cpu_time, the thread was preempted (host/VM noise).
 *              If wall_time ≈ cpu_time, it's real allocator work.
 */

#include "slab_alloc_internal.h"

#if ENABLE_SLOWPATH_SAMPLING

#include <time.h>
#include <stdio.h>

/* Get current time in nanoseconds */
static inline uint64_t now_ns(clockid_t clock_id) {
    struct timespec ts;
    clock_gettime(clock_id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Record a slowpath sample (lock-free, overwriting ring buffer) */
void slowpath_record_sample(SlabAllocator* a, uint64_t wall_ns, uint64_t cpu_ns,
                            uint32_t size_class, uint32_t reason_flags, uint32_t retries) {
    /* Atomic fetch-and-increment to claim a slot */
    uint32_t idx = atomic_fetch_add_explicit(&a->slowpath_sampler.write_idx, 1, 
                                             memory_order_relaxed);
    idx %= SLOWPATH_MAX_SAMPLES;
    
    /* Write sample (may overwrite old data - that's okay) */
    SlowpathSample* s = &a->slowpath_sampler.samples[idx];
    s->wall_ns = wall_ns;
    s->cpu_ns = cpu_ns;
    s->size_class = size_class;
    s->reason_flags = reason_flags;
    s->retries = retries;
}

/* Begin timing a potentially slow operation */
void slowpath_timing_start(uint64_t* out_wall_start, uint64_t* out_cpu_start) {
    *out_wall_start = now_ns(CLOCK_MONOTONIC);
    *out_cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
}

/* End timing and record sample if above threshold */
void slowpath_timing_end(SlabAllocator* a, uint64_t wall_start, uint64_t cpu_start,
                        uint32_t size_class, uint32_t reason_flags, uint32_t retries) {
    uint64_t wall_end = now_ns(CLOCK_MONOTONIC);
    uint64_t cpu_end = now_ns(CLOCK_THREAD_CPUTIME_ID);
    
    uint64_t wall_elapsed = wall_end - wall_start;
    
    if (wall_elapsed >= SLOWPATH_THRESHOLD_NS) {
        uint64_t cpu_elapsed = cpu_end - cpu_start;
        slowpath_record_sample(a, wall_elapsed, cpu_elapsed, size_class, 
                              reason_flags, retries);
    }
}

/* Print slowpath samples with analysis */
void slowpath_print_samples(SlabAllocator* a) {
    uint32_t total_samples = atomic_load_explicit(&a->slowpath_sampler.write_idx, 
                                                  memory_order_relaxed);
    
    if (total_samples == 0) {
        printf("\n=== Slowpath Samples ===\n");
        printf("No samples recorded (no allocations exceeded %uns threshold)\n", 
               SLOWPATH_THRESHOLD_NS);
        printf("========================\n\n");
        return;
    }
    
    /* Analyze samples */
    uint32_t count_preempted = 0;  /* wall >> cpu */
    uint32_t count_real_work = 0;  /* wall ≈ cpu */
    uint64_t sum_wall = 0, sum_cpu = 0;
    uint64_t max_wall = 0, max_cpu = 0;
    
    uint32_t samples_to_analyze = total_samples < SLOWPATH_MAX_SAMPLES 
        ? total_samples : SLOWPATH_MAX_SAMPLES;
    
    for (uint32_t i = 0; i < samples_to_analyze; i++) {
        SlowpathSample* s = &a->slowpath_sampler.samples[i];
        sum_wall += s->wall_ns;
        sum_cpu += s->cpu_ns;
        if (s->wall_ns > max_wall) max_wall = s->wall_ns;
        if (s->cpu_ns > max_cpu) max_cpu = s->cpu_ns;
        
        /* Heuristic: preempted if wall > 3× cpu */
        if (s->wall_ns > 3 * s->cpu_ns) {
            count_preempted++;
        } else {
            count_real_work++;
        }
    }
    
    printf("\n=== Slowpath Samples ===\n");
    printf("Total samples: %u (threshold: %uns)\n", samples_to_analyze, SLOWPATH_THRESHOLD_NS);
    printf("\nClassification:\n");
    printf("  Preempted (wall>3×cpu): %u (%.1f%%) - WSL2/VM scheduling noise\n",
           count_preempted, 100.0 * count_preempted / samples_to_analyze);
    printf("  Real work (wall≈cpu):    %u (%.1f%%) - Actual allocator slowpath\n",
           count_real_work, 100.0 * count_real_work / samples_to_analyze);
    printf("\nLatency statistics:\n");
    printf("  Wall-clock: avg=%luus max=%luus\n", 
           sum_wall / samples_to_analyze / 1000, max_wall / 1000);
    printf("  Thread CPU: avg=%luus max=%luus\n",
           sum_cpu / samples_to_analyze / 1000, max_cpu / 1000);
    
    /* Reason breakdown */
    uint32_t reason_counts[5] = {0};
    for (uint32_t i = 0; i < samples_to_analyze; i++) {
        SlowpathSample* s = &a->slowpath_sampler.samples[i];
        if (s->reason_flags & SLOWPATH_LOCK_WAIT) reason_counts[0]++;
        if (s->reason_flags & SLOWPATH_NEW_SLAB) reason_counts[1]++;
        if (s->reason_flags & SLOWPATH_ZOMBIE_REPAIR) reason_counts[2]++;
        if (s->reason_flags & SLOWPATH_CACHE_OVERFLOW) reason_counts[3]++;
        if (s->reason_flags & SLOWPATH_CAS_RETRY) reason_counts[4]++;
    }
    
    printf("\nReason breakdown:\n");
    printf("  Lock wait:      %u\n", reason_counts[0]);
    printf("  New slab:       %u\n", reason_counts[1]);
    printf("  Zombie repair:  %u\n", reason_counts[2]);
    printf("  Cache overflow: %u\n", reason_counts[3]);
    printf("  CAS retry:      %u\n", reason_counts[4]);
    printf("========================\n\n");
}

#endif /* ENABLE_SLOWPATH_SAMPLING */
