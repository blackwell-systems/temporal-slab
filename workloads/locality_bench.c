/*
 * locality_bench.c - High temporal locality benchmark for TLS cache validation
 *
 * Workload: Each thread maintains a small working set (256 objects) with
 * random alloc/free within that set. This creates high temporal reuse,
 * which should benefit from TLS caching.
 *
 * Expected behavior:
 * - TLS enabled: High hit rate (>80%), low p99/p999
 * - TLS disabled: Lower hit rate (0%), baseline p99/p999
 *
 * Build:
 *   cd src/
 *   make CFLAGS="-DENABLE_TLS_CACHE=1 -I../include"
 *   gcc -O3 -pthread -I../include -DENABLE_TLS_CACHE=1 \
 *       ../workloads/locality_bench.c \
 *       slab_alloc.o slab_tls_cache.o epoch_domain.o slab_stats.o \
 *       -o ../workloads/locality_bench
 *
 * Run:
 *   ./workloads/locality_bench
 */

#include <slab_alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define WORKING_SET_SIZE 256     /* Small live set per thread */
#define OPS_PER_THREAD 1000000   /* 1M operations per thread */
#define OBJECT_SIZE 128          /* Single size class */
#define NUM_THREADS 4            /* Modest parallelism */

typedef struct {
    uint64_t alloc_latency_ns;
    uint64_t free_latency_ns;
    uint64_t alloc_count;
    uint64_t free_count;
} ThreadStats;

typedef struct {
    SlabAllocator* alloc;
    uint32_t thread_id;
    uint32_t epoch_id;
    ThreadStats stats;
} ThreadArg;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void* worker_thread(void* arg) {
    ThreadArg* targ = (ThreadArg*)arg;
    SlabAllocator* alloc = targ->alloc;
    uint32_t epoch_id = targ->epoch_id;
    uint32_t rng_state = targ->thread_id + 1;
    
    /* Working set: small array of live handles */
    SlabHandle working_set[WORKING_SET_SIZE];
    memset(working_set, 0, sizeof(working_set));
    uint32_t live_count = 0;
    
    uint64_t total_alloc_lat = 0;
    uint64_t total_free_lat = 0;
    uint64_t alloc_count = 0;
    uint64_t free_count = 0;
    
    /* Warmup: Fill working set */
    for (uint32_t i = 0; i < WORKING_SET_SIZE; i++) {
        SlabHandle h;
        alloc_obj_epoch(alloc, OBJECT_SIZE, epoch_id, &h);
        working_set[i] = h;
        live_count++;
    }
    
    /* Main loop: Random alloc/free within working set */
    for (uint32_t op = 0; op < OPS_PER_THREAD; op++) {
        uint32_t slot = xorshift32(&rng_state) % WORKING_SET_SIZE;
        
        if (working_set[slot] != 0) {
            /* Slot occupied: free it */
            uint64_t t0 = rdtsc();
            free_obj(alloc, working_set[slot]);
            uint64_t t1 = rdtsc();
            
            total_free_lat += (t1 - t0);
            free_count++;
            working_set[slot] = 0;
            live_count--;
        } else {
            /* Slot empty: allocate */
            SlabHandle h;
            uint64_t t0 = rdtsc();
            void* ptr = alloc_obj_epoch(alloc, OBJECT_SIZE, epoch_id, &h);
            uint64_t t1 = rdtsc();
            
            if (ptr) {
                total_alloc_lat += (t1 - t0);
                alloc_count++;
                working_set[slot] = h;
                live_count++;
            }
        }
    }
    
    /* Cleanup: Free all live objects */
    for (uint32_t i = 0; i < WORKING_SET_SIZE; i++) {
        if (working_set[i] != 0) {
            free_obj(alloc, working_set[i]);
        }
    }
    
    targ->stats.alloc_latency_ns = total_alloc_lat;
    targ->stats.free_latency_ns = total_free_lat;
    targ->stats.alloc_count = alloc_count;
    targ->stats.free_count = free_count;
    
    return NULL;
}

int main(void) {
    printf("=== Locality Benchmark ===\n");
    printf("Working set: %u objects per thread\n", WORKING_SET_SIZE);
    printf("Operations: %u per thread\n", OPS_PER_THREAD);
    printf("Threads: %u\n", NUM_THREADS);
    printf("Object size: %u bytes\n\n", OBJECT_SIZE);
    
    SlabAllocator* alloc = slab_allocator_create();
    if (!alloc) {
        fprintf(stderr, "Failed to create allocator\n");
        return 1;
    }
    
    uint32_t epoch_id = epoch_current(alloc);
    
    pthread_t threads[NUM_THREADS];
    ThreadArg thread_args[NUM_THREADS];
    
    /* Launch threads */
    for (uint32_t i = 0; i < NUM_THREADS; i++) {
        thread_args[i].alloc = alloc;
        thread_args[i].thread_id = i;
        thread_args[i].epoch_id = epoch_id;
        memset(&thread_args[i].stats, 0, sizeof(ThreadStats));
        
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_args[i]) != 0) {
            fprintf(stderr, "Failed to create thread %u\n", i);
            return 1;
        }
    }
    
    /* Wait for completion */
    for (uint32_t i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Aggregate stats */
    uint64_t total_alloc_lat = 0;
    uint64_t total_free_lat = 0;
    uint64_t total_alloc_count = 0;
    uint64_t total_free_count = 0;
    
    for (uint32_t i = 0; i < NUM_THREADS; i++) {
        total_alloc_lat += thread_args[i].stats.alloc_latency_ns;
        total_free_lat += thread_args[i].stats.free_latency_ns;
        total_alloc_count += thread_args[i].stats.alloc_count;
        total_free_count += thread_args[i].stats.free_count;
    }
    
    double avg_alloc_cycles = (double)total_alloc_lat / total_alloc_count;
    double avg_free_cycles = (double)total_free_lat / total_free_count;
    
    printf("Results:\n");
    printf("  Alloc operations: %lu (avg %.1f cycles)\n", total_alloc_count, avg_alloc_cycles);
    printf("  Free operations:  %lu (avg %.1f cycles)\n\n", total_free_count, avg_free_cycles);
    
#if ENABLE_TLS_CACHE
    /* Print TLS stats to validate hit rate */
    extern void tls_print_stats(void);
    tls_print_stats();
#else
    printf("TLS cache disabled (ENABLE_TLS_CACHE=0)\n\n");
#endif
    
    
#if ENABLE_SLOWPATH_SAMPLING
    /* Print slowpath samples for WSL2/VM tail latency diagnosis */
    extern void slowpath_print_samples(SlabAllocator* a);
    slowpath_print_samples(alloc);
#endif
    
    slab_allocator_free(alloc);
    
    printf("Expected TLS behavior:\n");
    printf("  - High hit rate (>80%%) with small working set\n");
    printf("  - Low bypass rate (<5%%)\n");
    printf("  - Alloc latency should be lower than baseline\n");
    
    return 0;
}
