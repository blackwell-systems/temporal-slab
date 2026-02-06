/*
  benchmark_accurate.c - Defensible RSS and latency measurements

  Improvements:
  1. Measures baseline RSS before allocations
  2. Frees handle array before final RSS measurement
  3. Uses compiler barriers to prevent optimization
  4. Reports p50/p99/p999 latencies (not just average)
  5. Separates allocator RSS from test infrastructure
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

/* ------------------------------ Utilities ------------------------------ */

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

/* Compiler barrier to prevent optimization */
static volatile uint8_t g_sink = 0;

static inline void barrier(void) {
  __asm__ volatile("" ::: "memory");
  g_sink++;
}

/* ------------------------------ Accurate RSS Benchmark ------------------------------ */

static void benchmark_rss_accurate(void) {
  printf("\n=== Accurate RSS Benchmark ===\n\n");

  SlabAllocator a;
  allocator_init(&a);

  /* Measure baseline RSS */
  uint64_t rss_baseline = read_rss_bytes_linux();
  printf("RSS baseline (after allocator_init): %.2f MiB\n", 
         (double)rss_baseline / (1024.0 * 1024.0));

  const int N = 2 * 1000 * 1000;
  
  /* Allocate handle array with mmap (so unmap actually returns pages) */
  size_t handle_size = (size_t)N * sizeof(SlabHandle);
  SlabHandle* hs = (SlabHandle*)mmap(NULL, handle_size,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);
  if (hs == MAP_FAILED) {
    fprintf(stderr, "Failed to mmap handle array\n");
    exit(1);
  }

  /* Touch pages to fault them in for accurate RSS */
  memset(hs, 0, handle_size);

  uint64_t rss_with_handles = read_rss_bytes_linux();
  printf("RSS with handle array:             %.2f MiB (+%.2f MiB)\n",
         (double)rss_with_handles / (1024.0 * 1024.0),
         (double)(rss_with_handles - rss_baseline) / (1024.0 * 1024.0));

  /* Allocate objects */
  printf("\nAllocating %d objects of 128 bytes...\n", N);
  for (int i = 0; i < N; i++) {
    void* p = alloc_obj(&a, 128, &hs[i]);
    if (!p) {
      fprintf(stderr, "Allocation failed at %d\n", i);
      exit(1);
    }
    /* Touch memory to ensure RSS accounting */
    ((uint8_t*)p)[0] = 1;
    barrier();
  }

  uint64_t rss_with_objects = read_rss_bytes_linux();
  printf("RSS with objects allocated:        %.2f MiB (+%.2f MiB)\n",
         (double)rss_with_objects / (1024.0 * 1024.0),
         (double)(rss_with_objects - rss_with_handles) / (1024.0 * 1024.0));

  /* Unmap handle array to isolate allocator RSS (returns pages to OS) */
  munmap(hs, handle_size);
  
  uint64_t rss_allocator_only = read_rss_bytes_linux();
  printf("RSS after freeing handles:         %.2f MiB\n",
         (double)rss_allocator_only / (1024.0 * 1024.0));

  /* Calculate overhead */
  uint64_t payload_bytes = (uint64_t)N * 128;
  uint64_t allocator_bytes = rss_allocator_only - rss_baseline;
  
  printf("\n--- Analysis ---\n");
  printf("Payload (2M x 128B):               %.2f MiB\n",
         (double)payload_bytes / (1024.0 * 1024.0));
  printf("Allocator RSS (delta):             %.2f MiB\n",
         (double)allocator_bytes / (1024.0 * 1024.0));
  printf("Overhead:                          %.1f%%\n",
         ((double)allocator_bytes / (double)payload_bytes - 1.0) * 100.0);
  
  /* Calculate expected slab usage */
  uint32_t objects_per_slab = slab_object_count(128);
  uint64_t num_slabs = ((uint64_t)N + objects_per_slab - 1) / objects_per_slab;
  uint64_t slab_bytes = num_slabs * SLAB_PAGE_SIZE;
  
  printf("\nExpected (theoretical):\n");
  printf("Objects per slab:                  %u\n", objects_per_slab);
  printf("Number of slabs:                   %" PRIu64 "\n", num_slabs);
  printf("Slab memory:                       %.2f MiB\n",
         (double)slab_bytes / (1024.0 * 1024.0));
  printf("Slab overhead vs payload:          %.1f%%\n",
         ((double)slab_bytes / (double)payload_bytes - 1.0) * 100.0);

  allocator_destroy(&a);
}

/* ------------------------------ Accurate Latency Benchmark ------------------------------ */

static void benchmark_latency_accurate(void) {
  printf("\n=== Accurate Latency Benchmark ===\n\n");

  SlabAllocator a;
  allocator_init(&a);

  const int N = 1000 * 1000;  /* 1M iterations */
  
  /* Use mmap for all arrays so they can be cleanly unmapped */
  size_t hs_size = (size_t)N * sizeof(SlabHandle);
  size_t times_size = (size_t)N * sizeof(uint64_t);
  
  SlabHandle* hs = (SlabHandle*)mmap(NULL, hs_size,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);
  uint64_t* alloc_times = (uint64_t*)mmap(NULL, times_size,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS,
                                           -1, 0);
  uint64_t* free_times = (uint64_t*)mmap(NULL, times_size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS,
                                          -1, 0);
  
  if (hs == MAP_FAILED || alloc_times == MAP_FAILED || free_times == MAP_FAILED) {
    fprintf(stderr, "Failed to mmap measurement arrays\n");
    exit(1);
  }
  
  /* Touch pages to fault them in */
  memset(hs, 0, hs_size);
  memset(alloc_times, 0, times_size);
  memset(free_times, 0, times_size);

  /* Measure allocation latencies */
  printf("Measuring allocation latency (1M iterations)...\n");
  for (int i = 0; i < N; i++) {
    uint64_t t0 = now_ns();
    void* p = alloc_obj(&a, 128, &hs[i]);
    uint64_t t1 = now_ns();
    
    if (!p) {
      fprintf(stderr, "Allocation failed at %d\n", i);
      exit(1);
    }
    
    ((uint8_t*)p)[0] = 1;
    barrier();  /* Prevent optimization */
    
    alloc_times[i] = t1 - t0;
  }

  /* Measure free latencies */
  printf("Measuring free latency (1M iterations)...\n");
  for (int i = 0; i < N; i++) {
    uint64_t t0 = now_ns();
    bool ok = free_obj(&a, hs[i]);
    uint64_t t1 = now_ns();
    
    barrier();  /* Prevent optimization */
    
    if (!ok) {
      fprintf(stderr, "Free failed at %d\n", i);
      exit(1);
    }
    
    free_times[i] = t1 - t0;
  }

  /* Sort once for percentile calculations */
  qsort(alloc_times, N, sizeof(uint64_t), compare_uint64);
  qsort(free_times, N, sizeof(uint64_t), compare_uint64);
  
  /* Calculate percentiles from sorted arrays */
  uint64_t alloc_p50 = percentile(alloc_times, N, 0.50);
  uint64_t alloc_p99 = percentile(alloc_times, N, 0.99);
  uint64_t alloc_p999 = percentile(alloc_times, N, 0.999);
  
  uint64_t free_p50 = percentile(free_times, N, 0.50);
  uint64_t free_p99 = percentile(free_times, N, 0.99);
  uint64_t free_p999 = percentile(free_times, N, 0.999);

  /* Calculate averages */
  uint64_t alloc_sum = 0, free_sum = 0;
  for (int i = 0; i < N; i++) {
    alloc_sum += alloc_times[i];
    free_sum += free_times[i];
  }
  double alloc_avg = (double)alloc_sum / (double)N;
  double free_avg = (double)free_sum / (double)N;

  printf("\n--- Allocation Latency ---\n");
  printf("Average: %.1f ns\n", alloc_avg);
  printf("p50:     %" PRIu64 " ns\n", alloc_p50);
  printf("p99:     %" PRIu64 " ns\n", alloc_p99);
  printf("p999:    %" PRIu64 " ns\n", alloc_p999);

  printf("\n--- Free Latency ---\n");
  printf("Average: %.1f ns\n", free_avg);
  printf("p50:     %" PRIu64 " ns\n", free_p50);
  printf("p99:     %" PRIu64 " ns\n", free_p99);
  printf("p999:    %" PRIu64 " ns\n", free_p999);

  /* Get performance counters for 128B size class (index 1) */
  PerfCounters counters;
  get_perf_counters(&a, 1, &counters);

  printf("\n--- Tail Latency Attribution (128B size class) ---\n");
  printf("Slow path hits:             %" PRIu64 "\n", counters.slow_path_hits);
  printf("New slabs allocated:        %" PRIu64 "\n", counters.new_slab_count);
  printf("Moves PARTIAL->FULL:        %" PRIu64 "\n", counters.list_move_partial_to_full);
  printf("Moves FULL->PARTIAL:        %" PRIu64 "\n", counters.list_move_full_to_partial);
  printf("current_partial NULL:       %" PRIu64 " (no slab cached)\n", counters.current_partial_null);
  printf("current_partial FULL:       %" PRIu64 " (cached slab was full)\n", counters.current_partial_full);
  
  /* Attribution explanation */
  printf("\nAttribution:\n");
  if (counters.new_slab_count > 0) {
    printf("  - p99/p999 spikes primarily from %" PRIu64 " new slab allocations (mmap)\n", 
           counters.new_slab_count);
  }
  if (counters.slow_path_hits > counters.new_slab_count) {
    printf("  - Additional slow path hits (%" PRIu64 ") from list contention\n", 
           counters.slow_path_hits - counters.new_slab_count);
  }
  if (counters.new_slab_count == 0) {
    printf("  - All allocations served from slab cache (no mmap calls)\n");
  }

  /* Unmap all measurement arrays */
  munmap(alloc_times, times_size);
  munmap(free_times, times_size);
  munmap(hs, hs_size);
  
  allocator_destroy(&a);
}

/* ------------------------------ Main ------------------------------ */

int main(void) {
  printf("ZNS-Slab Phase 1 - Accurate Benchmarks\n");
  printf("======================================\n");
  
  benchmark_rss_accurate();
  benchmark_latency_accurate();
  
  printf("\n=== All Benchmarks Complete ===\n");
  return 0;
}
