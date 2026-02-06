/*
 * benchmark_threads.c - Multi-threaded scaling benchmark
 * 
 * Tests how allocation latency scales with thread count.
 * Key property: Lock-free fast path should scale linearly until
 * cache coherence overhead dominates (~8-16 threads).
 * 
 * Measures:
 * - Per-thread p50/p95/p99 latency
 * - Aggregate throughput (ops/sec)
 * - Cache coherence effects
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "slab_alloc_internal.h"

/* Test parameters */
#define OBJECT_SIZE 128
#define OPS_PER_THREAD 100000  /* 100K ops per thread */
#define MAX_THREADS 16

/* Per-thread results */
typedef struct {
  uint64_t* latencies;
  size_t count;
  uint64_t p50;
  uint64_t p95;
  uint64_t p99;
  double avg;
} ThreadResult;

/* Shared state */
typedef struct {
  SlabAllocator* alloc;
  _Atomic int start_flag;
  ThreadResult results[MAX_THREADS];
} BenchState;

static int compare_uint64(const void* a, const void* b) {
  uint64_t x = *(const uint64_t*)a;
  uint64_t y = *(const uint64_t*)b;
  if (x < y) return -1;
  if (x > y) return 1;
  return 0;
}

static uint64_t percentile(uint64_t* arr, size_t n, double p) {
  if (p < 0.0) p = 0.0;
  if (p > 1.0) p = 1.0;
  size_t idx = (size_t)(p * (double)(n - 1) + 0.5);
  if (idx >= n) idx = n - 1;
  return arr[idx];
}

static void* worker_thread(void* arg) {
  BenchState* state = (BenchState*)arg;
  long thread_id = (long)pthread_self() % MAX_THREADS;
  ThreadResult* result = &state->results[thread_id];
  
  /* Allocate latency array */
  result->latencies = (uint64_t*)malloc(OPS_PER_THREAD * sizeof(uint64_t));
  if (!result->latencies) {
    fprintf(stderr, "Thread %ld: Failed to allocate latency array\n", thread_id);
    return NULL;
  }
  result->count = OPS_PER_THREAD;
  
  /* Allocate handle array */
  SlabHandle* handles = (SlabHandle*)calloc(OPS_PER_THREAD, sizeof(SlabHandle));
  if (!handles) {
    fprintf(stderr, "Thread %ld: Failed to allocate handle array\n", thread_id);
    free(result->latencies);
    return NULL;
  }
  
  /* Wait for start signal */
  while (atomic_load(&state->start_flag) == 0) {
    sched_yield();
  }
  
  /* Benchmark allocation */
  for (int i = 0; i < OPS_PER_THREAD; i++) {
    uint64_t t0 = now_ns();
    void* p = alloc_obj(state->alloc, OBJECT_SIZE, &handles[i]);
    uint64_t t1 = now_ns();
    
    if (!p) {
      fprintf(stderr, "Thread %ld: Allocation failed at %d\n", thread_id, i);
      break;
    }
    
    ((uint8_t*)p)[0] = 1;  /* Touch memory */
    result->latencies[i] = t1 - t0;
  }
  
  /* Free all */
  for (int i = 0; i < OPS_PER_THREAD; i++) {
    free_obj(state->alloc, handles[i]);
  }
  
  /* Compute statistics */
  qsort(result->latencies, result->count, sizeof(uint64_t), compare_uint64);
  result->p50 = percentile(result->latencies, result->count, 0.50);
  result->p95 = percentile(result->latencies, result->count, 0.95);
  result->p99 = percentile(result->latencies, result->count, 0.99);
  
  uint64_t sum = 0;
  for (size_t i = 0; i < result->count; i++) {
    sum += result->latencies[i];
  }
  result->avg = (double)sum / (double)result->count;
  
  free(handles);
  return NULL;
}

static void run_scaling_test(int num_threads, FILE* csv_file) {
  printf("\n=== Testing with %d thread(s) ===\n", num_threads);
  
  BenchState state;
  state.alloc = slab_allocator_create();
  if (!state.alloc) {
    fprintf(stderr, "Failed to create allocator\n");
    return;
  }
  
  atomic_store(&state.start_flag, 0);
  memset(state.results, 0, sizeof(state.results));
  
  /* Create threads */
  pthread_t threads[MAX_THREADS];
  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&threads[i], NULL, worker_thread, &state) != 0) {
      fprintf(stderr, "Failed to create thread %d\n", i);
      return;
    }
  }
  
  /* Small delay to let threads reach spin loop */
  usleep(10000);  /* 10ms */
  
  /* Start all threads simultaneously */
  uint64_t start_time = now_ns();
  atomic_store(&state.start_flag, 1);
  
  /* Wait for completion */
  for (int i = 0; i < num_threads; i++) {
    pthread_join(threads[i], NULL);
  }
  uint64_t end_time = now_ns();
  
  /* Aggregate results */
  uint64_t total_ops = 0;
  double avg_p50 = 0, avg_p95 = 0, avg_p99 = 0, avg_avg = 0;
  
  for (int i = 0; i < num_threads; i++) {
    ThreadResult* r = &state.results[i];
    if (r->count > 0) {
      total_ops += r->count;
      avg_p50 += r->p50;
      avg_p95 += r->p95;
      avg_p99 += r->p99;
      avg_avg += r->avg;
    }
  }
  
  if (num_threads > 0) {
    avg_p50 /= num_threads;
    avg_p95 /= num_threads;
    avg_p99 /= num_threads;
    avg_avg /= num_threads;
  }
  
  double duration_sec = (double)(end_time - start_time) / 1e9;
  double throughput = (double)total_ops / duration_sec;
  
  printf("Results:\n");
  printf("  Total ops:    %" PRIu64 "\n", total_ops);
  printf("  Duration:     %.3f sec\n", duration_sec);
  printf("  Throughput:   %.0f ops/sec\n", throughput);
  printf("  Avg latency:  %.1f ns\n", avg_avg);
  printf("  Avg p50:      %.0f ns\n", avg_p50);
  printf("  Avg p95:      %.0f ns\n", avg_p95);
  printf("  Avg p99:      %.0f ns\n", avg_p99);
  
  /* Write CSV */
  if (csv_file) {
    fprintf(csv_file, "temporal-slab,%d,%.0f,%.1f,%.0f,%.0f,%.0f\n",
            num_threads, throughput, avg_avg, avg_p50, avg_p95, avg_p99);
  }
  
  /* Cleanup */
  for (int i = 0; i < num_threads; i++) {
    if (state.results[i].latencies) {
      free(state.results[i].latencies);
    }
  }
  
  slab_allocator_free(state.alloc);
}

int main(int argc, char** argv) {
  /* Parse arguments */
  const char* csv_path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
      csv_path = argv[i + 1];
      i++;
    }
  }
  
  FILE* csv_file = NULL;
  if (csv_path) {
    csv_file = fopen(csv_path, "w");
    if (!csv_file) {
      fprintf(stderr, "Failed to open CSV file: %s\n", csv_path);
      return 1;
    }
    fprintf(csv_file, "allocator,threads,throughput_ops_sec,avg_ns,p50_ns,p95_ns,p99_ns\n");
  }
  
  printf("temporal-slab Multi-threaded Scaling Benchmark\n");
  printf("==============================================\n");
  printf("Object size: %d bytes\n", OBJECT_SIZE);
  printf("Ops per thread: %d\n", OPS_PER_THREAD);
  
  /* Test thread counts: 1, 2, 4, 8, 16 */
  int thread_counts[] = {1, 2, 4, 8, 16};
  for (size_t i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]); i++) {
    run_scaling_test(thread_counts[i], csv_file);
  }
  
  if (csv_file) {
    fclose(csv_file);
    printf("\nCSV written to: %s\n", csv_path);
  }
  
  printf("\n=== Scaling Benchmark Complete ===\n");
  return 0;
}
