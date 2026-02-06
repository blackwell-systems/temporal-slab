/*
 * smoke_tests.c - temporal-slab Phase 1.5 Smoke Tests
 * 
 * Basic correctness tests:
 * - Single-threaded alloc/free
 * - Multi-threaded alloc/free (8 threads x 500K ops)
 * - Simple micro-benchmark
 */

#define _GNU_SOURCE
#include "slab_alloc_internal.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------ Single-thread smoke test ------------------------------ */

void smoke_test_single_thread(void) {
  SlabAllocator a;
  allocator_init(&a);

  const int N = 20000;
  SlabHandle* hs = (SlabHandle*)calloc((size_t)N, sizeof(SlabHandle));
  if (!hs) exit(1);

  for (int i = 0; i < N; i++) {
    void* p = alloc_obj(&a, 128, &hs[i]);
    if (!p) {
      fprintf(stderr, "alloc failed at %d (errno=%d)\n", i, errno);
      exit(1);
    }
    memset(p, (unsigned char)(i & 0xFF), 128);
  }

  for (int i = 0; i < N; i += 2) {
    if (!free_obj(&a, hs[i])) {
      fprintf(stderr, "free failed at %d\n", i);
      exit(1);
    }
  }

  for (int i = 0; i < N / 2; i++) {
    SlabHandle h;
    void* p = alloc_obj(&a, 128, &h);
    if (!p) {
      fprintf(stderr, "re-alloc failed at %d\n", i);
      exit(1);
    }
    memset(p, 0xAB, 128);
  }

  free(hs);
  allocator_destroy(&a);

  printf("smoke_test_single_thread: OK\n");
}

/* ------------------------------ Multi-thread smoke test ------------------------------ */

typedef struct ThreadArgs {
  SlabAllocator* alloc;
  int iters;
  int thread_id;
} ThreadArgs;

static void* worker_alloc_free(void* arg) {
  ThreadArgs* a = (ThreadArgs*)arg;
  SlabHandle* hs = (SlabHandle*)malloc((size_t)a->iters * sizeof(SlabHandle));
  if (!hs) return NULL;

  for (int i = 0; i < a->iters; i++) {
    void* p = alloc_obj(a->alloc, 128, &hs[i]);
    if (!p) {
      free(hs);
      return (void*)1;
    }
    ((uint8_t*)p)[0] = (uint8_t)(a->thread_id);
  }

  for (int i = 0; i < a->iters; i++) {
    if (!free_obj(a->alloc, hs[i])) {
      free(hs);
      return (void*)1;
    }
  }

  free(hs);
  return NULL;
}

void smoke_test_multi_thread(void) {
  SlabAllocator a;
  allocator_init(&a);

  const int threads = 8;
  const int iters_per = 500000;

  pthread_t th[threads];
  ThreadArgs args[threads];

  for (int i = 0; i < threads; i++) {
    args[i].alloc = &a;
    args[i].iters = iters_per;
    args[i].thread_id = i;
    if (pthread_create(&th[i], NULL, worker_alloc_free, &args[i]) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      exit(1);
    }
  }

  for (int i = 0; i < threads; i++) {
    void* ret = NULL;
    pthread_join(th[i], &ret);
    if (ret != NULL) {
      fprintf(stderr, "multi-thread worker %d failed\n", i);
      exit(1);
    }
  }

  allocator_destroy(&a);
  printf("smoke_test_multi_thread: OK (%d threads x %d iters)\n", threads, iters_per);
}

/* ------------------------------ Micro bench ------------------------------ */

void micro_bench(void) {
  SlabAllocator a;
  allocator_init(&a);

  const int N = 2 * 1000 * 1000;
  SlabHandle* hs = (SlabHandle*)malloc((size_t)N * sizeof(SlabHandle));
  if (!hs) exit(1);

  uint64_t t0 = now_ns();
  for (int i = 0; i < N; i++) {
    void* p = alloc_obj(&a, 128, &hs[i]);
    if (!p) {
      fprintf(stderr, "alloc failed at %d\n", i);
      exit(1);
    }
    ((uint8_t*)p)[0] = 1;
  }
  uint64_t t1 = now_ns();

  for (int i = 0; i < N; i++) {
    if (!free_obj(&a, hs[i])) {
      fprintf(stderr, "free failed at %d\n", i);
      exit(1);
    }
  }
  uint64_t t2 = now_ns();

  uint64_t rss = read_rss_bytes_linux();

  double alloc_ns = (double)(t1 - t0) / (double)N;
  double free_ns  = (double)(t2 - t1) / (double)N;

  printf("micro_bench (128B):\n");
  printf("  alloc avg: %.1f ns/op\n", alloc_ns);
  printf("  free  avg: %.1f ns/op\n", free_ns);
  if (rss) {
    printf("  RSS: %" PRIu64 " bytes (%.2f MiB)\n", rss, (double)rss / (1024.0 * 1024.0));
  } else {
    printf("  RSS: (unavailable on this platform)\n");
  }

  free(hs);
  allocator_destroy(&a);
}

/* ------------------------------ Main ------------------------------ */

int main(void) {
  printf("Starting smoke_test_single_thread...\n");
  fflush(stdout);
  smoke_test_single_thread();
  
  printf("Starting smoke_test_multi_thread...\n");
  fflush(stdout);
  smoke_test_multi_thread();
  
  printf("Starting micro_bench...\n");
  fflush(stdout);
  micro_bench();
  return 0;
}
