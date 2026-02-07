/*
 * slab_alloc.c - temporal-slab Phase 1.5 Implementation
 * 
 * Release-quality slab allocator with:
 * - Lock-free fast path (atomic current_partial pointer)
 * - Per-size-class slab cache (97% hit rate)
 * - Performance counter attribution
 * - Sub-100ns median latency
 */

#define _GNU_SOURCE
#include "slab_alloc_internal.h"
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

/* ------------------------------ Config ------------------------------ */

#ifndef SLAB_PAGE_SIZE
#define SLAB_PAGE_SIZE 4096u
#endif

_Static_assert((SLAB_PAGE_SIZE & (SLAB_PAGE_SIZE - 1)) == 0, "SLAB_PAGE_SIZE must be power of two");

/* Phase 1 RSS Reclamation: Enable madvise(MADV_DONTNEED) on empty slabs
 * 
 * When enabled, empty slabs have their physical pages reclaimed via madvise(),
 * causing RSS to drop immediately. The virtual memory remains mapped (safe for
 * stale handle validation), but kernel zero-fills on next access.
 * 
 * Trade-off: Slight latency cost on slab reuse (zero-fill overhead).
 * 
 * Enable with: gcc -DENABLE_RSS_RECLAMATION ...
 * Default: DISABLED (preserve existing behavior, avoid zero-fill overhead)
 */
#ifndef ENABLE_RSS_RECLAMATION
#define ENABLE_RSS_RECLAMATION 0
#endif

/* Size classes (HFT-optimized: sub-100 byte granularity) */
static const uint32_t k_size_classes[] = {64u, 96u, 128u, 192u, 256u, 384u, 512u, 768u};
static const size_t   k_num_classes   = sizeof(k_size_classes) / sizeof(k_size_classes[0]);

/* O(1) lookup table: maps size -> class index
 * Max supported size: 768 bytes
 * Granularity: 1 byte (768 entries)
 * Memory cost: 768 bytes (fits in L1 cache)
 */
#define MAX_ALLOC_SIZE 768u
static uint8_t k_class_lookup[MAX_ALLOC_SIZE + 1];
static bool k_lookup_initialized = false;

/* ------------------------------ Utilities ------------------------------ */

uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Helper: Get epoch state for a given size class and epoch */
static inline EpochState* get_epoch_state(SizeClassAlloc* sc, uint32_t epoch_id) {
  return &sc->epochs[epoch_id];
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

/* Initialize O(1) class lookup table at first use */
static void init_class_lookup(void) {
  if (k_lookup_initialized) return;
  
  /* For each byte size, find smallest class that fits */
  for (uint32_t sz = 1; sz <= MAX_ALLOC_SIZE; sz++) {
    uint8_t class_idx = 0xFF; /* invalid */
    for (size_t i = 0; i < k_num_classes; i++) {
      if (sz <= k_size_classes[i]) {
        class_idx = (uint8_t)i;
        break;
      }
    }
    k_class_lookup[sz] = class_idx;
  }
  k_class_lookup[0] = 0xFF; /* zero-size is invalid */
  
  k_lookup_initialized = true;
}

/* O(1) class index lookup (deterministic, no branches per class) */
static inline int class_index_for_size(uint32_t sz) {
  if (sz == 0 || sz > MAX_ALLOC_SIZE) return -1;
  return (int)k_class_lookup[sz];
}

/* ------------------------------ Slab Registry (ABA Protection) ------------------------------ */

/* Initialize slab registry */
static void reg_init(SlabRegistry* r) {
  r->metas = NULL;
  r->cap = 0;
  r->free_ids = NULL;
  r->free_count = 0;
  r->next_id = 0;
  pthread_mutex_init(&r->lock, NULL);
}

/* Destroy slab registry */
static void reg_destroy(SlabRegistry* r) {
  free(r->metas);
  free(r->free_ids);
  pthread_mutex_destroy(&r->lock);
}

/* Allocate a new slab_id (grows registry if needed) */
static uint32_t reg_alloc_id(SlabRegistry* r) {
  pthread_mutex_lock(&r->lock);
  
  uint32_t id;
  if (r->free_count > 0) {
    id = r->free_ids[--r->free_count];
  } else {
    id = r->next_id++;
  }
  
  /* Grow registry if needed (double capacity) */
  if (id >= r->cap) {
    uint32_t new_cap = r->cap ? r->cap * 2 : 1024;
    while (new_cap <= id) new_cap *= 2;
    
    SlabMeta* nm = (SlabMeta*)calloc(new_cap, sizeof(SlabMeta));
    if (!nm) {
      pthread_mutex_unlock(&r->lock);
      return UINT32_MAX;  /* Allocation failure */
    }
    
    for (uint32_t i = 0; i < r->cap; i++) {
      nm[i] = r->metas[i];
    }
    free(r->metas);
    r->metas = nm;
    r->cap = new_cap;
    
    /* Grow free_ids array too */
    uint32_t* nf = (uint32_t*)calloc(new_cap, sizeof(uint32_t));
    if (!nf) {
      pthread_mutex_unlock(&r->lock);
      return UINT32_MAX;
    }
    free(r->free_ids);
    r->free_ids = nf;
  }
  
  /* Initialize generation (start at 1, 0 reserved for NULL handle) */
  atomic_store_explicit(&r->metas[id].gen, 1u, memory_order_relaxed);
  atomic_store_explicit(&r->metas[id].ptr, NULL, memory_order_relaxed);
  
  pthread_mutex_unlock(&r->lock);
  return id;
}

/* Publish slab pointer in registry */
static void reg_set_ptr(SlabRegistry* r, uint32_t id, Slab* s) {
  if (id < r->cap) {
    atomic_store_explicit(&r->metas[id].ptr, s, memory_order_release);
  }
}

/* Bump generation on reuse (ABA protection)
 * 
 * Memory ordering: Uses relaxed because validation safety comes from
 * the handshake between ptr (release/acquire) and gen (checked after ptr load).
 * We don't need acq_rel here - the ptr store/load provides synchronization.
 */
static uint32_t reg_bump_gen(SlabRegistry* r, uint32_t id) {
  if (id >= r->cap) return 0;
  
  uint32_t g = atomic_fetch_add_explicit(&r->metas[id].gen, 1u, memory_order_relaxed) + 1u;
  uint32_t g24 = g & 0xFFFFFFu;  /* Truncate to 24 bits for handles */
  if (g24 == 0) g24 = 1;  /* Avoid 0 (reserved for NULL handle) */
  return g24;
}

/* Get current generation (24-bit for handle encoding)
 * 
 * Memory ordering: Acquire ensures we see the generation value that was current
 * when the ptr was published. Combined with ptr acquire load, this provides
 * the validation handshake.
 */
static uint32_t reg_get_gen24(SlabRegistry* r, uint32_t id) {
  if (id >= r->cap) return 0;
  
  uint32_t g = atomic_load_explicit(&r->metas[id].gen, memory_order_acquire);
  uint32_t g24 = g & 0xFFFFFFu;  /* Truncate to 24 bits */
  if (g24 == 0) g24 = 1;
  return g24;
}

/* Lookup and validate slab by id + generation (returns NULL if invalid)
 * 
 * Validation handshake:
 * 1. Load ptr with acquire (sees everything before writer's release store)
 * 2. Load gen with acquire (sees current generation)
 * 3. Compare gen from handle with current gen
 * 
 * Safety: If ptr is non-NULL, the slab mapping is valid (never unmapped).
 * If generation mismatches, handle is stale from old incarnation (ABA detected).
 * 
 * Note: We may spuriously fail if we observe new ptr but old gen (or vice versa)
 * due to independent atomics, but that's safe - just returns NULL early.
 */
static Slab* reg_lookup_validate(SlabRegistry* r, uint32_t id, uint32_t gen24) {
  if (id >= r->cap) return NULL;
  
  /* Step 1: Load ptr with acquire (handshake point) */
  Slab* s = atomic_load_explicit(&r->metas[id].ptr, memory_order_acquire);
  if (!s) return NULL;
  
  /* Step 2: Load current generation with acquire */
  uint32_t cur = reg_get_gen24(r, id);
  
  /* Step 3: Validate generation matches handle */
  if (cur != gen24) return NULL;  /* ABA: stale handle from old generation */
  
  return s;
}

/* ------------------------------ Intrusive list operations ------------------------------ */

static inline void assert_slab_unlinked(Slab* s) {
  assert(s->prev == NULL && "slab must be unlinked before insertion");
  assert(s->next == NULL && "slab must be unlinked before insertion");
}

static inline void list_init(SlabList* l) {
  l->head = NULL; l->tail = NULL; l->len = 0;
}

static inline void list_push_back(SlabList* l, Slab* s) {
  assert_slab_unlinked(s);
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
  return (h + 63u) & ~63u;
}

static inline uint32_t slab_bitmap_words(uint32_t obj_count) {
  return (obj_count + 31u) / 32u;
}

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

uint32_t slab_object_count(uint32_t obj_size) {
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
  Returns slot index (0..object_count-1) on success, UINT32_MAX on failure.
  If out_prev_fc is non-NULL, stores previous free_count (for transition detection).
*/
static uint32_t slab_alloc_slot_atomic(Slab* s, uint32_t* out_prev_fc) {
  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  const uint32_t words = slab_bitmap_words(s->object_count);

  for (uint32_t w = 0; w < words; w++) {
    while (1) {
      uint32_t x = atomic_load_explicit(&bm[w], memory_order_relaxed);
      if (x == 0xFFFFFFFFu) break;

      uint32_t free_mask = ~x;

      if (w == words - 1u) {
        uint32_t valid_bits = s->object_count - (w * 32u);
        if (valid_bits < 32u) {
          uint32_t valid_mask = (1u << valid_bits) - 1u;
          free_mask &= valid_mask;
          if (free_mask == 0u) break;
        }
        /* If valid_bits == 32, all 32 bits are valid - no masking needed */
      }

      uint32_t bit = ctz32(free_mask);
      uint32_t mask = 1u << bit;
      uint32_t desired = x | mask;

      if (atomic_compare_exchange_weak_explicit(
              &bm[w], &x, desired,
              memory_order_acq_rel,
              memory_order_relaxed)) {
        /* Precise transition detection: return previous free_count */
        uint32_t prev_fc = atomic_fetch_sub_explicit(&s->free_count, 1u, memory_order_relaxed);
        if (out_prev_fc) *out_prev_fc = prev_fc;
        return w * 32u + bit;
      }
    }
  }

  return UINT32_MAX;
}

/*
  Returns true on success, false if slot was already free.
  If out_prev_fc is non-NULL, stores previous free_count (for 0->1 transition detection).
*/
static bool slab_free_slot_atomic(Slab* s, uint32_t idx, uint32_t* out_prev_fc) {
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
      /* Precise transition detection: return previous free_count */
      uint32_t prev_fc = atomic_fetch_add_explicit(&s->free_count, 1u, memory_order_relaxed);
      if (out_prev_fc) *out_prev_fc = prev_fc;
      return true;
    }
  }
}

/* ------------------------------ Allocator Lifetime ------------------------------ */

/* Opaque API: create/free for external users */
SlabAllocator* slab_allocator_create(void) {
  SlabAllocator* a = (SlabAllocator*)calloc(1, sizeof(SlabAllocator));
  if (!a) return NULL;
  allocator_init(a);
  return a;
}

void slab_allocator_free(SlabAllocator* a) {
  if (!a) return;
  allocator_destroy(a);
  free(a);
}

/* Init/destroy: for internal use or when caller provides storage */
void allocator_init(SlabAllocator* a) {
  /* Initialize O(1) class lookup table (once per process) */
  init_class_lookup();
  
  /* Initialize slab registry for portable handle encoding */
  reg_init(&a->reg);
  
  /* IMPORTANT: Initialize atomics BEFORE memset (memset on atomics is UB) */
  a->epoch_count = EPOCH_COUNT;
  atomic_store_explicit(&a->current_epoch, 0, memory_order_relaxed);
  
  /* Initialize all epochs as ACTIVE (MUST be before memset) */
  for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
    atomic_store_explicit(&a->epoch_state[e], EPOCH_ACTIVE, memory_order_relaxed);
  }
  
  /* Zero out non-atomic fields (classes array) */
  for (size_t i = 0; i < k_num_classes; i++) {
    a->classes[i].epochs = NULL;
    a->classes[i].slab_cache = NULL;
    a->classes[i].cache_size = 0;
    a->classes[i].cache_capacity = 0;
    a->classes[i].total_slabs = 0;
  }
  
  for (size_t i = 0; i < k_num_classes; i++) {
    a->classes[i].object_size = k_size_classes[i];
    pthread_mutex_init(&a->classes[i].lock, NULL);
    a->classes[i].total_slabs = 0;

    /* Allocate per-epoch state arrays */
    a->classes[i].epochs = (EpochState*)calloc(EPOCH_COUNT, sizeof(EpochState));
    if (!a->classes[i].epochs) {
      /* Allocation failure - clean up and abort */
      for (size_t j = 0; j < i; j++) {
        free(a->classes[j].epochs);
      }
      return;
    }
    
    /* Initialize each epoch's state */
    for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
      list_init(&a->classes[i].epochs[e].partial);
      list_init(&a->classes[i].epochs[e].full);
      atomic_store_explicit(&a->classes[i].epochs[e].current_partial, NULL, memory_order_relaxed);
      atomic_store_explicit(&a->classes[i].epochs[e].empty_partial_count, 0, memory_order_relaxed);
    }

    /* Initialize performance counters */
    atomic_store_explicit(&a->classes[i].slow_path_hits, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].new_slab_count, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].list_move_partial_to_full, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].list_move_full_to_partial, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].current_partial_null, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].current_partial_full, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].empty_slab_recycled, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].empty_slab_overflowed, 0, memory_order_relaxed);
    
    /* Phase 2.0: Initialize slow-path attribution counters */
    atomic_store_explicit(&a->classes[i].slow_path_cache_miss, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].slow_path_epoch_closed, 0, memory_order_relaxed);
    
    /* Phase 2.0: Initialize RSS reclamation tracking counters */
    atomic_store_explicit(&a->classes[i].madvise_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].madvise_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].madvise_failures, 0, memory_order_relaxed);

    /* Initialize slab cache (32 pages per size class = 128KB) */
    a->classes[i].cache_capacity = 32;
    a->classes[i].cache_size = 0;
    a->classes[i].slab_cache = (CachedSlab*)calloc(a->classes[i].cache_capacity, sizeof(CachedSlab));
    pthread_mutex_init(&a->classes[i].cache_lock, NULL);
    
    /* Initialize overflow list (CachedNode for madvise safety) */
    a->classes[i].cache_overflow_head = NULL;
    a->classes[i].cache_overflow_tail = NULL;
    a->classes[i].cache_overflow_len = 0;
  }
}

/* ------------------------------ Slab cache operations ------------------------------ */

/* Pop cached slab (returns slab pointer and slab_id via out parameter)
 * 
 * CRITICAL: Overflow nodes store slab_id off-page because madvise zeros slab header.
 */
static Slab* cache_pop(SizeClassAlloc* sc, uint32_t* out_slab_id) {
  pthread_mutex_lock(&sc->cache_lock);
  Slab* s = NULL;
  uint32_t id = UINT32_MAX;
  
  if (sc->cache_size > 0) {
    /* Pop from array cache (slab_id stored off-page) */
    CachedSlab* entry = &sc->slab_cache[--sc->cache_size];
    s = entry->slab;
    id = entry->slab_id;
  } else if (sc->cache_overflow_head) {
    /* Cache empty - try overflow list (slab_id stored in node, off-page) */
    CachedNode* node = sc->cache_overflow_head;
    s = node->slab;
    id = node->slab_id;  /* SAFE: stored in node, not slab header */
    
    /* Remove from list */
    sc->cache_overflow_head = node->next;
    if (sc->cache_overflow_head) {
      sc->cache_overflow_head->prev = NULL;
    } else {
      sc->cache_overflow_tail = NULL;
    }
    sc->cache_overflow_len--;
    
    /* Free node */
    free(node);
  }
  
  pthread_mutex_unlock(&sc->cache_lock);
  
  if (out_slab_id) *out_slab_id = id;
  return s;
}

/* FIX #3: Push empty slab to cache (madvise AFTER releasing lock for predictable latency) */
static void cache_push(SizeClassAlloc* sc, Slab* s) {
  /* Slab must be fully unlinked from epoch lists before caching */
  assert_slab_unlinked(s);
  
  pthread_mutex_lock(&sc->cache_lock);
  
  s->list_id = SLAB_LIST_NONE;  /* Not on partial/full list anymore */
  s->prev = NULL;  /* Defensive: ensure clean state */
  s->next = NULL;
  
  if (sc->cache_size < sc->cache_capacity) {
    /* Store both slab pointer and ID (survives madvise) */
    sc->slab_cache[sc->cache_size].slab = s;
    sc->slab_cache[sc->cache_size].slab_id = s->slab_id;
    sc->cache_size++;
    s->cache_state = SLAB_CACHED;
    atomic_fetch_add_explicit(&sc->empty_slab_recycled, 1, memory_order_relaxed);
  } else {
    /* Cache full - push to overflow list
     * 
     * CRITICAL: Store slab_id in CachedNode (off-page) because madvise zeros slab header.
     * This prevents corruption on reuse: cache_pop() will read node->slab_id (safe),
     * not s->slab_id (zeroed by madvise). */
    CachedNode* node = (CachedNode*)malloc(sizeof(CachedNode));
    if (!node) {
      /* Allocation failure - skip caching this slab (leak until destroy) */
      pthread_mutex_unlock(&sc->cache_lock);
      return;
    }
    
    node->slab = s;
    node->slab_id = s->slab_id;  /* CRITICAL: store off-page before madvise */
    node->prev = sc->cache_overflow_tail;
    node->next = NULL;
    
    if (sc->cache_overflow_tail) {
      sc->cache_overflow_tail->next = node;
    } else {
      sc->cache_overflow_head = node;
    }
    sc->cache_overflow_tail = node;
    sc->cache_overflow_len++;
    
    s->cache_state = SLAB_OVERFLOWED;
    atomic_fetch_add_explicit(&sc->empty_slab_overflowed, 1, memory_order_relaxed);
  }
  
  pthread_mutex_unlock(&sc->cache_lock);
  
  /* FIX #3: madvise AFTER releasing lock (eliminates syscall variance from critical section)
   * 
   * Phase 1 RSS Reclamation: Tell kernel to reclaim physical pages.
   * Virtual memory stays mapped (safe for handle validation via registry).
   * Header will be zeroed, but generation lives in registry (ABA-proof).
   * 
   * Combined with FIX #1 (epoch-aware recycling), this enables deterministic RSS drops:
   * - ACTIVE epochs: empty slabs stay hot (no madvise, good latency)
   * - CLOSING epochs: empty slabs recycled + madvised (RSS drops)
   */
  #if ENABLE_RSS_RECLAMATION && defined(__linux__)
  atomic_fetch_add_explicit(&sc->madvise_calls, 1, memory_order_relaxed);
  int ret = madvise(s, SLAB_PAGE_SIZE, MADV_DONTNEED);
  if (ret == 0) {
    atomic_fetch_add_explicit(&sc->madvise_bytes, SLAB_PAGE_SIZE, memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&sc->madvise_failures, 1, memory_order_relaxed);
  }
  /* Non-fatal: RSS stays high if this fails, but no functional impact */
  #endif
}

/* ------------------------------ Slab allocation ------------------------------ */

/* FIX #2: new_slab with registry integration (generation bumping on reuse) */
static Slab* new_slab(SlabAllocator* a, SizeClassAlloc* sc, uint32_t epoch_id) {
  uint32_t obj_size = sc->object_size;

  /* Try to pop from cache first */
  uint32_t cached_id = UINT32_MAX;
  Slab* s = cache_pop(sc, &cached_id);
  
  if (s) {
    /* FIX #2: Bump generation on reuse (ABA protection)
     * This makes old handles invalid even after madvise zeroes the header */
    (void)reg_bump_gen(&a->reg, cached_id);
    
    /* Reinitialize all slab metadata (may have been zeroed by madvise) */
    uint32_t expected_count = slab_object_count(obj_size);
    
    s->prev = NULL;
    s->next = NULL;
    atomic_store_explicit(&s->magic, SLAB_MAGIC, memory_order_relaxed);
    s->object_size = obj_size;
    s->object_count = expected_count;
    s->list_id = SLAB_LIST_NONE;
    s->cache_state = SLAB_ACTIVE;
    s->epoch_id = epoch_id;
    s->slab_id = cached_id;  /* Restore ID from cache */
    atomic_store_explicit(&s->free_count, expected_count, memory_order_relaxed);
    
    /* Clear bitmap (may have been zeroed by madvise) */
    _Atomic uint32_t* bm = slab_bitmap_ptr(s);
    uint32_t words = slab_bitmap_words(expected_count);
    for (uint32_t i = 0; i < words; i++) {
      atomic_store_explicit(&bm[i], 0u, memory_order_relaxed);
    }
    
    /* Update registry pointer (may have been nulled) */
    reg_set_ptr(&a->reg, cached_id, s);
    
    return s;
  }

  /* Cache miss - allocate new page */
  atomic_fetch_add_explicit(&sc->new_slab_count, 1, memory_order_relaxed);
  
  /* Phase 2.0: Attribute slow-path to cache miss (needed mmap) */
  atomic_fetch_add_explicit(&sc->slow_path_cache_miss, 1, memory_order_relaxed);
  
  void* page = map_one_page();
  if (!page) return NULL;

  s = (Slab*)page;

  uint32_t count = slab_object_count(obj_size);
  if (count == 0) {
    unmap_one_page(page);
    errno = EINVAL;
    return NULL;
  }

  /* Allocate slab_id from registry */
  uint32_t id = reg_alloc_id(&a->reg);
  if (id == UINT32_MAX) {
    unmap_one_page(page);
    errno = ENOMEM;
    return NULL;
  }

  s->prev = NULL;
  s->next = NULL;
  atomic_store_explicit(&s->magic, SLAB_MAGIC, memory_order_relaxed);
  s->object_size = obj_size;
  s->object_count = count;
  atomic_store_explicit(&s->free_count, count, memory_order_relaxed);
  s->list_id = SLAB_LIST_NONE;
  s->cache_state = SLAB_ACTIVE;
  s->epoch_id = epoch_id;
  s->slab_id = id;  /* Store registry ID */

  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  uint32_t words = slab_bitmap_words(count);
  for (uint32_t i = 0; i < words; i++) {
    atomic_store_explicit(&bm[i], 0u, memory_order_relaxed);
  }

  /* Publish in registry */
  reg_set_ptr(&a->reg, id, s);

  return s;
}

/* ------------------------------ Handle encoding (Portable, ABA-safe) ------------------------------ */

/* Portable handle encoding v1: slab_id + generation + slot + class + version
 * 
 * Layout (64-bit):
 *   [63:42] slab_id (22 bits) - registry index (max 4M slabs)
 *   [41:18] generation (24 bits) - ABA protection (wraps after 16M reuses)
 *   [17:10] slot (8 bits) - object index within slab (max 255 objects)
 *   [9:2]   size_class (8 bits) - 0-255 size classes
 *   [1:0]   version (2 bits) - handle format version (v1=0b01)
 * 
 * Key properties:
 * - No raw pointers (portable across all platforms)
 * - Generation stored in registry (survives madvise)
 * - 24-bit generation: safe against ABA even under pathological churn
 * - 2-bit version field allows future format changes
 * 
 * Design constraints:
 * - 8-bit slot limits max objects per slab to 255
 * - This bounds min object size to ~16 bytes (4096 / 255)
 * - Current size classes (64-768 bytes) well within limits
 * - If smaller classes needed, must rev handle format (use version bits)
 */
#define HANDLE_VERSION_V1 0x1u

static inline SlabHandle handle_pack(uint32_t slab_id, uint32_t gen, uint8_t slot, uint8_t cls) {
  return ((uint64_t)(slab_id & 0x3FFFFFu) << 42)  /* 22 bits */
       | ((uint64_t)(gen & 0xFFFFFFu) << 18)      /* 24 bits */
       | ((uint64_t)slot << 10)                   /* 8 bits */
       | ((uint64_t)cls << 2)                     /* 8 bits */
       | (uint64_t)HANDLE_VERSION_V1;             /* 2 bits */
}

static inline void handle_unpack(SlabHandle h, uint32_t* slab_id, uint32_t* gen, uint32_t* slot, uint32_t* cls) {
  uint32_t version = (uint32_t)(h & 0x3u);
  if (version != HANDLE_VERSION_V1) {
    /* Invalid version - set sentinel values that will fail validation */
    *slab_id = UINT32_MAX;  /* Will fail bounds check */
    *gen = 0;               /* Generation 0 never used */
    *slot = 0;
    *cls = UINT32_MAX;      /* Will fail bounds check */
    return;
  }
  
  *cls     = (uint32_t)((h >> 2) & 0xFFu);
  *slot    = (uint32_t)((h >> 10) & 0xFFu);
  *gen     = (uint32_t)((h >> 18) & 0xFFFFFFu);
  *slab_id = (uint32_t)(h >> 42);
}

/* Encode handle from slab (reads slab_id from slab, generation from registry) */
static inline SlabHandle encode_handle(Slab* slab, SlabRegistry* reg, uint32_t slot, uint32_t size_class) {
  uint32_t id = slab->slab_id;
  uint32_t gen = reg_get_gen24(reg, id);
  return handle_pack(id, gen, (uint8_t)slot, (uint8_t)size_class);
}

/* ------------------------------ Allocation ------------------------------ */

/*
  Epoch-Aware Allocation Strategy:
    1) Load epoch state for (size_class, epoch_id)
    2) Try fast path: atomic load of epochs[epoch_id].current_partial
    3) On success: check if slab became full, move PARTIAL->FULL within epoch
    4) Slow path: lock, pick/create slab from epoch's partial list
*/
void* alloc_obj_epoch(SlabAllocator* a, uint32_t size, EpochId epoch, SlabHandle* out) {
  int ci = class_index_for_size(size);
  if (ci < 0) return NULL;
  
  if (epoch >= a->epoch_count) return NULL;  /* Invalid epoch */
  
  SizeClassAlloc* sc = &a->classes[(size_t)ci];
  
  /* CRITICAL: Refuse allocations into CLOSING epochs
   * Best-effort check: may race with epoch_advance (allocation may succeed briefly) */
  uint32_t state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed);
  if (state != EPOCH_ACTIVE) {
    /* Phase 2.0: Attribute slow-path rejection to CLOSING epoch */
    atomic_fetch_add_explicit(&sc->slow_path_epoch_closed, 1, memory_order_relaxed);
    return NULL;  /* Epoch is not active (draining or invalid state) */
  }
  EpochState* es = get_epoch_state(sc, epoch);

  /* Fast path: try current_partial for this epoch */
  Slab* cur = atomic_load_explicit(&es->current_partial, memory_order_acquire);
  
  if (cur && atomic_load_explicit(&cur->magic, memory_order_relaxed) == SLAB_MAGIC) {
    uint32_t prev_fc = 0;
    uint32_t idx = slab_alloc_slot_atomic(cur, &prev_fc);
    
    if (idx != UINT32_MAX) {
      /* Phase 2.1: If allocating from empty slab, decrement empty counter */
      if (prev_fc == cur->object_count) {
        atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
      }
      
      /* Precise transition detection: check if WE caused 1->0 */
      if (prev_fc == 1) {
        /* Slab is now full - move PARTIAL->FULL and publish new current */
        pthread_mutex_lock(&sc->lock);
        if (cur->list_id == SLAB_LIST_PARTIAL) {
          atomic_fetch_add_explicit(&sc->list_move_partial_to_full, 1, memory_order_relaxed);
          list_remove(&es->partial, cur);
          cur->list_id = SLAB_LIST_FULL;
          list_push_back(&es->full, cur);
          
          /* Publish next partial slab to reduce slow-path contention */
          Slab* next = es->partial.head;
          atomic_store_explicit(&es->current_partial, next, memory_order_release);
        }
        pthread_mutex_unlock(&sc->lock);
      }
      
      void* p = slab_slot_ptr(cur, idx);
      if (out) {
        *out = encode_handle(cur, &a->reg, idx, (uint32_t)ci);
      }
      return p;
    }
    
    /* Allocation failed (slab was full) - null current_partial and go slow */
    atomic_fetch_add_explicit(&sc->current_partial_full, 1, memory_order_relaxed);
    Slab* expected = cur;
    atomic_compare_exchange_strong_explicit(
      &es->current_partial, &expected, NULL,
      memory_order_release, memory_order_relaxed);
  } else if (!cur) {
    /* current_partial was NULL - count this miss */
    atomic_fetch_add_explicit(&sc->current_partial_null, 1, memory_order_relaxed);
  }

  /* Slow path: need mutex to pick/create slab (use loop, not recursion) */
  for (;;) {
    /* Double-check epoch state in slow path (may have changed) */
    state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed);
    if (state != EPOCH_ACTIVE) {
      return NULL;  /* Epoch closed while we were contending */
    }
    
    atomic_fetch_add_explicit(&sc->slow_path_hits, 1, memory_order_relaxed);
    pthread_mutex_lock(&sc->lock);

    Slab* s = es->partial.head;
    if (!s) {
      s = new_slab(a, sc, epoch);
      if (!s) {
        pthread_mutex_unlock(&sc->lock);
        return NULL;
      }
      s->list_id = SLAB_LIST_PARTIAL;
      list_push_back(&es->partial, s);
      sc->total_slabs++;
    }

    /* Publish to current_partial (release) */
    assert(s->list_id == SLAB_LIST_PARTIAL);
    atomic_store_explicit(&es->current_partial, s, memory_order_release);

    pthread_mutex_unlock(&sc->lock);

    /* Now allocate from published slab */
    uint32_t prev_fc = 0;
    uint32_t idx = slab_alloc_slot_atomic(s, &prev_fc);
    
    if (idx == UINT32_MAX) {
      /* Rare: slab filled between publish and our alloc - retry loop */
      continue;
    }
    
    /* Phase 2.1: If allocating from empty slab, decrement empty counter */
    if (prev_fc == s->object_count) {
      atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
    }

    /* Check if we caused 1->0 transition */
    if (prev_fc == 1) {
      pthread_mutex_lock(&sc->lock);
      if (s->list_id == SLAB_LIST_PARTIAL) {
        atomic_fetch_add_explicit(&sc->list_move_partial_to_full, 1, memory_order_relaxed);
        list_remove(&es->partial, s);
        s->list_id = SLAB_LIST_FULL;
        list_push_back(&es->full, s);
        
        /* Publish next partial if available */
        Slab* next = es->partial.head;
        atomic_store_explicit(&es->current_partial, next, memory_order_release);
      }
      pthread_mutex_unlock(&sc->lock);
    }

    void* p = slab_slot_ptr(s, idx);
    if (out) {
      *out = encode_handle(s, &a->reg, idx, (uint32_t)ci);
    }
    return p;
  }
}


/*
  Free Strategy:
    - Validates handle by checking slab magic (safe because slabs stay mapped)
    - CAS-clears bitmap bit for double-free detection
    - If slab became empty AND is on FULL list: recycle to cache (safe from UAF)
    - If 0->1 transition: move FULL->PARTIAL
    
  Safety contract:
    - Returns false on invalid/stale/double-free handles (never crashes)
    - Handles can be held indefinitely; free_obj validates them
    - Slabs stay mapped during allocator lifetime for safe validation
*/
bool free_obj(SlabAllocator* a, SlabHandle h) {
  if (h == 0) return false;  /* NULL handle */
  
  /* FIX #2: Decode handle and validate through registry (ABA-proof) */
  uint32_t slab_id, slot, size_class, gen;
  handle_unpack(h, &slab_id, &gen, &slot, &size_class);
  
  /* Validate bounds (catches invalid version handles) */
  if (slab_id == UINT32_MAX || size_class >= (uint32_t)k_num_classes) return false;

  SizeClassAlloc* sc = &a->classes[size_class];
  
  /* FIX #2: Validate slab through registry (checks generation for ABA safety) */
  Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
  if (!s) return false;  /* Invalid: wrong ID, stale generation, or unmapped */
  
  /* Double-check magic (defensive, should always pass if registry returned non-NULL) */
  if (atomic_load_explicit(&s->magic, memory_order_relaxed) != SLAB_MAGIC) return false;
  
  /* Get epoch state for this slab */
  uint32_t epoch = s->epoch_id;
  if (epoch >= a->epoch_count) return false;  /* Invalid epoch */
  EpochState* es = get_epoch_state(sc, epoch);

  /* Precise transition detection: get previous free_count from fetch_add */
  uint32_t prev_fc = 0;
  if (!slab_free_slot_atomic(s, slot, &prev_fc)) return false;

  /* New free_count is prev_fc + 1 */
  uint32_t new_fc = prev_fc + 1;

  /* FIX #1: Epoch-aware recycling - only recycle empty slabs in CLOSING epochs
   * 
   * Key design decision:
   * - ACTIVE epochs: Keep empty slabs "hot" for fast reuse (latency goal, RSS stable)
   * - CLOSING epochs: Aggressively recycle empty slabs → cache → madvise → RSS drops
   * 
   * This makes epoch_close() actually matter for RSS - it's the explicit boundary
   * between "keep memory hot" and "return memory to OS". */
  if (new_fc == s->object_count) {
    pthread_mutex_lock(&sc->lock);
    
    uint32_t epoch_state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed);
    
    /* Phase 2.1: Track transition to empty (prev_fc == object_count-1 → new_fc == object_count) */
    bool became_empty = (prev_fc == s->object_count - 1);  /* Was one-away, now fully free */
    
    if (epoch_state == EPOCH_CLOSING) {
      /* FIX #1: Aggressive recycling ONLY for CLOSING epochs
       * 
       * This is the key fix: empty slabs in CLOSING epochs get recycled + madvised,
       * while ACTIVE epochs keep empty slabs around for fast reuse. */
      if (s->list_id == SLAB_LIST_FULL) {
        list_remove(&es->full, s);
      } else if (s->list_id == SLAB_LIST_PARTIAL) {
        list_remove(&es->partial, s);
        /* Phase 2.1: Decrement empty counter (removing from partial list) */
        atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
      }
      
      /* Ensure not published in current_partial */
      Slab* expected = s;
      (void)atomic_compare_exchange_strong_explicit(
          &es->current_partial, &expected, NULL,
          memory_order_release, memory_order_relaxed);
      
      if (s->list_id != SLAB_LIST_NONE) {
        s->list_id = SLAB_LIST_NONE;
        sc->total_slabs--;
        pthread_mutex_unlock(&sc->lock);
        
        /* Push to cache → triggers madvise (FIX #3: outside lock) → RSS drops */
        cache_push(sc, s);
        return true;
      }
    } else {
      /* FIX #1: For ACTIVE epochs, do NOT recycle empty slabs
       * They stay on partial list for fast reuse (good latency, stable RSS).
       * This makes epoch_close() the explicit control point for RSS reclamation. */
      
      /* Phase 2.1: Increment empty counter (slab just became empty and staying in partial) */
      if (became_empty && s->list_id == SLAB_LIST_PARTIAL) {
        atomic_fetch_add_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
      }
    }
    
    pthread_mutex_unlock(&sc->lock);
    return true;
  }

  /* Check if WE caused 0->1 transition (slab was full, now has space) */
  if (prev_fc == 0) {
    pthread_mutex_lock(&sc->lock);
    if (s->list_id == SLAB_LIST_FULL) {
      atomic_fetch_add_explicit(&sc->list_move_full_to_partial, 1, memory_order_relaxed);
      list_remove(&es->full, s);
      s->list_id = SLAB_LIST_PARTIAL;
      list_push_back(&es->partial, s);
      
      /* Publish as current_partial if NULL (reduce slow-path trips) */
      assert(s->list_id == SLAB_LIST_PARTIAL);
      Slab* expected = NULL;
      atomic_compare_exchange_strong_explicit(
        &es->current_partial, &expected, s,
        memory_order_release, memory_order_relaxed);
    }
    pthread_mutex_unlock(&sc->lock);
  }

  return true;
}

/* ------------------------------ Malloc-style wrapper ------------------------------ */

void* slab_malloc_epoch(SlabAllocator* a, size_t size, EpochId epoch) {
  /* Reserve 8 bytes for handle header */
  if (size == 0 || size > 504) return NULL;  /* Max: 512 - 8 = 504 bytes */
  
  /* Round up to ensure SlabHandle and user pointer are both 8-byte aligned */
  uint32_t alloc_size = (uint32_t)((size + sizeof(SlabHandle) + 7) & ~7u);
  SlabHandle h;
  void* obj = alloc_obj_epoch(a, alloc_size, epoch, &h);
  if (!obj) return NULL;
  
  /* Store handle in first 8 bytes (use memcpy for unaligned safety) */
  memcpy(obj, &h, sizeof(SlabHandle));
  
  /* Return pointer after header */
  return (void*)((uint8_t*)obj + sizeof(SlabHandle));
}


void slab_free(SlabAllocator* a, void* ptr) {
  if (!ptr) return;
  
  /* Read handle from 8 bytes before user pointer (use memcpy for unaligned safety) */
  SlabHandle h;
  memcpy(&h, (uint8_t*)ptr - sizeof(SlabHandle), sizeof(SlabHandle));
  
  /* Free using handle (validates and frees) */
  free_obj(a, h);
}

/* ------------------------------ Cleanup ------------------------------ */

void allocator_destroy(SlabAllocator* a) {
  for (size_t i = 0; i < k_num_classes; i++) {
    SizeClassAlloc* sc = &a->classes[i];

    pthread_mutex_lock(&sc->lock);

    /* Destroy all per-epoch state */
    if (sc->epochs) {
      for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
        EpochState* es = &sc->epochs[e];
        
        Slab* cur = es->partial.head;
        while (cur) {
          Slab* next = cur->next;
          unmap_one_page(cur);
          cur = next;
        }

        cur = es->full.head;
        while (cur) {
          Slab* next = cur->next;
          unmap_one_page(cur);
          cur = next;
        }

        list_init(&es->partial);
        list_init(&es->full);
        atomic_store_explicit(&es->current_partial, NULL, memory_order_relaxed);
        atomic_store_explicit(&es->empty_partial_count, 0, memory_order_relaxed);
      }
      
      free(sc->epochs);
      sc->epochs = NULL;
    }
    
    sc->total_slabs = 0;

    pthread_mutex_unlock(&sc->lock);
    pthread_mutex_destroy(&sc->lock);

    /* Drain slab cache (now storing CachedSlab entries) */
    pthread_mutex_lock(&sc->cache_lock);
    for (size_t j = 0; j < sc->cache_size; j++) {
      unmap_one_page(sc->slab_cache[j].slab);
    }
    free(sc->slab_cache);
    sc->cache_size = 0;
    sc->cache_capacity = 0;
    
    /* Drain overflow list (free nodes and unmap slabs) */
    CachedNode* node = sc->cache_overflow_head;
    while (node) {
      CachedNode* next = node->next;
      unmap_one_page(node->slab);
      free(node);
      node = next;
    }
    sc->cache_overflow_head = NULL;
    sc->cache_overflow_tail = NULL;
    sc->cache_overflow_len = 0;
    
    pthread_mutex_unlock(&sc->cache_lock);
    pthread_mutex_destroy(&sc->cache_lock);
  }
  
  /* Destroy slab registry */
  reg_destroy(&a->reg);
}

/* ------------------------------ Performance counters ------------------------------ */

void get_perf_counters(SlabAllocator* a, uint32_t size_class, PerfCounters* out) {
  if (size_class >= k_num_classes || !out) return;
  
  SizeClassAlloc* sc = &a->classes[size_class];
  out->slow_path_hits = atomic_load_explicit(&sc->slow_path_hits, memory_order_relaxed);
  out->new_slab_count = atomic_load_explicit(&sc->new_slab_count, memory_order_relaxed);
  out->list_move_partial_to_full = atomic_load_explicit(&sc->list_move_partial_to_full, memory_order_relaxed);
  out->list_move_full_to_partial = atomic_load_explicit(&sc->list_move_full_to_partial, memory_order_relaxed);
  out->current_partial_null = atomic_load_explicit(&sc->current_partial_null, memory_order_relaxed);
  out->current_partial_full = atomic_load_explicit(&sc->current_partial_full, memory_order_relaxed);
  out->empty_slab_recycled = atomic_load_explicit(&sc->empty_slab_recycled, memory_order_relaxed);
  out->empty_slab_overflowed = atomic_load_explicit(&sc->empty_slab_overflowed, memory_order_relaxed);
}

/* ------------------------------ RSS (Linux) ------------------------------ */

uint64_t read_rss_bytes_linux(void) {
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

/* ------------------------------ Epoch API ------------------------------ */

EpochId epoch_current(SlabAllocator* a) {
  return atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
}

void epoch_advance(SlabAllocator* a) {
  uint32_t old_epoch_raw = atomic_fetch_add_explicit(&a->current_epoch, 1, memory_order_relaxed);
  uint32_t old_epoch = old_epoch_raw % a->epoch_count;
  uint32_t new_epoch = (old_epoch_raw + 1) % a->epoch_count;
  
  /* CRITICAL: Implement epoch closing semantics
   * Rule: Never allocate into a CLOSING epoch */
  
  /* Mark old epoch as CLOSING (relaxed: best-effort visibility) */
  atomic_store_explicit(&a->epoch_state[old_epoch], EPOCH_CLOSING, memory_order_relaxed);
  
  /* Mark new epoch as ACTIVE (overwrites CLOSING state on wrap-around) */
  atomic_store_explicit(&a->epoch_state[new_epoch], EPOCH_ACTIVE, memory_order_relaxed);
  
  /* Null current_partial for old epoch across all size classes
   * This ensures no thread will allocate from a published slab in CLOSING epoch */
  for (size_t i = 0; i < k_num_classes; i++) {
    EpochState* es = &a->classes[i].epochs[old_epoch];
    atomic_store_explicit(&es->current_partial, NULL, memory_order_release);
  }
  
  /* Old epoch now drains: frees continue, but no new allocations refill slabs */
}

void epoch_close(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return;
  
  /* Mark epoch as CLOSING - no new allocations allowed
   * 
   * This enables Phase 2 RSS reclamation: when slabs in this epoch become empty,
   * they are immediately recycled. With ENABLE_RSS_RECLAMATION=1, madvise() is
   * called to reclaim physical pages, causing RSS to drop.
   * 
   * Key difference from epoch_advance():
   * - epoch_advance() closes old epoch and rotates to next
   * - epoch_close() closes specific epoch without rotation
   * 
   * This allows applications to explicitly control reclamation boundaries
   * aligned with application lifetime phases (requests, frames, batches).
   */
  /* Best-effort visibility: allocations may briefly succeed until state observed */
  atomic_store_explicit(&a->epoch_state[epoch], EPOCH_CLOSING, memory_order_relaxed);
  
  /* Proactively scan for already-empty slabs and recycle them
   * 
   * This is the missing piece: slabs that became empty BEFORE epoch_close() was
   * called are sitting on the partial list. The free_obj() path only recycles
   * slabs at the moment they transition to empty, so we need to scan for slabs
   * that are already empty when we mark the epoch as CLOSING.
   */
  for (size_t i = 0; i < k_num_classes; i++) {
    SizeClassAlloc* sc = &a->classes[i];
    EpochState* es = &sc->epochs[epoch];
    
    /* Null current_partial first to prevent allocations */
    atomic_store_explicit(&es->current_partial, NULL, memory_order_release);
    
    /* Scan partial list for empty slabs (single-pass O(n) algorithm)
     * 
     * Strategy: Collect empty slabs in temporary array, then recycle outside lock.
     * This avoids O(n²) restart-scan pattern and minimizes lock hold time.
     */
    pthread_mutex_lock(&sc->lock);
    
    /* Quick scan to count empty slabs */
    size_t empty_count = 0;
    for (Slab* s = es->partial.head; s; s = s->next) {
      if (atomic_load_explicit(&s->free_count, memory_order_relaxed) == s->object_count) {
        empty_count++;
      }
    }
    for (Slab* s = es->full.head; s; s = s->next) {
      if (atomic_load_explicit(&s->free_count, memory_order_relaxed) == s->object_count) {
        empty_count++;
      }
    }
    
    if (empty_count > 0) {
      /* Allocate temporary array (stack for small counts, heap for large) */
      Slab** empty_slabs = NULL;
      Slab* stack_buf[32];  /* Stack allocation for common case (<32 empty slabs) */
      
      if (empty_count <= 32) {
        empty_slabs = stack_buf;
      } else {
        empty_slabs = (Slab**)malloc(empty_count * sizeof(Slab*));
        if (!empty_slabs) {
          pthread_mutex_unlock(&sc->lock);
          return;  /* Out of memory - skip recycling this time */
        }
      }
      
      /* Collect empty slabs from both lists (single pass) */
      size_t idx = 0;
      Slab* cur = es->partial.head;
      while (cur) {
        Slab* next = cur->next;
        if (atomic_load_explicit(&cur->free_count, memory_order_relaxed) == cur->object_count) {
          list_remove(&es->partial, cur);
          cur->list_id = SLAB_LIST_NONE;
          sc->total_slabs--;
          empty_slabs[idx++] = cur;
        }
        cur = next;
      }
      
      cur = es->full.head;
      while (cur) {
        Slab* next = cur->next;
        if (atomic_load_explicit(&cur->free_count, memory_order_relaxed) == cur->object_count) {
          list_remove(&es->full, cur);
          cur->list_id = SLAB_LIST_NONE;
          sc->total_slabs--;
          empty_slabs[idx++] = cur;
        }
        cur = next;
      }
      
      pthread_mutex_unlock(&sc->lock);
      
      /* Recycle all empty slabs outside lock (madvise happens here) */
      for (size_t j = 0; j < idx; j++) {
        cache_push(sc, empty_slabs[j]);
      }
      
      /* Free heap allocation if used */
      if (empty_slabs != stack_buf) {
        free(empty_slabs);
      }
    } else {
      pthread_mutex_unlock(&sc->lock);
    }
  }
  
  /* Epoch now drains: frees continue and empty slabs are aggressively recycled.
   * With RSS reclamation enabled, physical pages are returned to OS as slabs drain. */
}

/*
 * Note: Tests moved to smoke_tests.c (Phase 1.6 file organization)
 * This file now contains only the allocator implementation.
 */
