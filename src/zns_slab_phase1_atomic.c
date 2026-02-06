/*
  zns_slab_phase1_atomic.c  — Phase 1 "Core Slab Allocator" (C, atomic bitmap)

  Adds vs previous:
    - Atomic bitmap words with CAS loops for concurrent alloc/free within a slab
    - O(1) list membership via slab->list_id (no linear search)
    - Optional multi-thread smoke (POSIX threads)

  Notes:
    - This is still Phase 1: allocator only (no hash table, no eviction, no tiers).
    - Concurrency model:
        * Slot allocation/free within a slab uses atomic bitmap CAS loops.
        * List operations (partial/full) are protected by a per-size-class mutex.
      This is a practical Phase 1 stepping stone: fast slot ops, simple list locking.
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

/* ------------------------------ Config ------------------------------ */

#ifndef SLAB_PAGE_SIZE
#define SLAB_PAGE_SIZE 4096u
#endif

_Static_assert((SLAB_PAGE_SIZE & (SLAB_PAGE_SIZE - 1)) == 0, "SLAB_PAGE_SIZE must be power of two");

/* Size classes (Phase 1) */
static const uint32_t k_size_classes[] = {64u, 128u, 256u, 512u};
static const size_t   k_num_classes   = sizeof(k_size_classes) / sizeof(k_size_classes[0]);

/* ------------------------------ Utilities ------------------------------ */

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint32_t ctz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctz(x);
#else
  uint32_t n = 0;
  while ((x & 1u) == 0u) { x >>= 1u; n++; }
  return n;
#endif
}

static inline int class_index_for_size(uint32_t sz) {
  for (size_t i = 0; i < k_num_classes; i++) {
    if (sz <= k_size_classes[i]) return (int)i;
  }
  return -1;
}

/* ------------------------------ Slab layout ------------------------------ */

#define SLAB_MAGIC   0x534C4142u /* "SLAB" */
#define SLAB_VERSION 1u

typedef enum SlabListId {
  SLAB_LIST_NONE    = 0,
  SLAB_LIST_PARTIAL = 1,
  SLAB_LIST_FULL    = 2,
} SlabListId;

/* Forward declare for list */
typedef struct Slab Slab;

struct Slab {
  /* Intrusive list links (protected by size-class mutex) */
  Slab* prev;
  Slab* next;

  /* Metadata */
  uint32_t magic;
  uint32_t version;
  uint32_t object_size;
  uint32_t object_count;

  /* free_count is advisory for metrics; correctness comes from bitmap.
     We keep it atomic to allow approximate tracking.
  */
  _Atomic uint32_t free_count;

  /* List membership for O(1) moves (protected by size-class mutex) */
  SlabListId list_id;

  /* Padding to keep header nice-ish; actual header size is rounded up anyway */
  uint8_t _pad[3];

  /* Bitmap words stored immediately after header (rounded up) */
};

/* ------------------------------ Intrusive list ------------------------------ */

typedef struct SlabList {
  Slab* head;
  Slab* tail;
  size_t len;
} SlabList;

static inline void list_init(SlabList* l) {
  l->head = NULL; l->tail = NULL; l->len = 0;
}

static inline void list_push_back(SlabList* l, Slab* s) {
  s->prev = l->tail;
  s->next = NULL;
  if (l->tail) l->tail->next = s;
  else l->head = s;
  l->tail = s;
  l->len++;
}

static inline void list_remove(SlabList* l, Slab* s) {
  if (s->prev) s->prev->next = s->next;
  else l->head = s->next;

  if (s->next) s->next->prev = s->prev;
  else l->tail = s->prev;

  s->prev = NULL;
  s->next = NULL;
  if (l->len > 0) l->len--;
}

/* ------------------------------ Slab helper functions ------------------------------ */

static inline size_t slab_header_size(void) {
  size_t h = sizeof(Slab);
  return (h + 63u) & ~63u; /* 64-byte alignment */
}

static inline uint32_t slab_bitmap_words(uint32_t obj_count) {
  return (obj_count + 31u) / 32u;
}

/* Bitmap pointer as atomic u32 words */
static inline _Atomic uint32_t* slab_bitmap_ptr(Slab* s) {
  return (_Atomic uint32_t*)((uint8_t*)s + slab_header_size());
}

static inline uint8_t* slab_data_ptr(Slab* s) {
  uint32_t words = slab_bitmap_words(s->object_count);
  return (uint8_t*)slab_bitmap_ptr(s) + ((size_t)words * 4u);
}

static inline void* slab_slot_ptr(Slab* s, uint32_t slot_index) {
  return (void*)(slab_data_ptr(s) + ((size_t)slot_index * (size_t)s->object_size));
}

/*
  Compute object_count by iterating because bitmap size depends on object_count.
  Bitmap is stored as u32 words (bitmap_bytes = words*4).
*/
static inline uint32_t slab_object_count(uint32_t obj_size) {
  const size_t hdr = slab_header_size();
  size_t available = SLAB_PAGE_SIZE - hdr;
  uint32_t count = (uint32_t)(available / obj_size);
  if (count == 0) return 0;

  for (int iter = 0; iter < 8; iter++) {
    uint32_t words = (count + 31u) / 32u;
    size_t bitmap_bytes = (size_t)words * 4u;
    if (bitmap_bytes > available) return 0;
    size_t data_bytes = available - bitmap_bytes;
    uint32_t new_count = (uint32_t)(data_bytes / obj_size);
    if (new_count == count) break;
    count = new_count;
    if (count == 0) break;
  }
  return count;
}

/* ------------------------------ mmap ------------------------------ */

static void* map_one_page(void) {
  void* p = mmap(NULL, SLAB_PAGE_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p == MAP_FAILED) return NULL;

  if (((uintptr_t)p & (SLAB_PAGE_SIZE - 1u)) != 0u) {
    munmap(p, SLAB_PAGE_SIZE);
    errno = EINVAL;
    return NULL;
  }
  return p;
}

static void unmap_one_page(void* p) {
  if (p) munmap(p, SLAB_PAGE_SIZE);
}

/* ------------------------------ Atomic bitmap ops ------------------------------ */

/*
  Allocate a slot via CAS:
    - scan words until find one with a 0 bit
    - pick lowest free bit (CTZ(~word))
    - attempt CAS to set that bit
    - retry if CAS fails (someone else raced)

  Returns:
    - slot index [0..object_count-1] on success
    - UINT32_MAX if no free slot
*/
static uint32_t slab_alloc_slot_atomic(Slab* s) {
  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  const uint32_t words = slab_bitmap_words(s->object_count);

  for (uint32_t w = 0; w < words; w++) {
    while (1) {
      uint32_t x = atomic_load_explicit(&bm[w], memory_order_relaxed);
      if (x == 0xFFFFFFFFu) break; /* this word full, move to next word */

      uint32_t free_mask = ~x;

      /* Mask invalid bits in last word */
      if (w == words - 1u) {
        uint32_t valid_bits = s->object_count - (w * 32u);
        if (valid_bits < 32u) {
          uint32_t valid_mask = (valid_bits == 32u) ? 0xFFFFFFFFu : ((1u << valid_bits) - 1u);
          free_mask &= valid_mask;
          if (free_mask == 0u) break; /* no valid free bit in this word */
        }
      }

      uint32_t bit = ctz32(free_mask);
      uint32_t mask = 1u << bit;
      uint32_t desired = x | mask;

      /* CAS: if succeeds, we own the slot */
      if (atomic_compare_exchange_weak_explicit(
              &bm[w], &x, desired,
              memory_order_acq_rel,
              memory_order_relaxed)) {
        atomic_fetch_sub_explicit(&s->free_count, 1u, memory_order_relaxed);
        return w * 32u + bit;
      }

      /* else: x updated to observed value; retry inner loop */
    }
  }

  return UINT32_MAX;
}

static bool slab_free_slot_atomic(Slab* s, uint32_t idx) {
  if (idx >= s->object_count) return false;

  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  uint32_t w = idx / 32u;
  uint32_t bit = idx % 32u;
  uint32_t mask = 1u << bit;

  while (1) {
    uint32_t x = atomic_load_explicit(&bm[w], memory_order_relaxed);
    if ((x & mask) == 0u) return false; /* already free */

    uint32_t desired = x & ~mask;
    if (atomic_compare_exchange_weak_explicit(
            &bm[w], &x, desired,
            memory_order_acq_rel,
            memory_order_relaxed)) {
      atomic_fetch_add_explicit(&s->free_count, 1u, memory_order_relaxed);
      return true;
    }
  }
}

/* ------------------------------ Allocator ------------------------------ */

typedef struct SizeClassAlloc {
  uint32_t object_size;

  /* Lists guarded by mutex */
  SlabList partial;
  SlabList full;

  pthread_mutex_t lock; /* guards list membership & slab creation */

  size_t total_slabs;
} SizeClassAlloc;

typedef struct SlabAllocator {
  SizeClassAlloc classes[4];
} SlabAllocator;

static void allocator_init(SlabAllocator* a) {
  memset(a, 0, sizeof(*a));
  for (size_t i = 0; i < k_num_classes; i++) {
    a->classes[i].object_size = k_size_classes[i];
    list_init(&a->classes[i].partial);
    list_init(&a->classes[i].full);
    pthread_mutex_init(&a->classes[i].lock, NULL);
    a->classes[i].total_slabs = 0;
  }
}

static Slab* new_slab(uint32_t obj_size) {
  void* page = map_one_page();
  if (!page) return NULL;

  Slab* s = (Slab*)page;

  uint32_t count = slab_object_count(obj_size);
  if (count == 0) {
    unmap_one_page(page);
    errno = EINVAL;
    return NULL;
  }

  s->prev = NULL;
  s->next = NULL;
  s->magic = SLAB_MAGIC;
  s->version = SLAB_VERSION;
  s->object_size = obj_size;
  s->object_count = count;
  atomic_store_explicit(&s->free_count, count, memory_order_relaxed);
  s->list_id = SLAB_LIST_NONE;

  /* Zero bitmap (atomic words) */
  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  uint32_t words = slab_bitmap_words(count);
  for (uint32_t i = 0; i < words; i++) {
    atomic_store_explicit(&bm[i], 0u, memory_order_relaxed);
  }

  return s;
}

typedef struct SlabHandle {
  Slab* slab;
  uint32_t slot;
  uint32_t size_class; /* 0..3 */
} SlabHandle;

/*
  Allocation strategy:
    1) Lock size-class to get a candidate slab from partial (or create one).
    2) Unlock size-class (so other threads can continue).
    3) CAS-allocate a slot in that slab.
    4) If slab became full (free_count==0), re-lock size-class and move it to FULL.
*/
static void* alloc_obj(SlabAllocator* a, uint32_t size, SlabHandle* out) {
  int ci = class_index_for_size(size);
  if (ci < 0) return NULL;

  SizeClassAlloc* sc = &a->classes[(size_t)ci];

  Slab* s = NULL;

  pthread_mutex_lock(&sc->lock);

  s = sc->partial.head;
  if (!s) {
    s = new_slab(sc->object_size);
    if (!s) {
      pthread_mutex_unlock(&sc->lock);
      return NULL;
    }
    s->list_id = SLAB_LIST_PARTIAL;
    list_push_back(&sc->partial, s);
    sc->total_slabs++;
  }

  /* Keep a pointer to attempt; do not remove from list yet */
  pthread_mutex_unlock(&sc->lock);

  if (!s || s->magic != SLAB_MAGIC) return NULL;

  uint32_t idx = slab_alloc_slot_atomic(s);
  if (idx == UINT32_MAX) {
    /* That slab was actually full; we need to repair list membership and retry. */
    pthread_mutex_lock(&sc->lock);
    if (s->list_id == SLAB_LIST_PARTIAL) {
      /* Move to FULL */
      list_remove(&sc->partial, s);
      s->list_id = SLAB_LIST_FULL;
      list_push_back(&sc->full, s);
    }
    /* Try another partial slab or create new */
    s = sc->partial.head;
    if (!s) {
      s = new_slab(sc->object_size);
      if (!s) {
        pthread_mutex_unlock(&sc->lock);
        return NULL;
      }
      s->list_id = SLAB_LIST_PARTIAL;
      list_push_back(&sc->partial, s);
      sc->total_slabs++;
    }
    pthread_mutex_unlock(&sc->lock);

    idx = slab_alloc_slot_atomic(s);
    if (idx == UINT32_MAX) return NULL;
  }

  /* If slab is now full, move it to FULL list (O(1)) */
  if (atomic_load_explicit(&s->free_count, memory_order_relaxed) == 0u) {
    pthread_mutex_lock(&sc->lock);
    if (s->list_id == SLAB_LIST_PARTIAL) {
      list_remove(&sc->partial, s);
      s->list_id = SLAB_LIST_FULL;
      list_push_back(&sc->full, s);
    }
    pthread_mutex_unlock(&sc->lock);
  }

  void* p = slab_slot_ptr(s, idx);
  if (out) {
    out->slab = s;
    out->slot = idx;
    out->size_class = (uint32_t)ci;
  }
  return p;
}

/*
  Free strategy:
    - CAS-clear bitmap bit
    - If slab was FULL and now has space, move FULL->PARTIAL (O(1))
*/
static bool free_obj(SlabAllocator* a, SlabHandle h) {
  if (!h.slab) return false;
  if (h.size_class >= (uint32_t)k_num_classes) return false;

  SizeClassAlloc* sc = &a->classes[h.size_class];
  Slab* s = h.slab;
  if (s->magic != SLAB_MAGIC) return false;

  /* Check if it was full before free (approx but good enough for list move trigger) */
  bool was_full = (atomic_load_explicit(&s->free_count, memory_order_relaxed) == 0u);

  if (!slab_free_slot_atomic(s, h.slot)) return false;

  if (was_full) {
    pthread_mutex_lock(&sc->lock);
    if (s->list_id == SLAB_LIST_FULL) {
      list_remove(&sc->full, s);
      s->list_id = SLAB_LIST_PARTIAL;
      list_push_back(&sc->partial, s);
    }
    pthread_mutex_unlock(&sc->lock);
  }

  return true;
}

static void allocator_destroy(SlabAllocator* a) {
  for (size_t i = 0; i < k_num_classes; i++) {
    SizeClassAlloc* sc = &a->classes[i];

    pthread_mutex_lock(&sc->lock);

    Slab* cur = sc->partial.head;
    while (cur) {
      Slab* next = cur->next;
      unmap_one_page(cur);
      cur = next;
    }

    cur = sc->full.head;
    while (cur) {
      Slab* next = cur->next;
      unmap_one_page(cur);
      cur = next;
    }

    list_init(&sc->partial);
    list_init(&sc->full);
    sc->total_slabs = 0;

    pthread_mutex_unlock(&sc->lock);
    pthread_mutex_destroy(&sc->lock);
  }
}

/* ------------------------------ RSS (Linux) ------------------------------ */

static uint64_t read_rss_bytes_linux(void) {
#if defined(__linux__)
  FILE* f = fopen("/proc/self/statm", "r");
  if (!f) return 0;

  unsigned long size = 0, resident = 0;
  if (fscanf(f, "%lu %lu", &size, &resident) != 2) {
    fclose(f);
    return 0;
  }
  fclose(f);
  long page = sysconf(_SC_PAGESIZE);
  return (uint64_t)resident * (uint64_t)page;
#else
  return 0;
#endif
}

/* ------------------------------ Tests / Demo ------------------------------ */

static void smoke_test_single_thread(void) {
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

/* ------------------------------ Optional multi-thread smoke ------------------------------ */

typedef struct ThreadArgs {
  SlabAllocator* alloc;
  int iters;
  int thread_id;
} ThreadArgs;

static void* worker_alloc_free(void* arg) {
  ThreadArgs* a = (ThreadArgs*)arg;
  /* simple per-thread list of handles */
  SlabHandle* hs = (SlabHandle*)malloc((size_t)a->iters * sizeof(SlabHandle));
  if (!hs) return NULL;

  for (int i = 0; i < a->iters; i++) {
    void* p = alloc_obj(a->alloc, 128, &hs[i]);
    if (!p) {
      /* best-effort; in Phase 1 tests we treat this as failure */
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

static void smoke_test_multi_thread(void) {
  SlabAllocator a;
  allocator_init(&a);

  const int threads = 4;
  const int iters_per = 200000;

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
      fprintf(stderr, "multi-thread worker failed\n");
      exit(1);
    }
  }

  allocator_destroy(&a);
  printf("smoke_test_multi_thread: OK (%d threads)\n", threads);
}

/* ------------------------------ Micro bench ------------------------------ */

static void micro_bench(void) {
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
    /* touch for RSS */
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

int main(void) {
  smoke_test_single_thread();
  smoke_test_multi_thread();
  micro_bench();
  return 0;
}
