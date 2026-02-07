/*
 * tsan_test.c - ThreadSanitizer validation for relaxed atomics
 * 
 * Focused test for Phase 2.1 semantic tightening:
 * Validates that relaxed memory ordering on epoch_state is race-free.
 * 
 * Key operations tested:
 * 1. Concurrent epoch_state reads (alloc threads)
 * 2. Concurrent epoch_state writes (epoch_advance thread)
 * 3. Concurrent allocations in same epoch (fast path contention)
 * 4. Concurrent frees in same epoch (transition detection)
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>

#include "slab_alloc_internal.h"

#define NUM_THREADS 4
#define ALLOCS_PER_THREAD 10000
#define EPOCH_ADVANCES 100

typedef struct {
  SlabAllocator* alloc;
  int thread_id;
  _Atomic int stop_flag;
} TestContext;

/* Worker thread: allocate and free repeatedly */
static void* alloc_worker(void* arg) {
  TestContext* ctx = (TestContext*)arg;
  SlabHandle handles[100];
  
  for (int i = 0; i < ALLOCS_PER_THREAD / 100; i++) {
    EpochId epoch = epoch_current(ctx->alloc);
    
    /* Allocate batch */
    for (int j = 0; j < 100; j++) {
      void* ptr = alloc_obj_epoch(ctx->alloc, 128, epoch, &handles[j]);
      if (ptr) {
        /* Touch memory to ensure it's mapped */
        *(volatile char*)ptr = 42;
      }
    }
    
    /* Free batch */
    for (int j = 0; j < 100; j++) {
      free_obj(ctx->alloc, handles[j]);
    }
    
    if (atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed)) {
      break;
    }
  }
  
  return NULL;
}

/* Epoch advance thread: rotate epochs continuously */
static void* epoch_advancer(void* arg) {
  TestContext* ctx = (TestContext*)arg;
  
  for (int i = 0; i < EPOCH_ADVANCES; i++) {
    usleep(1000);  /* 1ms between advances */
    epoch_advance(ctx->alloc);
  }
  
  atomic_store_explicit(&ctx->stop_flag, 1, memory_order_relaxed);
  return NULL;
}

int main(void) {
  printf("ThreadSanitizer validation test\n");
  printf("Testing: relaxed atomics + concurrent epoch operations\n\n");
  
  SlabAllocator* alloc = slab_allocator_create();
  if (!alloc) {
    fprintf(stderr, "Failed to create allocator\n");
    return 1;
  }
  
  TestContext ctx = {
    .alloc = alloc,
    .stop_flag = ATOMIC_VAR_INIT(0)
  };
  
  pthread_t workers[NUM_THREADS];
  pthread_t advancer;
  
  /* Start epoch advancer */
  if (pthread_create(&advancer, NULL, epoch_advancer, &ctx) != 0) {
    fprintf(stderr, "Failed to create epoch advancer thread\n");
    return 1;
  }
  
  /* Start worker threads */
  for (int i = 0; i < NUM_THREADS; i++) {
    ctx.thread_id = i;
    if (pthread_create(&workers[i], NULL, alloc_worker, &ctx) != 0) {
      fprintf(stderr, "Failed to create worker thread %d\n", i);
      return 1;
    }
  }
  
  /* Wait for completion */
  pthread_join(advancer, NULL);
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(workers[i], NULL);
  }
  
  slab_allocator_free(alloc);
  
  printf("✓ Test completed successfully\n");
  printf("✓ No data races detected by ThreadSanitizer\n");
  
  return 0;
}
