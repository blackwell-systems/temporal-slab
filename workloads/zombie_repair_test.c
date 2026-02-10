/*
 * zombie_repair_test.c - Adversarial workload to trigger zombie partial repairs
 * 
 * Purpose: Force zombie partial slab conditions through racing alloc/free patterns
 * 
 * Zombie partial condition:
 * - Slab on PARTIAL list with free_count diverged from bitmap state
 * - Typically: free_count==0 but bitmap shows slab is actually full
 * - Caused by publication races in lock-free fast path
 * 
 * Attack strategy:
 * - Multiple threads race to exhaust same slab
 * - Some threads see partial slab, allocate last slots
 * - free_count update races cause divergence
 * - Slow path scans partial list, finds "zombie" with free_count but full bitmap
 * 
 * Validation:
 * - Measure repair timing (wall, CPU, wait)
 * - Track repair reasons (full_bitmap, list_mismatch)
 * - Compare repair cost to normal allocation cost
 * 
 * Environment: WSL2 Ubuntu 22.04, AMD Ryzen 7950X, Feb 10 2026
 * Sampling: 1/1024 probabilistic (ENABLE_SLOWPATH_SAMPLING)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif
#include "slab_alloc_internal.h"
#include "slab_stats.h"

#define DEFAULT_THREADS 16
#define ALLOCS_PER_THREAD 50000
#define ALLOC_SIZE 128
#define EPOCH_ID 0

/* Shared state to coordinate racing allocations */
typedef struct {
    SlabAllocator* alloc;
    int thread_id;
    _Atomic uint64_t* total_allocs;
    _Atomic uint64_t* total_repairs;  /* Aggregate repair count across threads */
    pthread_barrier_t* start_barrier;  /* Synchronize thread start for maximum contention */
} ThreadArgs;

void* worker_thread(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    SlabAllocator* alloc = args->alloc;
    int tid = args->thread_id;
    
    /* Pin thread to specific core */
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(tid % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    
    /* Wait for all threads to be ready (maximize initial contention burst) */
    pthread_barrier_wait(args->start_barrier);
    
    /* Adversarial pattern: rapid alloc/free with minimal delay
     * Goal: Create races where multiple threads see same slab as partial,
     * allocate last slots concurrently, cause free_count divergence */
    SlabHandle handles[10];
    int handle_count = 0;
    
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        SlabHandle h;
        void* ptr = alloc_obj_epoch(alloc, ALLOC_SIZE, EPOCH_ID, &h);
        if (!ptr) {
            /* Allocation failed - should not happen */
            printf("Thread %d: Allocation %d FAILED\n", tid, i);
            break;
        }
        
        /* Accumulate a few handles before freeing (creates partial fills) */
        handles[handle_count++] = h;
        
        if (handle_count >= 10 || i % 100 == 99) {
            /* Batch free: creates many partial->full and full->partial transitions */
            for (int j = 0; j < handle_count; j++) {
                free_obj(alloc, handles[j]);
            }
            handle_count = 0;
        }
        
        atomic_fetch_add_explicit(args->total_allocs, 1, memory_order_relaxed);
    }
    
    /* Free any remaining handles */
    for (int j = 0; j < handle_count; j++) {
        free_obj(alloc, handles[j]);
    }
    
#ifdef ENABLE_SLOWPATH_SAMPLING
    ThreadStats stats = slab_stats_thread();
    
    if (stats.repair_count > 0) {
        /* This thread encountered repairs - report details */
        atomic_fetch_add_explicit(args->total_repairs, stats.repair_count, memory_order_relaxed);
        
        uint64_t avg_repair_wall = stats.repair_wall_ns_sum / stats.repair_count;
        uint64_t avg_repair_cpu = stats.repair_cpu_ns_sum / stats.repair_count;
        uint64_t avg_repair_wait = stats.repair_wait_ns_sum / stats.repair_count;
        
        printf("\n[Thread %d] REPAIRS DETECTED: %lu\n", tid, stats.repair_count);
        printf("  Avg: wall=%lu ns, cpu=%lu ns, wait=%lu ns (%.2fx)\n",
               avg_repair_wall, avg_repair_cpu, avg_repair_wait,
               (double)avg_repair_wall / avg_repair_cpu);
        printf("  Max: wall=%lu ns, cpu=%lu ns, wait=%lu ns\n",
               stats.repair_wall_ns_max, stats.repair_cpu_ns_max, stats.repair_wait_ns_max);
        printf("  Reasons: full_bitmap=%lu, list_mismatch=%lu, other=%lu\n",
               stats.repair_reason_full_bitmap, stats.repair_reason_list_mismatch,
               stats.repair_reason_other);
    }
    
    if (stats.alloc_samples > 0) {
        uint64_t avg_wall = stats.alloc_wall_ns_sum / stats.alloc_samples;
        uint64_t avg_cpu = stats.alloc_cpu_ns_sum / stats.alloc_samples;
        uint64_t avg_wait = stats.alloc_wait_ns_sum / stats.alloc_samples;
        
        printf("[Thread %d] Samples: %lu (repairs: %lu)\n", tid, stats.alloc_samples, stats.repair_count);
        printf("  Avg: wall=%lu ns, cpu=%lu ns, wait=%lu ns (%.2fx)\n",
               avg_wall, avg_cpu, avg_wait, (double)avg_wall / avg_cpu);
    }
#endif
    
    return NULL;
}

int main(int argc, char** argv) {
    int num_threads = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREADS;
    if (num_threads < 1 || num_threads > 128) {
        printf("Usage: %s [threads] (1-128, default %d)\n", argv[0], DEFAULT_THREADS);
        return 1;
    }
    
    printf("=== Zombie Partial Repair Test ===\n");
    printf("Date: Feb 10 2026\n");
    printf("Environment: WSL2 Ubuntu 22.04, AMD Ryzen 7950X\n");
    printf("Threads: %d (pinned to cores)\n", num_threads);
    printf("Allocations per thread: %d\n", ALLOCS_PER_THREAD);
    printf("Total allocations: %d\n", num_threads * ALLOCS_PER_THREAD);
    printf("Size class: %d bytes\n", ALLOC_SIZE);
    printf("Pattern: Adversarial (rapid alloc/batch-free to force races)\n\n");
    
    SlabAllocator* alloc = slab_allocator_create();
    if (!alloc) {
        printf("Failed to create allocator\n");
        return 1;
    }
    
    _Atomic uint64_t total_allocs = 0;
    _Atomic uint64_t total_repairs = 0;
    pthread_barrier_t start_barrier;
    pthread_barrier_init(&start_barrier, NULL, num_threads);
    
    pthread_t threads[128];
    ThreadArgs thread_args[128];
    
    printf("Launching threads...\n");
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].alloc = alloc;
        thread_args[i].thread_id = i;
        thread_args[i].total_allocs = &total_allocs;
        thread_args[i].total_repairs = &total_repairs;
        thread_args[i].start_barrier = &start_barrier;
        
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_args[i]) != 0) {
            printf("Failed to create thread %d\n", i);
            return 1;
        }
    }
    
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("\n=== Summary ===\n");
    printf("Total allocations: %lu\n", (unsigned long)total_allocs);
    printf("Total repairs observed: %lu\n", (unsigned long)total_repairs);
    
    if (total_repairs > 0) {
        double repair_rate = (double)total_repairs / (double)total_allocs * 100.0;
        printf("Repair rate: %.6f%% (1 per %.0f allocations)\n",
               repair_rate, (double)total_allocs / (double)total_repairs);
        printf("\nInterpretation: Zombie partial repairs are an allocator health signal.\n");
        printf("  - Non-zero repairs: Publication races exist (expected under contention)\n");
        printf("  - Low rate (<0.01%%): Self-healing works, no performance impact\n");
        printf("  - High rate (>1%%): May indicate free_count divergence issue\n");
    } else {
        printf("\nNo repairs observed (clean run or insufficient contention)\n");
    }
    
#ifndef ENABLE_SLOWPATH_SAMPLING
    printf("\n(ENABLE_SLOWPATH_SAMPLING not defined - rebuild to see repair timing)\n");
#endif
    
    pthread_barrier_destroy(&start_barrier);
    slab_allocator_free(alloc);
    printf("\n=== Test Complete ===\n");
    return 0;
}
