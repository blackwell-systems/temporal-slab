/*
 * contention_sampling_test.c - Multi-threaded contention test with sampling
 * 
 * Purpose: Validate probabilistic sampling under high contention
 * 
 * Test pattern:
 * - Multiple threads (default 8) allocate from same size class
 * - Shared allocator, shared epoch
 * - Measure wall vs CPU time split under contention
 * - Report per-thread samples and aggregate statistics
 * 
 * Expected results:
 * - CPU time increases with contention (locks, CAS retries, repairs)
 * - Wall time includes both CPU work and scheduler interference
 * - wait_ns shows scheduler contribution separately
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

#define DEFAULT_THREADS 8
#define ALLOCS_PER_THREAD 100000
#define ALLOC_SIZE 128
#define EPOCH_ID 0

typedef struct {
    SlabAllocator* alloc;
    int thread_id;
    _Atomic uint64_t* total_allocs;
} ThreadArgs;

void* worker_thread(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    SlabAllocator* alloc = args->alloc;
    int tid = args->thread_id;
    
    // Pin thread to specific core (reduce variability on Linux)
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(tid % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    
    printf("Thread %d starting (%d allocations)...\n", tid, ALLOCS_PER_THREAD);
    
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        SlabHandle handle;
        void* ptr = alloc_obj_epoch(alloc, ALLOC_SIZE, EPOCH_ID, &handle);
        if (!ptr) {
            printf("Thread %d: Allocation %d FAILED\n", tid, i);
            return NULL;
        }
        free_obj(alloc, handle);
        atomic_fetch_add_explicit(args->total_allocs, 1, memory_order_relaxed);
    }
    
#ifdef ENABLE_SLOWPATH_SAMPLING
    // Report this thread's sampling statistics
    ThreadStats stats = slab_stats_thread();
    if (stats.alloc_samples > 0) {
        uint64_t avg_wall = stats.alloc_wall_ns_sum / stats.alloc_samples;
        uint64_t avg_cpu = stats.alloc_cpu_ns_sum / stats.alloc_samples;
        uint64_t avg_wait = stats.alloc_wait_ns_sum / stats.alloc_samples;
        
        printf("\n[Thread %d] Samples: %lu\n", tid, stats.alloc_samples);
        printf("  Avg: wall=%lu ns, cpu=%lu ns, wait=%lu ns (%.2fx)\n", 
               avg_wall, avg_cpu, avg_wait, (double)avg_wall / avg_cpu);
        printf("  Max: wall=%lu ns, cpu=%lu ns, wait=%lu ns\n",
               stats.alloc_wall_ns_max, stats.alloc_cpu_ns_max, stats.alloc_wait_ns_max);
        
        if (stats.repair_count > 0) {
            uint64_t avg_repair_wall = stats.repair_wall_ns_sum / stats.repair_count;
            uint64_t avg_repair_cpu = stats.repair_cpu_ns_sum / stats.repair_count;
            uint64_t avg_repair_wait = stats.repair_wait_ns_sum / stats.repair_count;
            printf("  Repairs: %lu (avg: wall=%lu ns, cpu=%lu ns, wait=%lu ns)\n",
                   stats.repair_count, avg_repair_wall, avg_repair_cpu, avg_repair_wait);
        }
    }
#endif
    
    printf("Thread %d completed\n", tid);
    return NULL;
}

int main(int argc, char** argv) {
    int num_threads = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREADS;
    if (num_threads < 1 || num_threads > 128) {
        printf("Usage: %s [threads] (1-128, default %d)\n", argv[0], DEFAULT_THREADS);
        return 1;
    }
    
    printf("=== Multi-threaded Contention Sampling Test ===\n");
    printf("Threads: %d\n", num_threads);
    printf("Allocations per thread: %d\n", ALLOCS_PER_THREAD);
    printf("Total allocations: %d\n", num_threads * ALLOCS_PER_THREAD);
    printf("Size class: %d bytes\n", ALLOC_SIZE);
    printf("Expected samples per thread: ~%d (1/1024 rate)\n\n", ALLOCS_PER_THREAD / 1024);
    
    SlabAllocator* alloc = slab_allocator_create();
    if (!alloc) {
        printf("Failed to create allocator\n");
        return 1;
    }
    
    _Atomic uint64_t total_allocs = 0;
    pthread_t threads[128];
    ThreadArgs thread_args[128];
    
    // Launch threads
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].alloc = alloc;
        thread_args[i].thread_id = i;
        thread_args[i].total_allocs = &total_allocs;
        
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_args[i]) != 0) {
            printf("Failed to create thread %d\n", i);
            return 1;
        }
    }
    
    // Wait for threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("\n=== Summary ===\n");
    printf("Total allocations completed: %lu\n", (unsigned long)total_allocs);
    
#ifndef ENABLE_SLOWPATH_SAMPLING
    printf("\n(ENABLE_SLOWPATH_SAMPLING not defined - no per-thread sampling data)\n");
    printf("Rebuild with: make CFLAGS=\"-DENABLE_SLOWPATH_SAMPLING -I../include\"\n");
#else
    printf("\n(Per-thread sampling statistics reported above)\n");
#endif
    
    slab_allocator_free(alloc);
    printf("\n=== Test Complete ===\n");
    return 0;
}
