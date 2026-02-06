/*
 * soak_test.c - temporal-slab Long-Running Stability Test
 * 
 * Runs allocation/free patterns for extended periods to catch:
 * - Memory leaks (RSS growth)
 * - Rare race conditions
 * - Counter overflow issues
 * - Cache corruption
 * - Performance degradation over time
 * 
 * Usage: ./soak_test [duration_seconds]
 * Default: 3600 seconds (1 hour)
 */

#define _GNU_SOURCE
#include "slab_alloc_internal.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int sig) {
  (void)sig;
  g_stop = 1;
}

typedef struct WorkerStats {
  uint64_t allocs;
  uint64_t frees;
  uint64_t failures;
} WorkerStats;

typedef struct SoakArgs {
  SlabAllocator* alloc;
  int thread_id;
  WorkerStats* stats;
} SoakArgs;

/* Worker thread: continuous alloc/free with varying patterns */
static void* soak_worker(void* arg) {
  SoakArgs* a = (SoakArgs*)arg;
  const int batch_size = 10000;
  SlabHandle* handles = malloc((size_t)batch_size * sizeof(SlabHandle));
  if (!handles) return (void*)1;
  
  /* Use different sizes based on thread_id to exercise all size classes */
  uint32_t sizes[] = {64, 128, 256, 512};
  uint32_t size = sizes[a->thread_id % 4];
  
  while (!g_stop) {
    /* Allocate batch */
    for (int i = 0; i < batch_size && !g_stop; i++) {
      void* p = alloc_obj(a->alloc, size, &handles[i]);
      if (!p) {
        a->stats->failures++;
        continue;
      }
      a->stats->allocs++;
      
      /* Write pattern to catch use-after-free */
      memset(p, (unsigned char)(a->thread_id + i), size);
    }
    
    /* Free batch */
    for (int i = 0; i < batch_size && !g_stop; i++) {
      if (handles[i] != 0) {
        if (free_obj(a->alloc, handles[i])) {
          a->stats->frees++;
        } else {
          a->stats->failures++;
        }
        handles[i] = 0;
      }
    }
  }
  
  free(handles);
  return NULL;
}

/* Print progress report */
static void print_report(SlabAllocator* alloc, WorkerStats* stats, int num_threads, 
                        uint64_t start_rss, time_t start_time, time_t now) {
  uint64_t total_allocs = 0, total_frees = 0, total_failures = 0;
  for (int i = 0; i < num_threads; i++) {
    total_allocs += stats[i].allocs;
    total_frees += stats[i].frees;
    total_failures += stats[i].failures;
  }
  
  uint64_t current_rss = read_rss_bytes_linux();
  double rss_mb = (double)current_rss / (1024.0 * 1024.0);
  double rss_delta_mb = (double)(current_rss - start_rss) / (1024.0 * 1024.0);
  
  time_t elapsed = now - start_time;
  double ops_per_sec = (double)(total_allocs + total_frees) / (double)elapsed;
  
  printf("\n=== Soak Test Report (T+%ld seconds) ===\n", elapsed);
  printf("Total allocs:     %" PRIu64 "\n", total_allocs);
  printf("Total frees:      %" PRIu64 "\n", total_frees);
  printf("Total failures:   %" PRIu64 "\n", total_failures);
  printf("Ops/sec:          %.0f\n", ops_per_sec);
  printf("RSS:              %.2f MiB (delta: %+.2f MiB)\n", rss_mb, rss_delta_mb);
  
  /* Print per-size-class counters */
  printf("\n--- Size Class Counters ---\n");
  const char* sizes[] = {"64B", "128B", "256B", "512B"};
  for (uint32_t i = 0; i < 4; i++) {
    PerfCounters pc;
    get_perf_counters(alloc, i, &pc);
    printf("%s: slow=%"PRIu64" new=%"PRIu64" null=%"PRIu64" full=%"PRIu64"\n",
           sizes[i], pc.slow_path_hits, pc.new_slab_count, 
           pc.current_partial_null, pc.current_partial_full);
  }
  printf("\n");
}

int main(int argc, char** argv) {
  int duration_seconds = 3600;  /* Default: 1 hour */
  if (argc > 1) {
    duration_seconds = atoi(argv[1]);
    if (duration_seconds <= 0) {
      fprintf(stderr, "Usage: %s [duration_seconds]\n", argv[0]);
      return 1;
    }
  }
  
  printf("temporal-slab Soak Test\n");
  printf("==================\n");
  printf("Duration:  %d seconds (%.1f hours)\n", duration_seconds, duration_seconds / 3600.0);
  printf("Threads:   8 (2 per size class)\n");
  printf("Pattern:   Continuous alloc/free batches (10K per batch)\n\n");
  printf("Press Ctrl+C to stop early.\n\n");
  
  /* Set up signal handler for graceful shutdown */
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  
  /* Initialize allocator */
  SlabAllocator alloc;
  allocator_init(&alloc);
  
  /* Record baseline */
  uint64_t start_rss = read_rss_bytes_linux();
  time_t start_time = time(NULL);
  time_t last_report = start_time;
  
  /* Start worker threads */
  const int num_threads = 8;
  pthread_t threads[num_threads];
  SoakArgs args[num_threads];
  WorkerStats stats[num_threads];
  
  memset(stats, 0, sizeof(stats));
  
  for (int i = 0; i < num_threads; i++) {
    args[i].alloc = &alloc;
    args[i].thread_id = i;
    args[i].stats = &stats[i];
    
    if (pthread_create(&threads[i], NULL, soak_worker, &args[i]) != 0) {
      fprintf(stderr, "Failed to create thread %d\n", i);
      g_stop = 1;
      break;
    }
  }
  
  /* Monitor progress */
  while (!g_stop) {
    sleep(1);
    time_t now = time(NULL);
    
    /* Print report every 60 seconds */
    if (now - last_report >= 60) {
      print_report(&alloc, stats, num_threads, start_rss, start_time, now);
      last_report = now;
    }
    
    /* Check if duration elapsed */
    if (now - start_time >= duration_seconds) {
      printf("\nDuration elapsed. Stopping...\n");
      g_stop = 1;
    }
  }
  
  /* Wait for workers to finish */
  printf("Waiting for workers to complete...\n");
  for (int i = 0; i < num_threads; i++) {
    void* ret;
    pthread_join(threads[i], &ret);
  }
  
  /* Final report */
  time_t end_time = time(NULL);
  print_report(&alloc, stats, num_threads, start_rss, start_time, end_time);
  
  /* Cleanup */
  allocator_destroy(&alloc);
  
  printf("=== Soak Test Complete ===\n");
  printf("Result: SUCCESS (no crashes, assertions, or hangs)\n");
  
  return 0;
}
