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
#include "slab_stats.h"     /* Phase 2.5: For slowpath sampling */
#include "epoch_domain.h"  /* Phase 2.3: For TLS label attribution */
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

/* Macro stringification helper for lock timeout diagnostics */
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/* Lock rank constants (needed for timeout diagnostics even when rank checking is disabled) */
#ifndef LOCK_RANK_REGISTRY
#define LOCK_RANK_REGISTRY       10
#define LOCK_RANK_CACHE          20
#define LOCK_RANK_SIZE_CLASS     30
#define LOCK_RANK_EPOCH_LABEL    40
#define LOCK_RANK_LABEL_REGISTRY 50
#endif

/* Stub macros for lock rank checking (no-op when ENABLE_LOCK_RANK_DEBUG is off) */
#ifndef CHECK_LOCK_RANK
#define CHECK_LOCK_RANK(rank, name, location) ((void)0)
#define RELEASE_LOCK_RANK() ((void)0)
#endif

/* Lock rank debugging - TLS storage (stack-based for nested locks) */
#if ENABLE_LOCK_RANK_DEBUG
_Thread_local LockRankEntry _lock_rank_stack[LOCK_RANK_STACK_MAX];
_Thread_local int _lock_rank_depth = 0;
#endif

/* Phase 2.3: Hot-path label ID lookup for contention attribution */
#ifdef ENABLE_LABEL_CONTENTION
static inline uint8_t current_label_id(SlabAllocator* alloc) {
  epoch_domain_t* d = epoch_domain_current();
  if (!d) return 0;  /* No active domain = ID 0 (unlabeled) */
  return alloc->epoch_meta[d->epoch_id].label_id;
}
#endif

/* Phase 2.2: Tier 0 lock contention probe (HFT-friendly, always-on)
 * Phase 2.3: Extended with optional per-label attribution
 * 
 * Measures contention occurrence without clock syscalls (~1-2 instruction overhead).
 * Answers: "Are threads blocking on this lock?" and "Which label is contending?"
 * 
 * Pattern: Try fast path (trylock), fall back to blocking lock on contention.
 * Cost: ~2ns best case (trylock succeeds), +5ns if ENABLE_LABEL_CONTENTION.
 */
#ifdef ENABLE_LABEL_CONTENTION
#define LOCK_WITH_PROBE(mutex, sc, rank, name) do { \
  CHECK_LOCK_RANK(rank, name, __FILE__ ":" #rank); \
  if (pthread_mutex_trylock(mutex) == 0) { \
    atomic_fetch_add_explicit(&(sc)->lock_fast_acquire, 1, memory_order_relaxed); \
    uint8_t lid = current_label_id((sc)->parent_alloc); \
    atomic_fetch_add_explicit(&(sc)->lock_fast_acquire_by_label[lid], 1, memory_order_relaxed); \
  } else { \
    atomic_fetch_add_explicit(&(sc)->lock_contended, 1, memory_order_relaxed); \
    uint8_t lid = current_label_id((sc)->parent_alloc); \
    atomic_fetch_add_explicit(&(sc)->lock_contended_by_label[lid], 1, memory_order_relaxed); \
    pthread_mutex_lock(mutex); \
  } \
} while (0)
#else
#define LOCK_WITH_PROBE(mutex, sc, rank, name) do { \
  CHECK_LOCK_RANK(rank, name, __FILE__ ":" #rank); \
  if (pthread_mutex_trylock(mutex) == 0) { \
    atomic_fetch_add_explicit(&(sc)->lock_fast_acquire, 1, memory_order_relaxed); \
  } else { \
    atomic_fetch_add_explicit(&(sc)->lock_contended, 1, memory_order_relaxed); \
    struct timespec timeout; \
    clock_gettime(CLOCK_REALTIME, &timeout); \
    timeout.tv_sec += 5; /* 5 second timeout */ \
    int ret = pthread_mutex_timedlock(mutex, &timeout); \
    if (ret == ETIMEDOUT) { \
      fprintf(stderr, "\n*** LOCK TIMEOUT ***\n"); \
      fprintf(stderr, "Thread %lu stuck waiting for lock: %s (rank %d)\n", \
              (unsigned long)pthread_self(), (name), (rank)); \
      fprintf(stderr, "Location: %s\n", __FILE__ ":" STRINGIFY(__LINE__)); \
      fflush(stderr); \
      abort(); \
    } \
  } \
} while (0)
#endif

#define LOCK_WITHOUT_PROBE(mutex, rank, name) do { \
  CHECK_LOCK_RANK(rank, name, __FILE__ ":" #rank); \
  struct timespec timeout; \
  clock_gettime(CLOCK_REALTIME, &timeout); \
  timeout.tv_sec += 5; /* 5 second timeout */ \
  int ret = pthread_mutex_timedlock(mutex, &timeout); \
  if (ret == ETIMEDOUT) { \
    fprintf(stderr, "\n*** LOCK TIMEOUT ***\n"); \
    fprintf(stderr, "Thread %lu stuck waiting for lock: %s (rank %d)\n", \
            (unsigned long)pthread_self(), (name), (rank)); \
    fprintf(stderr, "Location: %s\n", __FILE__ ":" STRINGIFY(__LINE__)); \
    fflush(stderr); \
    abort(); \
  } \
} while (0)

#define UNLOCK_WITH_RANK(mutex) do { \
  pthread_mutex_unlock(mutex); \
  RELEASE_LOCK_RANK(); \
} while (0)

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
static pthread_once_t k_lookup_once = PTHREAD_ONCE_INIT;

/* Phase 2.2+: Adaptive bitmap scanning helpers (HFT-friendly: no clocks, windowed deltas)
 *
 * Adaptive policy uses:
 * - Windowed deltas (not lifetime average) for fast convergence
 * - Allocation-count triggered checks (no clock reads)
 * - TLS-cached offsets (computed once per thread)
 * - Minimum dwell time to prevent flapping
 */

static inline uint32_t mix32(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return (uint32_t)x;
}

static _Thread_local uint32_t tls_scan_offset = UINT32_MAX;

static inline uint32_t get_tls_scan_offset(uint32_t words) {
  if (tls_scan_offset == UINT32_MAX) {
    uint64_t tid = (uint64_t)(uintptr_t)pthread_self();
    uint32_t h = mix32(tid);
    tls_scan_offset = h;
  }
  return (words == 0) ? 0 : (tls_scan_offset % words);
}

static inline void scan_adapt_check(SizeClassAlloc* sc) {
  /* Single-writer guard: cheap, lock-free */
  uint32_t expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &sc->scan_adapt.in_check, &expected, 1,
          memory_order_acquire, memory_order_relaxed)) {
    return; /* Another thread is already running the controller */
  }

  /* Snapshot global counters (monotonic) */
  uint64_t attempts = atomic_load_explicit(&sc->bitmap_alloc_attempts, memory_order_relaxed);
  uint64_t retries  = atomic_load_explicit(&sc->bitmap_alloc_cas_retries, memory_order_relaxed);

  /* Load previous window endpoints */
  uint64_t last_a = atomic_load_explicit(&sc->scan_adapt.last_attempts, memory_order_relaxed);
  uint64_t last_r = atomic_load_explicit(&sc->scan_adapt.last_retries,  memory_order_relaxed);

  uint64_t da = attempts - last_a;
  uint64_t dr = retries  - last_r;

  /* Publish new window endpoints */
  atomic_store_explicit(&sc->scan_adapt.last_attempts, attempts, memory_order_relaxed);
  atomic_store_explicit(&sc->scan_adapt.last_retries,  retries,  memory_order_relaxed);

  /* Observable: this is now real and export-safe */
  atomic_fetch_add_explicit(&sc->scan_adapt.checks, 1, memory_order_relaxed);

  /* Require a minimum window size for stable rate */
  if (da < 100000) {
    atomic_store_explicit(&sc->scan_adapt.in_check, 0, memory_order_release);
    return;
  }

  double rate = (double)dr / (double)da;

  const double ENABLE  = 0.30;
  const double DISABLE = 0.10;

  /* Dwell/hysteresis (single-writer, plain store is fine) */
  if (sc->scan_adapt.dwell_countdown) {
    sc->scan_adapt.dwell_countdown--;
    atomic_store_explicit(&sc->scan_adapt.in_check, 0, memory_order_release);
    return;
  }

  uint32_t mode = atomic_load_explicit(&sc->scan_adapt.mode, memory_order_relaxed);

  if (mode == 0 && rate > ENABLE) {
    atomic_store_explicit(&sc->scan_adapt.mode, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&sc->scan_adapt.switches, 1, memory_order_relaxed);
    sc->scan_adapt.dwell_countdown = 50;

#ifdef DEBUG_SCAN_ADAPT
    fprintf(stderr,
            "[scan_adapt] class=%u mode 0->1 dr=%lu da=%lu rate=%.4f attempts=%lu\n",
            (unsigned)(sc - sc->parent_alloc->classes),
            (unsigned long)dr, (unsigned long)da, rate, (unsigned long)attempts);
#endif

  } else if (mode == 1 && rate < DISABLE) {
    atomic_store_explicit(&sc->scan_adapt.mode, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&sc->scan_adapt.switches, 1, memory_order_relaxed);
    sc->scan_adapt.dwell_countdown = 50;

#ifdef DEBUG_SCAN_ADAPT
    fprintf(stderr,
            "[scan_adapt] class=%u mode 1->0 dr=%lu da=%lu rate=%.4f attempts=%lu\n",
            (unsigned)(sc - sc->parent_alloc->classes),
            (unsigned long)dr, (unsigned long)da, rate, (unsigned long)attempts);
#endif
  }

  atomic_store_explicit(&sc->scan_adapt.in_check, 0, memory_order_release);
}

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

/* Initialize O(1) class lookup table (thread-safe via pthread_once) */
static void init_class_lookup_impl(void) {
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
}

static void init_class_lookup(void) {
  pthread_once(&k_lookup_once, init_class_lookup_impl);
}

/* O(1) class index lookup (deterministic, no branches per class) */
static inline int class_index_for_size(uint32_t sz) {
  if (sz == 0 || sz > MAX_ALLOC_SIZE) return -1;
  return (int)k_class_lookup[sz];
}

/* ------------------------------ Slab Registry (ABA Protection) ------------------------------ */

/* Initialize slab registry.
 *
 * Registry maps slab_id (compact integer) to (slab pointer, generation counter).
 * This enables portable handle encoding without embedding raw pointers.
 *
 * Initial capacity: 1024 entries (allocated on first reg_alloc_id call).
 * Growth strategy: Double on overflow (exponential growth, amortized O(1)).
 */
static void reg_init(SlabRegistry* r) {
  r->metas = NULL;       /* Allocated on demand */
  r->cap = 0;            /* No capacity yet */
  r->free_ids = NULL;    /* Free list (currently unused, reserved for future) */
  r->free_count = 0;
  r->next_id = 0;        /* Bump allocator starts at ID 0 */
  pthread_mutex_init(&r->lock, NULL);
}

/* Destroy slab registry.
 *
 * Frees all registry metadata. Slab pages themselves are freed separately
 * during allocator_destroy() when walking partial/full/cache lists.
 */
static void reg_destroy(SlabRegistry* r) {
  free(r->metas);
  free(r->free_ids);
  pthread_mutex_destroy(&r->lock);
}

/* Allocate a new slab_id from registry.
 *
 * Two-tier allocation:
 * 1. Reuse free IDs (currently unused, infrastructure exists for future)
 * 2. Bump allocate from next_id (simple, fast, no fragmentation)
 *
 * Registry grows dynamically (starts at 1024, doubles on overflow).
 * Returns UINT32_MAX on allocation failure (out of memory for registry growth).
 *
 * Concurrency: Protected by registry lock (cold path, allocation only).
 */
static uint32_t reg_alloc_id(SlabRegistry* r) {
  LOCK_WITHOUT_PROBE(&r->lock, LOCK_RANK_REGISTRY, "registry.lock");
  
  uint32_t id;
  if (r->free_count > 0) {
    /* Try free list first (currently always empty, reserved for future) */
    id = r->free_ids[--r->free_count];
  } else {
    /* Bump allocate: monotonically increasing IDs */
    id = r->next_id++;
  }
  
  /* Check if registry needs to grow to accommodate this ID */
  if (id >= r->cap) {
    /* Exponential growth: start at 1024, double each time.
     * Example: 1024 → 2048 → 4096 → 8192 → ...
     * Max 4M slabs (22-bit slab_id in handle encoding). */
    uint32_t new_cap = r->cap ? r->cap * 2 : 1024;
    while (new_cap <= id) new_cap *= 2;  /* Handle large jumps */
    
    /* Allocate new metadata array */
    SlabMeta* nm = (SlabMeta*)calloc(new_cap, sizeof(SlabMeta));
    if (!nm) {
      UNLOCK_WITH_RANK(&r->lock);
      return UINT32_MAX;  /* Out of memory */
    }
    
    /* Copy existing metadata (preserve generation counters and pointers) */
    for (uint32_t i = 0; i < r->cap; i++) {
      nm[i] = r->metas[i];
    }
    free(r->metas);
    r->metas = nm;
    r->cap = new_cap;
    
    /* Grow free_ids array to match (preserve existing free list) */
    uint32_t* nf = (uint32_t*)calloc(new_cap, sizeof(uint32_t));
    if (!nf) {
      UNLOCK_WITH_RANK(&r->lock);
      return UINT32_MAX;
    }
    for (size_t i = 0; i < r->free_count; i++) {
      nf[i] = r->free_ids[i];
    }
    free(r->free_ids);
    r->free_ids = nf;
  }
  
  /* Initialize metadata for new ID.
   * Generation starts at 1 (0 reserved for NULL handle).
   * Pointer starts NULL (set later via reg_set_ptr). */
  atomic_store_explicit(&r->metas[id].gen, 1u, memory_order_relaxed);
  atomic_store_explicit(&r->metas[id].ptr, NULL, memory_order_relaxed);
  
  UNLOCK_WITH_RANK(&r->lock);
  return id;
}

/* Publish slab pointer in registry.
 *
 * Makes slab findable via handle lookup (used in free_obj validation).
 * Release ordering ensures slab initialization is visible before publication.
 *
 * Called after slab creation (new mmap) or reinitialization (cache hit).
 */
static void reg_set_ptr(SlabRegistry* r, uint32_t id, Slab* s) {
  if (id < r->cap) {
    atomic_store_explicit(&r->metas[id].ptr, s, memory_order_release);
  }
}

/* Bump generation on slab reuse (ABA protection).
 *
 * Increments generation counter to invalidate old handles pointing to this slab.
 * Essential for safety when recycling slabs from cache:
 *
 * Timeline without generation bumping (UNSAFE):
 * 1. Slab A allocated, handle H1 created with gen=1
 * 2. Slab A freed, recycled to cache
 * 3. Slab A reused for new allocation
 * 4. H1 still validates (gen=1 matches) → use-after-free!
 *
 * Timeline with generation bumping (SAFE):
 * 1. Slab A allocated, handle H1 created with gen=1
 * 2. Slab A freed, recycled to cache
 * 3. Slab A reused, generation bumped to gen=2
 * 4. H1 fails validation (gen=1 ≠ gen=2) → rejected safely
 *
 * Returns new 24-bit generation (truncated for handle encoding).
 *
 * Memory ordering: Relaxed is sufficient because validation safety comes from
 * the ptr (release/acquire) handshake, not gen ordering.
 */
static uint32_t reg_bump_gen(SlabRegistry* r, uint32_t id) {
  if (id >= r->cap) return 0;
  
  /* Atomically increment and get new value */
  uint32_t g = atomic_fetch_add_explicit(&r->metas[id].gen, 1u, memory_order_relaxed) + 1u;
  
  /* Truncate to 24 bits for handle encoding.
   * After 16M reuses (2^24), generation wraps to 0. We skip 0 and use 1 instead
   * since 0 is reserved for NULL handle. */
  uint32_t g24 = g & 0xFFFFFFu;
  if (g24 == 0) g24 = 1;
  return g24;
}

/* Get current generation for handle encoding.
 *
 * Returns 24-bit generation value to embed in handle.
 * Acquire ordering ensures we see the generation that was current when ptr was published.
 */
static uint32_t reg_get_gen24(SlabRegistry* r, uint32_t id) {
  if (id >= r->cap) return 0;
  
  uint32_t g = atomic_load_explicit(&r->metas[id].gen, memory_order_acquire);
  uint32_t g24 = g & 0xFFFFFFu;  /* Truncate to 24 bits */
  if (g24 == 0) g24 = 1;         /* Skip 0 (reserved for NULL) */
  return g24;
}

/* Lookup and validate slab by handle (returns NULL if invalid).
 *
 * Three-step validation handshake:
 * 1. Load ptr with acquire (sees slab initialization before publication)
 * 2. Load gen with acquire (sees current generation)
 * 3. Compare handle generation with current generation
 *
 * Failure modes (all return NULL, safe):
 * - id out of bounds: Invalid handle (corrupted or wrong allocator)
 * - ptr is NULL: Slab not yet published or already recycled
 * - gen mismatch: Stale handle from old incarnation (ABA detected)
 *
 * Safety invariant: Slabs are never unmapped (only madvised).
 * If ptr is non-NULL, the virtual mapping is valid—safe to dereference after validation.
 *
 * Spurious failures are safe:
 * If we observe new ptr but old gen (or vice versa) due to independent atomics,
 * validation fails and returns NULL early. Caller retries or treats as invalid.
 */
Slab* reg_lookup_validate(SlabRegistry* r, uint32_t id, uint32_t gen24) {
  if (id >= r->cap) return NULL;  /* Out of bounds */
  
  /* Step 1: Load ptr with acquire (synchronizes with reg_set_ptr release) */
  Slab* s = atomic_load_explicit(&r->metas[id].ptr, memory_order_acquire);
  if (!s) return NULL;  /* Not published yet or NULL-ed during recycling */
  
  /* Step 2: Load current generation with acquire */
  uint32_t cur = reg_get_gen24(r, id);
  
  /* Step 3: Validate generation from handle matches current generation */
  if (cur != gen24) return NULL;  /* ABA detected: handle from old incarnation */
  
  return s;  /* Valid! Safe to dereference slab */
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

inline void* slab_slot_ptr(Slab* s, uint32_t slot_index) {
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
  If out_retries is non-NULL, stores CAS retry count (for contention tracking).
*/
/* Helper: compute the "full" bitmask for a given word in the allocation bitmap.
 * 
 * For all words except possibly the last, a full word is 0xFFFFFFFF (all 32 bits set).
 * For the last word, only (object_count % 32) bits are valid; higher bits are unused.
 * 
 * CRITICAL: Handles edge case where object_count % 32 == 0 (all 32 bits valid).
 * Without this, (1u << 32) would be undefined behavior, and (1u << 0) - 1 == 0
 * would incorrectly treat an empty word (0x00000000) as "full".
 */
static inline uint32_t full_mask_for_word(uint32_t obj_count, uint32_t w, uint32_t words) {
  if (w != words - 1u) return 0xFFFFFFFFu;  /* Not last word: all 32 bits valid */
  uint32_t rem = obj_count & 31u;            /* obj_count % 32 */
  if (rem == 0u) return 0xFFFFFFFFu;         /* Last word uses all 32 bits */
  return (1u << rem) - 1u;                   /* Last word uses only 'rem' bits */
}

/* Lock-free bitmap allocation: find and mark first free slot.
 *
 * Returns slot index on success, UINT32_MAX if slab is full.
 * Outputs previous free_count (for transition detection) and retry count (for contention tracking).
 *
 * Scans bitmap word-by-word looking for a zero bit (free slot).
 * Uses atomic CAS to claim the slot without holding a lock.
 * May retry if another thread claims the slot first (thundering herd).
 *
 * Adaptive scanning reduces retries under contention:
 * - Mode 0 (sequential): Scan 0,1,2... (low contention, predictable)
 * - Mode 1 (randomized): Start at TLS-cached offset (high contention, spreads threads)
 */
static uint32_t slab_alloc_slot_atomic(Slab* s, SizeClassAlloc* sc, uint32_t* out_prev_fc, uint32_t* out_retries) {
  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  const uint32_t words = slab_bitmap_words(s->object_count);  /* Bitmap size in 32-bit words */
  uint32_t retries = 0;
  const uint32_t SPIN_TRIPWIRE = 10000000;  /* 10 million retries = infinite loop */

  /* Adaptive scanning: load current mode (sequential vs randomized).
   * Mode switches automatically based on CAS retry rate (0.30 threshold).
   * TLS offset is cached per-thread to avoid repeated hash computation. */
  uint32_t start_word = 0;
  if (atomic_load_explicit(&sc->scan_adapt.mode, memory_order_relaxed) == 1) {
    start_word = get_tls_scan_offset(words);  /* Hash thread ID, modulo words */
  }
  
  /* Scan bitmap words starting from start_word, wrapping around.
   * Example: If start_word=5 and words=8, scan order is 5,6,7,0,1,2,3,4 */
  for (uint32_t i = 0; i < words; i++) {
    uint32_t w = (start_word + i) % words;
    
    while (1) {  /* Retry loop in case of CAS failure */
      uint32_t x = atomic_load_explicit(&bm[w], memory_order_relaxed);
      if (x == 0xFFFFFFFFu) break;  /* All 32 bits set, word is full */

      uint32_t free_mask = ~x;  /* Invert: 0 bits become 1 (free slots) */

      /* Last word boundary handling: not all 32 bits may be valid.
       * Example: 63 slots = 2 words, last word has only 31 valid bits.
       * Mask out invalid high bits to prevent accessing non-existent slots. */
      if (w == words - 1u) {
        uint32_t valid_bits = s->object_count - (w * 32u);
        if (valid_bits < 32u) {
          uint32_t valid_mask = (1u << valid_bits) - 1u;
          free_mask &= valid_mask;
          if (free_mask == 0u) break;  /* No valid free bits */
        }
      }

      /* Find first free slot using count-trailing-zeros (BSF instruction on x86).
       * This is 1-3 cycles, much faster than scanning bits individually. */
      uint32_t bit = ctz32(free_mask);
      uint32_t mask = 1u << bit;
      uint32_t desired = x | mask;  /* Set the bit to mark slot allocated */

      /* Try to claim the slot atomically. If another thread claimed it first,
       * x is reloaded with the new value and we retry with a different bit. */
      if (atomic_compare_exchange_weak_explicit(
              &bm[w], &x, desired,
              memory_order_acq_rel,   /* Acquire on success for happens-before */
              memory_order_relaxed)) {
        /* Success! Decrement free_count and return the slot index.
         * prev_fc is used to detect transitions (partial→full when prev_fc==1). */
        uint32_t prev_fc = atomic_fetch_sub_explicit(&s->free_count, 1u, memory_order_relaxed);
        if (out_prev_fc) *out_prev_fc = prev_fc;
        if (out_retries) *out_retries = retries;
        return w * 32u + bit;  /* Convert (word, bit) to slot index */
      }
      
      /* CAS failed, another thread modified the bitmap. Retry with updated x. */
      retries++;
      
      /* SPIN TRIPWIRE: Detect infinite CAS retry loop (bitmap corruption) */
      if (retries > SPIN_TRIPWIRE) {
        fprintf(stderr, "\n*** SPIN TRIPWIRE: slab_alloc_slot_atomic infinite loop ***\n");
        fprintf(stderr, "Thread %lu stuck after %u retries\n", 
                (unsigned long)pthread_self(), retries);
        fprintf(stderr, "Slab: %p, word: %u/%u, object_count: %u\n",
                (void*)s, w, words, s->object_count);
        fprintf(stderr, "Current bitmap[%u]: 0x%08x, free_mask: 0x%08x\n",
                w, x, free_mask);
        fprintf(stderr, "Slab ID: %u, free_count: %u\n",
                s->slab_id, atomic_load_explicit(&s->free_count, memory_order_relaxed));
        fflush(stderr);
        abort();
      }
    }
  }

  if (out_retries) *out_retries = retries;  /* Phase 2.2: Failed allocation, still report retries */
  return UINT32_MAX;
}

/* Lock-free bitmap free: mark a slot as free.
 *
 * Returns true on success, false if slot was already free (double-free detection).
 * Outputs previous free_count (for full→partial transition detection) and retry count.
 *
 * Uses atomic CAS to clear the bit without holding a lock.
 */
static bool slab_free_slot_atomic(Slab* s, uint32_t idx, uint32_t* out_prev_fc, uint32_t* out_retries) {
  if (idx >= s->object_count) return false;  /* Out of bounds */

  /* Convert slot index to (word, bit) coordinates in bitmap */
  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  uint32_t w = idx / 32u;     /* Which 32-bit word */
  uint32_t bit = idx % 32u;   /* Which bit within that word */
  uint32_t mask = 1u << bit;  /* Bitmask with only that bit set */
  uint32_t retries = 0;
  const uint32_t SPIN_TRIPWIRE = 10000000;  /* 10 million retries = infinite loop */

  while (1) {  /* Retry loop in case of CAS failure */
    uint32_t x = atomic_load_explicit(&bm[w], memory_order_relaxed);
    
    /* Double-free detection: if bit is already 0, slot was already freed */
    if ((x & mask) == 0u) {
      if (out_retries) *out_retries = retries;
      return false;
    }

    /* Clear the bit to mark slot as free */
    uint32_t desired = x & ~mask;
    
    if (atomic_compare_exchange_weak_explicit(
            &bm[w], &x, desired,
            memory_order_acq_rel,   /* Release stores, acquire loads */
            memory_order_relaxed)) {
      /* Success! Increment free_count.
       * prev_fc is used to detect full→partial transitions (prev_fc==0 means was full). */
      uint32_t prev_fc = atomic_fetch_add_explicit(&s->free_count, 1u, memory_order_relaxed);
      if (out_prev_fc) *out_prev_fc = prev_fc;
      if (out_retries) *out_retries = retries;
      return true;
    }
    
    /* CAS failed, another thread modified the bitmap. Retry with updated x. */
    retries++;
    
    /* SPIN TRIPWIRE: Detect infinite CAS retry loop (bitmap corruption) */
    if (retries > SPIN_TRIPWIRE) {
      fprintf(stderr, "\n*** SPIN TRIPWIRE: slab_free_slot_atomic infinite loop ***\n");
      fprintf(stderr, "Thread %lu stuck after %u retries\n",
              (unsigned long)pthread_self(), retries);
      fprintf(stderr, "Slab: %p, word: %u, bit: %u, idx: %u, object_count: %u\n",
              (void*)s, w, bit, idx, s->object_count);
      fprintf(stderr, "Current bitmap[%u]: 0x%08x, mask: 0x%08x\n",
              w, x, mask);
      fprintf(stderr, "Slab ID: %u, free_count: %u\n",
              s->slab_id, atomic_load_explicit(&s->free_count, memory_order_relaxed));
      fflush(stderr);
      abort();
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
  
  /* Phase 2.2: Initialize era tracking for monotonic observability */
  atomic_store_explicit(&a->epoch_era_counter, 0, memory_order_relaxed);
  for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
    atomic_store_explicit(&a->epoch_era[e], 0, memory_order_relaxed);  /* Era 0 for all epochs at startup */
  }
  
  /* Phase 2.3: Initialize epoch metadata for rich observability */
  pthread_mutex_init(&a->epoch_label_lock, NULL);
  for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
    a->epoch_meta[e].open_since_ns = 0;  /* 0 = never opened */
    atomic_store_explicit(&a->epoch_meta[e].domain_refcount, 0, memory_order_relaxed);
    a->epoch_meta[e].label[0] = '\0';  /* Empty label */
    a->epoch_meta[e].label_id = 0;  /* Phase 2.3: ID 0 = unlabeled */
    
    /* Phase 2.4: Initialize RSS delta tracking */
    a->epoch_meta[e].rss_before_close = 0;  /* 0 = never closed */
    a->epoch_meta[e].rss_after_close = 0;
  }
  
  /* Phase 2.3: Initialize label registry for bounded semantic attribution */
  pthread_mutex_init(&a->label_registry.lock, NULL);
  a->label_registry.count = 1;  /* ID 0 reserved for unlabeled */
  memset(a->label_registry.labels, 0, sizeof(a->label_registry.labels));
  strncpy(a->label_registry.labels[0], "(unlabeled)", 31);
  
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
    a->classes[i].parent_alloc = a;  /* Phase 2.3: Backpointer for label_id lookup */
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
    
    /* Phase 2.2: Initialize lock-free contention counters */
    atomic_store_explicit(&a->classes[i].bitmap_alloc_cas_retries, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].bitmap_free_cas_retries, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].current_partial_cas_failures, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].bitmap_alloc_attempts, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].bitmap_free_attempts, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].current_partial_cas_attempts, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].lock_fast_acquire, 0, memory_order_relaxed);
    atomic_store_explicit(&a->classes[i].lock_contended, 0, memory_order_relaxed);
    
#ifdef ENABLE_LABEL_CONTENTION
    /* Phase 2.3: Initialize per-label contention counters */
    for (uint8_t lid = 0; lid < MAX_LABEL_IDS; lid++) {
      atomic_store_explicit(&a->classes[i].lock_fast_acquire_by_label[lid], 0, memory_order_relaxed);
      atomic_store_explicit(&a->classes[i].lock_contended_by_label[lid], 0, memory_order_relaxed);
      atomic_store_explicit(&a->classes[i].bitmap_alloc_cas_retries_by_label[lid], 0, memory_order_relaxed);
      atomic_store_explicit(&a->classes[i].bitmap_free_cas_retries_by_label[lid], 0, memory_order_relaxed);
    }
#endif

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

/* Pop a recycled slab from cache.
 *
 * Two-tier cache structure:
 * 1. Array cache (32 entries): Fast, no malloc/free overhead
 * 2. Overflow list (unbounded): Handles cache pressure, uses heap nodes
 *
 * Returns slab pointer and slab_id via out parameter.
 * Returns NULL if cache is empty (forces new_slab to call mmap).
 *
 * Why off-page storage matters:
 * - madvise(MADV_DONTNEED) zeros the slab header when recycling
 * - We can't read slab->slab_id after madvise (it's been zeroed)
 * - We store slab_id alongside the pointer (off-page) so it survives
 * - This enables generation bumping and registry updates on reuse
 */
static Slab* cache_pop(SizeClassAlloc* sc, uint32_t* out_slab_id, bool* out_was_published) {
  LOCK_WITHOUT_PROBE(&sc->cache_lock, LOCK_RANK_CACHE, "sc->cache_lock");
  Slab* s = NULL;
  uint32_t id = UINT32_MAX;
  bool was_pub = false;
  
  if (sc->cache_size > 0) {
    /* Pop from array cache (fast path).
     * CachedSlab stores pointer, ID, and was_published off-page. */
    CachedSlab* entry = &sc->slab_cache[--sc->cache_size];
    s = entry->slab;
    id = entry->slab_id;  /* Survived madvise because stored off-page */
    was_pub = entry->was_published;  /* Also survives madvise */
  } else if (sc->cache_overflow_head) {
    /* Array cache empty, try overflow list (slower, but avoids mmap).
     * CachedNode is a heap-allocated doubly-linked list node. */
    CachedNode* node = sc->cache_overflow_head;
    s = node->slab;
    id = node->slab_id;  /* Survived madvise because stored in node */
    was_pub = node->was_published;  /* Also survives madvise */
    
    /* Unlink from head of list */
    sc->cache_overflow_head = node->next;
    if (sc->cache_overflow_head) {
      sc->cache_overflow_head->prev = NULL;
    } else {
      sc->cache_overflow_tail = NULL;  /* List now empty */
    }
    sc->cache_overflow_len--;
    
    /* Free the node (slab itself stays mapped, just releasing metadata) */
    free(node);
  }
  
  UNLOCK_WITH_RANK(&sc->cache_lock);
  
  if (out_slab_id) *out_slab_id = id;
  if (out_was_published) *out_was_published = was_pub;
  return s;  /* NULL if both cache and overflow are empty */
}

/* Push empty slab to cache for reuse.
 *
 * Two-tier caching to balance performance vs memory:
 * 1. Array cache (32 entries): Fast, LIFO, no malloc overhead
 * 2. Overflow list (unbounded): Handles bursty workloads, uses heap nodes
 *
 * Critical design decision: madvise AFTER releasing lock.
 * - madvise() can take 5-50µs (variable, depends on kernel state)
 * - Holding lock during madvise adds jitter to all allocating threads
 * - Release lock first → predictable critical section latency
 * - madvise outside lock → only affects recycling thread
 *
 * Safety invariant: Slab must be fully unlinked from partial/full lists
 * before calling this function. Asserted defensively.
 */
static void cache_push(SizeClassAlloc* sc, Slab* s) {
  /* Verify slab was properly unlinked from epoch lists.
   * Caching a slab that's still on a list would corrupt the list structure
   * and allow concurrent allocation/free races. */
  assert_slab_unlinked(s);
  
  /* Mark slab as no longer on any active list.
   * Must happen before madvise because header fields will be zeroed. */
  s->list_id = SLAB_LIST_NONE;
  s->prev = NULL;  /* Defensive: ensure no dangling pointers */
  s->next = NULL;
  
  /* Snapshot slab_id and was_published from header NOW, before madvise.
   * After madvise zeros the header, these fields become unreadable.
   * We must store them off-page in the cache entry. */
  uint32_t id_snapshot = s->slab_id;
  bool was_pub_snapshot = s->was_published;
  
  /* RSS reclamation: madvise BEFORE making slab reachable via cache.
   *
   * CRITICAL ORDERING FIX:
   * Old code did: lock → insert into cache → unlock → madvise
   * Race: Another thread could pop, reinitialize, and publish the slab
   * BEFORE the original thread completes madvise, causing catastrophic
   * header corruption (free_count=UINT32_MAX, object_count=0).
   *
   * New code: madvise → lock → insert into cache → unlock
   * This eliminates the reuse-before-madvise race entirely.
   *
   * Only madvise slabs that were NEVER published to current_partial
   * (was_published==false). Published slabs may have in-flight lock-free
   * pointers even after retirement, so they must never be madvised.
   *
   * Trade-off: Published slabs keep RSS, but never-published slabs reclaim pages.
   * Gated by ENABLE_RSS_RECLAMATION compile flag.
   */
  #if ENABLE_RSS_RECLAMATION && defined(__linux__)
  if (!was_pub_snapshot) {
    atomic_fetch_add_explicit(&sc->madvise_calls, 1, memory_order_relaxed);
    int ret = madvise(s, SLAB_PAGE_SIZE, MADV_DONTNEED);
    if (ret == 0) {
      atomic_fetch_add_explicit(&sc->madvise_bytes, SLAB_PAGE_SIZE, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&sc->madvise_failures, 1, memory_order_relaxed);
    }
  }
  #endif
  
  /* Now insert into cache. After this point, another thread can pop this slab.
   * If we madvised above, the header is already zeroed and we rely on the
   * off-page snapshots (id_snapshot, was_pub_snapshot) for metadata. */
  LOCK_WITHOUT_PROBE(&sc->cache_lock, LOCK_RANK_CACHE, "sc->cache_lock");
  
  if (sc->cache_size < sc->cache_capacity) {
    /* Fast path: array cache has space (common case, 32 entries).
     * Store slab pointer and snapshots (not header fields, which may be zero). */
    sc->slab_cache[sc->cache_size].slab = s;
    sc->slab_cache[sc->cache_size].slab_id = id_snapshot;
    sc->slab_cache[sc->cache_size].was_published = was_pub_snapshot;
    sc->cache_size++;
    s->cache_state = SLAB_CACHED;  /* Safe: cache_state not in madvised region */
    atomic_fetch_add_explicit(&sc->empty_slab_recycled, 1, memory_order_relaxed);
  } else {
    /* Slow path: array cache full, push to overflow list.
     * Allocate heap node to store slab metadata. If malloc fails, we skip
     * caching this slab (it leaks until allocator_destroy, but better than crashing). */
    CachedNode* node = (CachedNode*)malloc(sizeof(CachedNode));
    if (!node) {
      /* Out of memory for overflow node. Skip caching.
       * Slab stays mapped but unusable until allocator shutdown. */
      UNLOCK_WITH_RANK(&sc->cache_lock);
      return;
    }
    
    /* Store slab pointer and snapshots in heap node (not header fields). */
    node->slab = s;
    node->slab_id = id_snapshot;
    node->was_published = was_pub_snapshot;
    node->prev = sc->cache_overflow_tail;
    node->next = NULL;
    
    /* Append to tail of doubly-linked list */
    if (sc->cache_overflow_tail) {
      sc->cache_overflow_tail->next = node;
    } else {
      sc->cache_overflow_head = node;  /* First node in list */
    }
    sc->cache_overflow_tail = node;
    sc->cache_overflow_len++;
    
    s->cache_state = SLAB_OVERFLOWED;  /* Safe: cache_state not in madvised region */
    atomic_fetch_add_explicit(&sc->empty_slab_overflowed, 1, memory_order_relaxed);
  }
  
  UNLOCK_WITH_RANK(&sc->cache_lock);
}

/* ------------------------------ Slab allocation ------------------------------ */

/* Allocate a new slab for a size class and epoch.
 *
 * Two-path allocation strategy:
 * 1. Cache hit: Pop recycled slab from cache (cheap, no syscall)
 * 2. Cache miss: Allocate fresh page via mmap (expensive, ~2µs)
 *
 * Cache-first avoids mmap overhead. Cache hit rate >97% in production.
 * Recycled slabs may have been madvised, so all metadata must be reinitialized.
 *
 * Generation bumping on reuse provides ABA protection:
 * - Old handles encode old generation number
 * - After reuse, generation increments
 * - Old handles fail validation (generation mismatch)
 * - Prevents use-after-free even if handle references same slab_id
 */
static Slab* new_slab(SlabAllocator* a, SizeClassAlloc* sc, uint32_t epoch_id) {
  uint32_t obj_size = sc->object_size;

  /* Try cache first. Avoids mmap syscall if we have recycled slabs available.
   * cache_pop() returns slab pointer, slab_id, and was_published (stored off-page). */
  uint32_t cached_id = UINT32_MAX;
  bool cached_was_published = false;
  Slab* s = cache_pop(sc, &cached_id, &cached_was_published);
  
  if (s) {
    /* Cache hit! Bump generation to invalidate old handles.
     * Without this, a handle from the slab's previous incarnation would
     * still validate, leading to use-after-free. */
    (void)reg_bump_gen(&a->reg, cached_id);
    
    /* Reinitialize all slab metadata.
     * If RSS reclamation is enabled, madvise() zeroed the header when this
     * slab was recycled. Even without madvise, defensive reinitialization
     * ensures clean state (prevents stale list pointers, corrupted counts). */
    uint32_t expected_count = slab_object_count(obj_size);
    
    /* CRITICAL: Handle slot field is only 8 bits (bits [17:10] in handle encoding).
     * If object_count > 255, handle encoding silently truncates slot index,
     * causing free_obj() to free wrong slot and corrupt bitmap. */
    assert(expected_count <= 255 && "handle slot field is 8-bit; object_count must be <=255");
    
    s->prev = NULL;
    s->next = NULL;
    atomic_store_explicit(&s->magic, SLAB_MAGIC, memory_order_relaxed);
    s->object_size = obj_size;
    s->object_count = expected_count;
    s->list_id = SLAB_LIST_NONE;
    s->cache_state = SLAB_ACTIVE;
    s->epoch_id = epoch_id;
    s->era = atomic_load_explicit(&a->epoch_era[epoch_id], memory_order_acquire);
    s->was_published = cached_was_published;   /* Restore from off-page cache (survived madvise) */
    s->slab_id = cached_id;  /* Restore ID from cache (survived madvise) */
    atomic_store_explicit(&s->free_count, expected_count, memory_order_relaxed);
    
    /* Clear allocation bitmap.
     * Each bit represents one slot: 0=free, 1=allocated.
     * Start with all zeros (all slots free). */
    _Atomic uint32_t* bm = slab_bitmap_ptr(s);
    uint32_t words = slab_bitmap_words(expected_count);
    for (uint32_t i = 0; i < words; i++) {
      atomic_store_explicit(&bm[i], 0u, memory_order_relaxed);
    }
    
    /* Republish slab pointer in registry.
     * The pointer may have been set to NULL during recycling, or this may
     * be the first time we're publishing it. Either way, make it findable. */
    reg_set_ptr(&a->reg, cached_id, s);
    
    return s;  /* Cache hit path complete */
  }

  /* Cache miss: no recycled slabs available, must allocate fresh page.
   * This path is ~100x slower than cache hit due to mmap syscall.
   * Counter helps diagnose "why is allocation slow?" (cache pressure). */
  atomic_fetch_add_explicit(&sc->new_slab_count, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&sc->slow_path_cache_miss, 1, memory_order_relaxed);
  
  /* Allocate 4KB page from kernel via mmap.
   * Page-aligned for fast address arithmetic: (ptr & ~4095) recovers header. */
  void* page = map_one_page();
  if (!page) return NULL;  /* mmap failed (out of memory) */

#if ENABLE_DIAGNOSTIC_COUNTERS
  /* Track committed bytes (mmap'd memory) for RSS analysis */
  atomic_fetch_add_explicit(&sc->committed_bytes, SLAB_PAGE_SIZE, memory_order_relaxed);
#endif

  s = (Slab*)page;  /* Slab header lives at start of page */

  /* Calculate how many objects fit in this page.
   * Formula accounts for: slab header + bitmap + alignment → usable space → object count.
   * Example: 4096B page, 128B objects → 64B header + 4B bitmap → ~3968B usable → 31 objects */
  uint32_t count = slab_object_count(obj_size);
  
  /* CRITICAL: Handle slot field is only 8 bits (bits [17:10] in handle encoding).
   * If object_count > 255, handle encoding silently truncates slot index,
   * causing free_obj() to free wrong slot and corrupt bitmap. */
  assert(count <= 255 && "handle slot field is 8-bit; object_count must be <=255");
  
  if (count == 0) {
    /* Pathological case: object too large to fit even one slot after header.
     * Should never happen with our size classes (64-768 bytes). */
    unmap_one_page(page);
    errno = EINVAL;
    return NULL;
  }

  /* Allocate registry ID for handle encoding.
   * Registry maps slab_id → (slab pointer, generation counter).
   * Enables portable handles and ABA protection. */
  uint32_t id = reg_alloc_id(&a->reg);
  if (id == UINT32_MAX) {
    /* Registry allocation failed (out of memory for registry growth) */
    unmap_one_page(page);
    errno = ENOMEM;
    return NULL;
  }

  /* Initialize slab header with metadata */
  s->prev = NULL;  /* Not on any list yet */
  s->next = NULL;
  atomic_store_explicit(&s->magic, SLAB_MAGIC, memory_order_relaxed);  /* "SLAB" in ASCII */
  s->object_size = obj_size;   /* Which size class: 64, 96, 128, etc. */
  s->object_count = count;      /* How many slots calculated above */
  atomic_store_explicit(&s->free_count, count, memory_order_relaxed);  /* All slots free initially */
  s->list_id = SLAB_LIST_NONE;  /* Not on partial/full list until added */
  s->cache_state = SLAB_ACTIVE; /* In use (not cached) */
  s->epoch_id = epoch_id;       /* Temporal grouping: objects from this epoch */
  s->era = a->epoch_era[epoch_id];  /* Monotonic timestamp for observability */
  s->was_published = false;     /* Fresh slab not yet reachable lock-free */
  s->slab_id = id;              /* Registry ID for handle encoding */

  /* Initialize allocation bitmap to all zeros (all slots free) */
  _Atomic uint32_t* bm = slab_bitmap_ptr(s);
  uint32_t words = slab_bitmap_words(count);
  for (uint32_t i = 0; i < words; i++) {
    atomic_store_explicit(&bm[i], 0u, memory_order_relaxed);
  }

  /* Publish slab in registry so handles can find it */
  reg_set_ptr(&a->reg, id, s);

  return s;  /* Fresh slab ready for allocation */
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

inline void handle_unpack(SlabHandle h, uint32_t* slab_id, uint32_t* gen, uint32_t* slot, uint32_t* cls) {
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

/* Main allocation function: allocate an object from a specific epoch.
 *
 * Two-tier allocation strategy:
 * 1. Fast path: Lock-free load of current_partial pointer, atomic bitmap allocation
 * 2. Slow path: Take mutex, scan partial list or create new slab
 *
 * Fast path succeeds >97% of the time (measured). Median latency: 74ns.
 * Slow path adds ~200ns for list scan or ~2µs for new slab (mmap syscall).
 *
 * Returns pointer to allocated object, or NULL if:
 * - Size exceeds 768 bytes (not a supported size class)
 * - Epoch is invalid or CLOSING (draining, no new allocations allowed)
 * - Out of memory (mmap failed)
 *
 * If out is non-NULL, writes a portable handle encoding (slab_id, generation, slot, class).
 */
void* alloc_obj_epoch(SlabAllocator* a, uint32_t size, EpochId epoch, SlabHandle* out) {
#ifdef ENABLE_SLOWPATH_SAMPLING
  /* Phase 2.5: Probabilistic end-to-end sampling (1/1024)
   * Wall vs CPU time split detects WSL2/virtualization interference */
  bool sample = ((++tls_sample_ctr & SAMPLE_RATE_MASK) == 0);
  struct timespec wall0, cpu0, wall1, cpu1;
  if (sample) {
    clock_gettime(CLOCK_MONOTONIC, &wall0);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
  }
  
  /* Helper macro to record sample before return */
  #define RECORD_SAMPLE() do { \
    if (sample) { \
      clock_gettime(CLOCK_MONOTONIC, &wall1); \
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1); \
      uint64_t wall_ns = ns_from_ts(&wall1) - ns_from_ts(&wall0); \
      uint64_t cpu_ns = ns_from_ts(&cpu1) - ns_from_ts(&cpu0); \
      tls_stats.alloc_samples++; \
      tls_stats.alloc_wall_ns_sum += wall_ns; \
      tls_stats.alloc_cpu_ns_sum += cpu_ns; \
      if (wall_ns > tls_stats.alloc_wall_ns_max) tls_stats.alloc_wall_ns_max = wall_ns; \
      if (cpu_ns > tls_stats.alloc_cpu_ns_max) tls_stats.alloc_cpu_ns_max = cpu_ns; \
    } \
  } while (0)
#else
  #define RECORD_SAMPLE() do { } while (0)
#endif
  
  /* Map requested size to nearest size class (64, 96, 128, ..., 768).
   * Uses O(1) lookup table: k_class_lookup[size] → class index. */
  int ci = class_index_for_size(size);
  if (ci < 0) { RECORD_SAMPLE(); return NULL; }  /* Size too large or zero */
  
  if (epoch >= a->epoch_count) { RECORD_SAMPLE(); return NULL; }  /* Invalid epoch ID */
  
  SizeClassAlloc* sc = &a->classes[(size_t)ci];
  
  /* Check if epoch is accepting allocations FIRST (before TLS).
   * CRITICAL: TLS refill must not bypass epoch-closed check, otherwise caches
   * will hold handles from CLOSING epochs that should be flushed.
   * ACTIVE (0) = accepting, CLOSING (1) = draining.
   * 
   * Use memory_order_acquire to synchronize with epoch_close()'s release store.
   * This ensures we observe CLOSING before epoch_close() flushes TLS caches. */
  uint32_t state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_acquire);
  if (state != EPOCH_ACTIVE) {
    atomic_fetch_add_explicit(&sc->slow_path_epoch_closed, 1, memory_order_relaxed);
    RECORD_SAMPLE();
    return NULL;
  }
  
#if ENABLE_TLS_CACHE
  /* Skip TLS during refill to prevent infinite recursion.
   * When tls_refill() calls alloc_obj_epoch(), we must bypass TLS entirely. */
  extern __thread bool _tls_in_refill;
  
  if (!_tls_in_refill) {
    /* Check if TLS bypass is active for this size class */
    extern __thread TLSCache _tls_cache[8];
    TLSCache* tls = &_tls_cache[ci];
    
    /* HARD BYPASS: skip all TLS logic if bypass is enabled */
    if (!tls->bypass_alloc) {
      /* TLS fast path: Check thread-local handle cache first (no atomics, no locks) */
      void* ptr = tls_try_alloc(a, (uint32_t)ci, epoch, out);
      if (ptr) return ptr;  /* TLS cache hit - fastest path (~10-15ns) */
      
      /* TLS miss: Refill from global allocator, then retry */
      tls_refill(a, (uint32_t)ci, epoch);
      ptr = tls_try_alloc(a, (uint32_t)ci, epoch, out);
      if (ptr) return ptr;  /* Refill succeeded */
      
      /* Fall through to global allocator if refill failed (out of memory) */
    }
  }
#endif
  
  EpochState* es = get_epoch_state(sc, epoch);  /* Array lookup: &sc->epochs[epoch] */

  /* Fast path: lock-free load of pre-selected slab.
   * current_partial is published by the slow path and points to a slab
   * with likely free slots. Multiple threads can attempt allocation concurrently
   * via atomic bitmap CAS. No mutex needed here—this is the hot path. */
  Slab* cur = atomic_load_explicit(&es->current_partial, memory_order_acquire);
  
  /* Validate pointer before dereferencing.
   * Magic check defends against corruption and stale pointers. */
  if (cur && atomic_load_explicit(&cur->magic, memory_order_relaxed) == SLAB_MAGIC) {
    uint32_t prev_fc = 0;  /* Previous free_count, for transition detection */
    uint32_t retries = 0;  /* CAS retry count, for contention tracking */
    
    /* Try to allocate a slot from this slab's bitmap atomically */
    uint32_t idx = slab_alloc_slot_atomic(cur, sc, &prev_fc, &retries);
    
    if (idx != UINT32_MAX) {
      /* Phase 2.2: Record successful allocation + CAS retries */
      uint64_t prev_attempts =
          atomic_fetch_add_explicit(&sc->bitmap_alloc_attempts, 1, memory_order_relaxed);
      uint64_t cur_attempts = prev_attempts + 1;
      
      if (retries > 0) {
        atomic_fetch_add_explicit(&sc->bitmap_alloc_cas_retries, retries, memory_order_relaxed);
#ifdef ENABLE_LABEL_CONTENTION
        /* Phase 2.3: Attribute CAS retries to current label */
        uint8_t lid = current_label_id(sc->parent_alloc);
        atomic_fetch_add_explicit(&sc->bitmap_alloc_cas_retries_by_label[lid], retries, memory_order_relaxed);
#endif
      }
      
      /* Adaptive controller heartbeat: every 262,144 allocations, check retry rate
       * and potentially switch scanning mode. Uses allocation count instead of
       * time to avoid clock syscalls in the hot path. */
      if ((cur_attempts & ((1u << 18) - 1u)) == 0u) {
        scan_adapt_check(sc);  /* May switch sequential↔randomized based on contention */
      }
      
      /* If we just allocated from a fully empty slab, decrement the empty counter.
       * prev_fc == object_count means the slab had all slots free before our allocation. */
      if (prev_fc == cur->object_count) {
        atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
      }
      
      /* Transition detection: did our allocation exhaust the last free slot?
       * prev_fc==1 means there was one free slot before our allocation, now zero.
       * Must move slab from PARTIAL to FULL list and select a new current_partial. */
      if (prev_fc == 1) {
        LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");  /* Need mutex to mutate lists */
        
        /* Re-check list_id under lock (may have been moved by another thread) */
        if (cur->list_id == SLAB_LIST_PARTIAL) {
          atomic_fetch_add_explicit(&sc->list_move_partial_to_full, 1, memory_order_relaxed);
          list_remove(&es->partial, cur);
          cur->list_id = SLAB_LIST_FULL;
          list_push_back(&es->full, cur);
          
          /* Publish next slab from partial list (NULL if list is empty).
           * This lets other threads continue on fast path without hitting slow path. */
          Slab* next = es->partial.head;
          if (next) {
            next->was_published = true;
          }
          atomic_store_explicit(&es->current_partial, next, memory_order_release);
        }
        UNLOCK_WITH_RANK(&sc->lock);
      }
      
      /* Convert slot index to actual memory address.
       * Address = slab_base + (slot_index × object_size). */
      void* p = slab_slot_ptr(cur, idx);
      
      /* Encode portable handle if requested.
       * Handle contains: slab_id (registry lookup), generation (ABA protection),
       * slot index, and size class. Avoids embedding raw pointers. */
      if (out) {
        *out = encode_handle(cur, &a->reg, idx, (uint32_t)ci);
      }
      
#if ENABLE_DIAGNOSTIC_COUNTERS
      /* Track live bytes (bytes allocated to live objects) for leak detection */
      atomic_fetch_add_explicit(&sc->live_bytes, sc->object_size, memory_order_relaxed);
#endif
      
      RECORD_SAMPLE();
      return p;  /* Fast path success! */
    }
    
    /* Fast path failed: slab was full between load and allocation attempt.
     * This is a race—another thread exhausted the slab first.
     * Null out current_partial so next thread goes to slow path.
     * 
     * ROBUSTNESS FIX: Also move slab to FULL list if bitmap is truly full.
     * This prevents "zombie partial" syndrome where free_count divergence
     * causes prev_fc==1 transition to never fire, leaving full slab on PARTIAL forever. */
    atomic_fetch_add_explicit(&sc->current_partial_full, 1, memory_order_relaxed);
    
    atomic_fetch_add_explicit(&sc->current_partial_cas_attempts, 1, memory_order_relaxed);
    Slab* expected = cur;
    bool swapped = atomic_compare_exchange_strong_explicit(
      &es->current_partial, &expected, NULL,
      memory_order_release, memory_order_relaxed);
    if (!swapped) {
      /* Another thread already nulled it—fine, contention is expected */
      atomic_fetch_add_explicit(&sc->current_partial_cas_failures, 1, memory_order_relaxed);
    }
    
    /* Additional robustness: If slab is still on PARTIAL list and bitmap is full,
     * move it to FULL list now. This ensures we don't depend on prev_fc==1 being
     * correct forever (handles free_count divergence gracefully). */
    LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");
    if (cur->list_id == SLAB_LIST_PARTIAL) {
      /* Verify bitmap is truly full (with double-check to avoid transient views) */
      _Atomic uint32_t* bm = slab_bitmap_ptr(cur);
      uint32_t words = slab_bitmap_words(cur->object_count);
      bool bitmap_full = true;
      
      for (uint32_t i = 0; i < words; i++) {
        uint32_t x = atomic_load_explicit(&bm[i], memory_order_relaxed);
        if (x != full_mask_for_word(cur->object_count, i, words)) {
          bitmap_full = false;
          break;
        }
      }
      
      if (bitmap_full) {
        atomic_thread_fence(memory_order_acquire);
        for (uint32_t i = 0; i < words; i++) {
          uint32_t x = atomic_load_explicit(&bm[i], memory_order_relaxed);
          if (x != full_mask_for_word(cur->object_count, i, words)) {
            bitmap_full = false;
            break;
          }
        }
      }
      
      if (bitmap_full) {
        /* Bitmap stably full: move PARTIAL → FULL */
        list_remove(&es->partial, cur);
        cur->list_id = SLAB_LIST_FULL;
        list_push_back(&es->full, cur);
        atomic_fetch_add_explicit(&sc->list_move_partial_to_full, 1, memory_order_relaxed);
        
        /* Publish next partial if available */
        Slab* next = es->partial.head;
        if (next) {
          next->was_published = true;
        }
        atomic_store_explicit(&es->current_partial, next, memory_order_release);
      }
    }
    UNLOCK_WITH_RANK(&sc->lock);
  } else if (!cur) {
    /* current_partial was NULL—no slab selected yet, or previous was exhausted.
     * Will proceed to slow path to select a new slab. */
    atomic_fetch_add_explicit(&sc->current_partial_null, 1, memory_order_relaxed);
  }

  /* Slow path: lock-protected allocation when fast path unavailable.
   * 
   * Loop structure handles rare race: if slab fills between publish and our
   * allocation attempt, retry instead of failing. */
  const uint32_t SLOW_PATH_TRIPWIRE = 1000;  /* 1000 retries = something is deeply wrong */
  uint32_t slow_path_attempts = 0;
  
  for (;;) {
    /* Re-check epoch state (might have closed while we waited for lock) */
    state = atomic_load_explicit(&a->epoch_state[epoch], memory_order_relaxed);
    if (state != EPOCH_ACTIVE) {
      RECORD_SAMPLE();
      return NULL;  /* Epoch closed, refuse allocation */
    }
    
    atomic_fetch_add_explicit(&sc->slow_path_hits, 1, memory_order_relaxed);
    slow_path_attempts++;
    
    /* SPIN TRIPWIRE: Detect infinite slow-path retry loop */
    if (slow_path_attempts > SLOW_PATH_TRIPWIRE) {
      fprintf(stderr, "\n*** SPIN TRIPWIRE: alloc_obj_epoch slow-path infinite loop ***\n");
      fprintf(stderr, "Thread %lu stuck after %u slow-path attempts\n",
              (unsigned long)pthread_self(), slow_path_attempts);
      fprintf(stderr, "Size class: %d, epoch: %u, state: %u\n",
              ci, epoch, state);
      fprintf(stderr, "Partial list length: %zu, current_partial: %p\n",
              es->partial.len, (void*)atomic_load_explicit(&es->current_partial, memory_order_relaxed));
      fflush(stderr);
      abort();
    }
    
    /* Try to allocate a new slab if needed (cache or mmap).
     * CRITICAL: Must call new_slab() BEFORE acquiring sc->lock to avoid
     * lock rank violation. new_slab() calls cache_pop() which acquires
     * cache_lock (rank 20), and we need sc->lock (rank 30) after.
     * Rank ordering requires: cache_lock → sc->lock, never reversed. */
    Slab* new_s = NULL;
    
    LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");  /* Take mutex, track contention */

    /* Try to get a slab from the partial list for this epoch. */
    Slab* s = es->partial.head;
    
    /* ZOMBIE PARTIAL REPAIR: If head slab is actually full, move it to FULL list
     * Check both free_count and bitmap to catch divergence cases */
    while (s) {
      uint32_t fc = atomic_load_explicit(&s->free_count, memory_order_relaxed);
      
      /* Quick check: if free_count >= 2, slab is definitely not full */
      if (fc >= 2) break;
      
      /* If free_count == 0 or == 1, verify against bitmap to catch divergence.
       * 
       * CRITICAL: Use full_mask_for_word() to avoid UB when object_count % 32 == 0.
       * Double-check with memory fence to reduce false positives from transient views. */
      _Atomic uint32_t* bm = slab_bitmap_ptr(s);
      uint32_t words = slab_bitmap_words(s->object_count);
      bool bitmap_full = true;
      
      for (uint32_t i = 0; i < words; i++) {
        uint32_t x = atomic_load_explicit(&bm[i], memory_order_relaxed);
        uint32_t expected_full = full_mask_for_word(s->object_count, i, words);
        if (x != expected_full) {
          bitmap_full = false;
          break;
        }
      }
      
      /* Double-check with acquire fence to synchronize with concurrent bitmap updates */
      if (bitmap_full) {
        atomic_thread_fence(memory_order_acquire);
        bitmap_full = true;  /* Reset and re-scan */
        for (uint32_t i = 0; i < words; i++) {
          uint32_t x = atomic_load_explicit(&bm[i], memory_order_relaxed);
          uint32_t expected_full = full_mask_for_word(s->object_count, i, words);
          if (x != expected_full) {
            bitmap_full = false;
            break;
          }
        }
      }
      
      if (!bitmap_full) break;  /* Slab has free slots, use it */
      
      /* Bitmap is stably full - move to FULL list.
       * DO NOT mutate free_count here: if there's divergence, the bug is in
       * allocation/free paths. Forcing free_count=0 under contention makes it worse. */
#ifdef ENABLE_SLOWPATH_SAMPLING
      struct timespec repair_wall0, repair_cpu0, repair_wall1, repair_cpu1;
      clock_gettime(CLOCK_MONOTONIC, &repair_wall0);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &repair_cpu0);
      
      uint32_t repair_reason = REPAIR_REASON_FULL_BITMAP;
      if (s->list_id != SLAB_LIST_PARTIAL) {
        repair_reason |= REPAIR_REASON_LIST_MISMATCH;
      }
#endif
      
      fprintf(stderr, "\n*** REPAIRING zombie partial slab %p (free_count=%u, bitmap full) ***\n", 
              (void*)s, fc);
      fflush(stderr);
      
      list_remove(&es->partial, s);
      s->list_id = SLAB_LIST_FULL;
      list_push_back(&es->full, s);
      atomic_fetch_add_explicit(&sc->list_move_partial_to_full, 1, memory_order_relaxed);
      
#ifdef ENABLE_SLOWPATH_SAMPLING
      clock_gettime(CLOCK_MONOTONIC, &repair_wall1);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &repair_cpu1);
      
      uint64_t repair_wall_ns = ns_from_ts(&repair_wall1) - ns_from_ts(&repair_wall0);
      uint64_t repair_cpu_ns = ns_from_ts(&repair_cpu1) - ns_from_ts(&repair_cpu0);
      
      tls_stats.repair_count++;
      tls_stats.repair_wall_ns_sum += repair_wall_ns;
      tls_stats.repair_cpu_ns_sum += repair_cpu_ns;
      if (repair_wall_ns > tls_stats.repair_wall_ns_max) tls_stats.repair_wall_ns_max = repair_wall_ns;
      if (repair_cpu_ns > tls_stats.repair_cpu_ns_max) tls_stats.repair_cpu_ns_max = repair_cpu_ns;
      
      if (repair_reason & REPAIR_REASON_FULL_BITMAP) tls_stats.repair_reason_full_bitmap++;
      if (repair_reason & REPAIR_REASON_LIST_MISMATCH) tls_stats.repair_reason_list_mismatch++;
      if (repair_reason == 0) tls_stats.repair_reason_other++;
#endif
      
      s = es->partial.head;  /* Try next partial slab */
    }
    if (!s) {
      /* No partial slab available. Release lock, allocate new slab, reacquire lock.
       * This avoids holding sc->lock while calling new_slab() → cache_pop() → cache_lock.
       * Small race window: another thread might add a slab to partial list while
       * we're unlocked, but that's fine—we'll just have one extra slab. */
      UNLOCK_WITH_RANK(&sc->lock);
      
      new_s = new_slab(a, sc, epoch);  /* Cache hit or mmap + setup */
      if (!new_s) {
        RECORD_SAMPLE();
        return NULL;  /* Out of memory */
      }
      
      LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");  /* Reacquire */
      
      /* Check again if partial list is still empty (another thread might have added one) */
      s = es->partial.head;
      if (!s) {
        /* Still empty, use our newly allocated slab */
        s = new_s;
        s->list_id = SLAB_LIST_PARTIAL;
        list_push_back(&es->partial, s);
        sc->total_slabs++;  /* Lifetime slab counter */
        new_s = NULL;  /* Ownership transferred to list */
      } else {
        /* Race: another thread added a slab while we were unlocked.
         * Use their slab, and we'll cache our new_s after releasing the lock. */
      }
    }

    /* Publish selected slab for lock-free fast path.
     * Release ordering ensures slab initialization is visible before publication. */
    assert(s->list_id == SLAB_LIST_PARTIAL);
    /* IMPORTANT: mark published before exposing via current_partial */
    s->was_published = true;
    atomic_store_explicit(&es->current_partial, s, memory_order_release);

    UNLOCK_WITH_RANK(&sc->lock);
    
    /* If we allocated a slab but didn't need it (race condition), recycle it.
     * cache_push() will return it to the cache for future use. */
    if (new_s) {
      cache_push(sc, new_s);
    }

    /* Allocate from the slab we just published.
     * Should almost always succeed since we just picked/created this slab. */
    uint32_t prev_fc = 0;
    uint32_t retries = 0;
    uint32_t idx = slab_alloc_slot_atomic(s, sc, &prev_fc, &retries);
    
    if (idx == UINT32_MAX) {
      /* Race: slab filled between our publish and allocation attempt.
       * Loop back and try again with a different slab. */
      continue;
    }
    
    /* Phase 2.2: Record successful allocation + CAS retries */
    uint64_t prev_attempts =
        atomic_fetch_add_explicit(&sc->bitmap_alloc_attempts, 1, memory_order_relaxed);
    uint64_t cur_attempts = prev_attempts + 1;
    
    if (retries > 0) {
      atomic_fetch_add_explicit(&sc->bitmap_alloc_cas_retries, retries, memory_order_relaxed);
#ifdef ENABLE_LABEL_CONTENTION
      /* Phase 2.3: Attribute CAS retries to current label */
      uint8_t lid = current_label_id(sc->parent_alloc);
      atomic_fetch_add_explicit(&sc->bitmap_alloc_cas_retries_by_label[lid], retries, memory_order_relaxed);
#endif
    }
    
    /* Phase 2.2+: Adaptive controller heartbeat (every 2^18 successful allocs) */
    if ((cur_attempts & ((1u << 18) - 1u)) == 0u) {
      scan_adapt_check(sc);
    }
    
    /* Phase 2.1: If allocating from empty slab, decrement empty counter */
    if (prev_fc == s->object_count) {
      atomic_fetch_sub_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
    }

    /* Check if slab is now full. We check the *actual* final free_count rather than
     * relying solely on prev_fc==1, to handle cases where free_count diverged from
     * bitmap state (e.g., due to concurrent operations or stale handle frees). */
    uint32_t final_fc = atomic_load_explicit(&s->free_count, memory_order_acquire);
    if (final_fc == 0 || prev_fc == 1) {
      LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");  /* Phase 2.2: Trylock probe (hot path) */
      if (s->list_id == SLAB_LIST_PARTIAL) {
        /* Double-check free_count under lock to avoid spurious transitions */
        final_fc = atomic_load_explicit(&s->free_count, memory_order_relaxed);
        if (final_fc == 0) {
          atomic_fetch_add_explicit(&sc->list_move_partial_to_full, 1, memory_order_relaxed);
          list_remove(&es->partial, s);
          s->list_id = SLAB_LIST_FULL;
          list_push_back(&es->full, s);
          
          /* Publish next partial if available */
          Slab* next = es->partial.head;
          if (next) {
            next->was_published = true;
          }
          atomic_store_explicit(&es->current_partial, next, memory_order_release);
        }
      }
      UNLOCK_WITH_RANK(&sc->lock);
    }

    void* p = slab_slot_ptr(s, idx);
    if (out) {
      *out = encode_handle(s, &a->reg, idx, (uint32_t)ci);
    }
    
#if ENABLE_DIAGNOSTIC_COUNTERS
    /* Track live bytes for slow path allocation success */
    atomic_fetch_add_explicit(&sc->live_bytes, sc->object_size, memory_order_relaxed);
#endif
    
    RECORD_SAMPLE();
    return p;
  }
  #undef RECORD_SAMPLE
}

/* Free an object using its handle.
 *
 * Handle-based free is safe:
 * - Handles are validated through registry (ABA protection via generation counters)
 * - Slabs stay mapped even after recycling, so validation never crashes
 * - Double-free detection via atomic bitmap check
 *
 * Returns false if handle is invalid, stale, or already freed.
 * Returns true on successful free.
 *
 * Handles lifecycle transitions:
 * - If slab becomes empty AND is on FULL list: recycle to cache (conservative recycling)
 * - If slab was full and now has free space: move FULL→PARTIAL
 * - Empty slabs on PARTIAL list are NOT recycled (they may be held by lock-free threads)
 */
bool free_obj(SlabAllocator* a, SlabHandle h) {
  if (h == 0) return false;  /* NULL handle */
  
  /* Decode handle fields: slab_id (registry key), generation (ABA check),
   * slot (which slot to free), size_class (which allocator). */
  uint32_t slab_id, slot, size_class, gen;
  handle_unpack(h, &slab_id, &gen, &slot, &size_class);
  
  /* Bounds check catches handles with invalid version bits or corrupted fields */
  if (slab_id == UINT32_MAX || size_class >= (uint32_t)k_num_classes) return false;

#if ENABLE_TLS_CACHE
  /* TLS free caching removed due to metadata divergence problem:
   * 
   * If TLS caches frees locally without updating global bitmap/free_count,
   * the global allocator believes slots are still allocated. This causes:
   * - Slabs appear artificially full (free_count stays at 0)
   * - Global selection logic makes wrong decisions
   * - Zombie partial slabs (free_count=0, but slots available in TLS)
   * 
   * Option A: Update global state during TLS free (makes handles "hints" - can be stolen)
   * Option B: Per-thread slab ownership with explicit reservation (major redesign)
   * 
   * Current choice: TLS alloc-only caching. Frees always update global state.
   * This avoids metadata divergence while preserving alloc fast path benefits. */
#endif

  SizeClassAlloc* sc = &a->classes[size_class];
  
  /* Validate through registry: checks generation counter for ABA safety.
   * Returns NULL if slab_id is out of bounds, generation mismatches, or slab was unmapped. */
  Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
  if (!s) return false;  /* Handle is stale or invalid */
  
  /* Paranoid magic check (should never fail if registry validation passed) */
  if (atomic_load_explicit(&s->magic, memory_order_relaxed) != SLAB_MAGIC) return false;
  
  /* Look up epoch state for this slab's epoch */
  uint32_t epoch = s->epoch_id;
  if (epoch >= a->epoch_count) return false;
  EpochState* es = get_epoch_state(sc, epoch);

  /* Free the slot atomically.
   * Returns false if slot was already free (double-free), true on success.
   * Outputs prev_fc (previous free_count) for transition detection. */
  uint32_t prev_fc = 0;
  uint32_t retries = 0;
  if (!slab_free_slot_atomic(s, slot, &prev_fc, &retries)) return false;
  
  /* Record free and contention metrics */
  atomic_fetch_add_explicit(&sc->bitmap_free_attempts, 1, memory_order_relaxed);
  if (retries > 0) {
    atomic_fetch_add_explicit(&sc->bitmap_free_cas_retries, retries, memory_order_relaxed);
  }
  
#if ENABLE_DIAGNOSTIC_COUNTERS
  /* Track live bytes decrement (only reached after successful free at line 1444) */
  atomic_fetch_sub_explicit(&sc->live_bytes, sc->object_size, memory_order_relaxed);
#endif
  
  uint32_t new_fc = prev_fc + 1;  /* New free_count after our free */

  /* Check if slab just became fully empty (all slots free).
   * 
   * SAFETY: Do NOT immediately recycle empty slabs!
   * Other threads may still hold handles to slots in this slab.
   * Recycling would invalidate those handles (generation mismatch).
   * 
   * Instead: Keep empty slabs on the partial list until epoch_close()
   * or explicit cleanup. This ensures all outstanding handles remain valid.
   * 
   * The empty_partial_count tracks these for potential trimming during
   * epoch_close() when it's safe (no outstanding allocations from that epoch).
   */
  if (new_fc == s->object_count) {
    /* Slab is now fully empty. Two cases:
     * 1. Was on FULL list → move to PARTIAL (so it can be reused)
     * 2. Was on PARTIAL list → already correct, just increment empty counter
     */
    if (s->list_id == SLAB_LIST_FULL) {
      LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");
      
      /* Move from full→partial */
      list_remove(&es->full, s);
      list_push_back(&es->partial, s);
      s->list_id = SLAB_LIST_PARTIAL;
      
      /* Track as empty partial */
      atomic_fetch_add_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
      
      UNLOCK_WITH_RANK(&sc->lock);
    } else if (s->list_id == SLAB_LIST_PARTIAL) {
      /* Already on partial list, just became fully empty */
      atomic_fetch_add_explicit(&es->empty_partial_count, 1, memory_order_relaxed);
    }
    
    return true;
  }

  /* Transition detection: did we just free the last allocated slot?
   * prev_fc==0 means slab was full before our free, now it has one free slot.
   * Must move slab from FULL to PARTIAL list so it can be allocated from again. */
  if (prev_fc == 0) {
    LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");  /* Need mutex to mutate lists */
    
    /* Re-check list membership under lock (defensive against races) */
    if (s->list_id == SLAB_LIST_FULL) {
      atomic_fetch_add_explicit(&sc->list_move_full_to_partial, 1, memory_order_relaxed);
      list_remove(&es->full, s);
      s->list_id = SLAB_LIST_PARTIAL;
      list_push_back(&es->partial, s);
      
      /* Try to publish this slab as current_partial if it's currently NULL.
       * This helps other threads avoid the slow path. If CAS fails, another
       * thread already published a different slab—that's fine. */
      assert(s->list_id == SLAB_LIST_PARTIAL);
      /* Mark as published even if CAS fails (monotonic safety flag). */
      s->was_published = true;
      atomic_fetch_add_explicit(&sc->current_partial_cas_attempts, 1, memory_order_relaxed);
      Slab* expected = NULL;
      bool swapped = atomic_compare_exchange_strong_explicit(
        &es->current_partial, &expected, s,
        memory_order_release, memory_order_relaxed);
      if (!swapped) {
        atomic_fetch_add_explicit(&sc->current_partial_cas_failures, 1, memory_order_relaxed);
      }
    }
    UNLOCK_WITH_RANK(&sc->lock);
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

    LOCK_WITHOUT_PROBE(&sc->lock, LOCK_RANK_SIZE_CLASS, "sc->lock");

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

    UNLOCK_WITH_RANK(&sc->lock);
    pthread_mutex_destroy(&sc->lock);

    /* Drain slab cache (now storing CachedSlab entries) */
    LOCK_WITHOUT_PROBE(&sc->cache_lock, LOCK_RANK_CACHE, "sc->cache_lock");
    for (size_t j = 0; j < sc->cache_size; j++) {
#if ENABLE_DIAGNOSTIC_COUNTERS
      /* Decrement committed_bytes before munmap for accurate cleanup tracking */
      atomic_fetch_sub_explicit(&sc->committed_bytes, SLAB_PAGE_SIZE, memory_order_relaxed);
#endif
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
    
    UNLOCK_WITH_RANK(&sc->cache_lock);
    pthread_mutex_destroy(&sc->cache_lock);
  }
  
  /* Destroy label lock */
  pthread_mutex_destroy(&a->epoch_label_lock);
  
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

/* Get current active epoch ID.
 *
 * Returns ring index (0-15), not the raw monotonic counter.
 * This is the epoch where new allocations go.
 */
EpochId epoch_current(SlabAllocator* a) {
  uint32_t raw = atomic_load_explicit(&a->current_epoch, memory_order_relaxed);
  return raw % a->epoch_count;  /* Modulo wraps to ring: 16→0, 17→1, etc. */
}

/* Advance to next epoch (rotate ring buffer).
 *
 * Three-phase operation:
 * 1. Mark old epoch CLOSING (draining, no new allocations)
 * 2. Mark new epoch ACTIVE (ready for allocations)
 * 3. Null current_partial pointers to prevent fast-path allocations into old epoch
 *
 * Timeline after advance:
 * - Old epoch (now CLOSING): Frees continue, empty slabs recycled aggressively
 * - New epoch (now ACTIVE): Accepts allocations, fills fresh slabs
 *
 * Typical usage: Call every ~1 second to rotate generations.
 * With 16 epochs, this gives ~16 seconds for objects to drain before wraparound.
 *
 * Ring wraparound: After epoch 15, advances to epoch 0 (overwrites oldest generation).
 * This is safe because applications should have drained old epochs via epoch_close().
 */
void epoch_advance(SlabAllocator* a) {
  /* Atomically increment and capture both old and new epoch IDs.
   * current_epoch is a monotonic counter (never decreases), we modulo to get ring index. */
  uint32_t old_epoch_raw = atomic_fetch_add_explicit(&a->current_epoch, 1, memory_order_relaxed);
  uint32_t old_epoch = old_epoch_raw % a->epoch_count;      /* Ring index: 0-15 */
  uint32_t new_epoch = (old_epoch_raw + 1) % a->epoch_count;  /* Next ring slot */
  
  /* Phase 1: Close old epoch.
   * Mark CLOSING to reject new allocations (checked in alloc_obj_epoch).
   * Relaxed ordering is sufficient—allocation fast path will eventually observe it. */
  atomic_store_explicit(&a->epoch_state[old_epoch], EPOCH_CLOSING, memory_order_relaxed);
  
  /* Phase 2: Open new epoch.
   * Mark ACTIVE to accept allocations. On wraparound, this overwrites CLOSING
   * state from 16 rotations ago (epoch 0 after 15→0 wrap). */
  atomic_store_explicit(&a->epoch_state[new_epoch], EPOCH_ACTIVE, memory_order_relaxed);
  
  /* Stamp monotonic era for observability.
   * Helps distinguish "epoch 5 at era 100" from "epoch 5 at era 116" after wraparound.
   * Useful for correlating allocator events with application logs. */
  uint64_t era = atomic_fetch_add_explicit(&a->epoch_era_counter, 1, memory_order_relaxed);
  atomic_store_explicit(&a->epoch_era[new_epoch], era + 1, memory_order_release);
  
  /* Reset metadata for new epoch (overwrites data from previous rotation).
   * Timestamp lets us measure epoch lifetime. Label cleared for fresh annotation. */
  a->epoch_meta[new_epoch].open_since_ns = now_ns();
  atomic_store_explicit(&a->epoch_meta[new_epoch].domain_refcount, 0, memory_order_relaxed);
  a->epoch_meta[new_epoch].label[0] = '\0';
  
  /* Phase 3: Null current_partial for old epoch across all size classes.
   * Prevents fast-path threads from allocating into CLOSING epoch.
   * Threads will see NULL and fall back to slow path, which checks epoch state. */
  for (size_t i = 0; i < k_num_classes; i++) {
    EpochState* es = &a->classes[i].epochs[old_epoch];
    atomic_store_explicit(&es->current_partial, NULL, memory_order_release);
  }
  
  /* Old epoch now drains passively.
   * - Frees continue normally (decrement free_count, transition tracking)
   * - Empty slabs recycled immediately (CLOSING triggers aggressive recycling in free_obj)
   * - No new allocations refill partially-empty slabs (CLOSING check rejects them)
   *
   * To force immediate reclamation, call epoch_close(old_epoch) explicitly. */
}

/* Force immediate reclamation of an epoch's memory.
 *
 * Unlike epoch_advance (which passively drains), epoch_close actively scans
 * for empty slabs and recycles them immediately. This triggers RSS drops when
 * ENABLE_RSS_RECLAMATION is enabled.
 *
 * Two-phase reclamation:
 * 1. Mark epoch CLOSING (reject new allocations)
 * 2. Proactively scan partial/full lists for already-empty slabs
 *
 * Use cases:
 * - End of request: Close request epoch to reclaim memory immediately
 * - End of batch: Force RSS drop after processing batch
 * - Memory pressure: Reclaim idle generations to stay under RSS limit
 *
 * Performance characteristics:
 * - Scans all slabs in epoch (O(n) where n = total slabs)
 * - Recycling happens outside lock (madvise doesn't block allocations)
 * - Typical latency: 50-500µs depending on slab count
 *
 * Difference from epoch_advance:
 * - epoch_advance: Rotates to next epoch (implicit close of old epoch)
 * - epoch_close: Explicit close without rotation (target specific epoch)
 *
 * Safety: Can be called on any epoch, even current or future ones.
 * Calling on current epoch stops further allocation into it.
 */
void epoch_close(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return;
  
  /* Capture start time for latency telemetry.
   * Helps answer "how long does epoch_close take?" for capacity planning. */
  struct timespec start_ts;
  clock_gettime(CLOCK_MONOTONIC, &start_ts);
  uint64_t start_ns = (uint64_t)start_ts.tv_sec * 1000000000ULL + (uint64_t)start_ts.tv_nsec;
  
  /* Capture RSS before closing to measure reclamation effectiveness.
   * Delta (before - after) shows MB returned to OS. */
  uint64_t rss_before = read_rss_bytes_linux();
  a->epoch_meta[epoch].rss_before_close = rss_before;
  
  /* Phase 1: Mark epoch CLOSING to reject new allocations.
   * This is the gate that enables aggressive recycling:
   * - free_obj() checks epoch state and recycles empty slabs immediately
   * - alloc_obj_epoch() checks epoch state and rejects allocations
   *
   * CRITICAL: Use memory_order_release to ensure all threads observe CLOSING
   * before we flush their TLS caches. With relaxed ordering, threads could
   * refill caches from this epoch even after flush completes (race condition). */
  atomic_store_explicit(&a->epoch_state[epoch], EPOCH_CLOSING, memory_order_release);
  
#if ENABLE_TLS_CACHE
  /* TLS Cache: Flush all handles from this epoch across all threads.
   * 
   * CRITICAL: Must happen BEFORE slab reclamation scanning to ensure accurate
   * free_count and empty slab detection. If TLS caches hold handles from this
   * epoch, those handles represent allocated slots that haven't been freed yet.
   * 
   * Thread registry lock ensures no threads modify their TLS caches during flush. */
  tls_flush_epoch_all_threads(a, epoch);
#endif
  
  /* Phase 2: Proactively scan for already-empty slabs and recycle them.
   *
   * Problem: Slabs that became empty BEFORE epoch_close() was called are sitting
   * on the partial list. The free_obj() reactive path only recycles slabs at the
   * moment they transition to empty, so we need proactive scanning.
   *
   * Without this scan, empty slabs would stay allocated until the next free into
   * that epoch, causing RSS to stay elevated unnecessarily.
   *
   * Algorithm complexity: O(n) where n = total slabs in epoch across all size classes.
   * Lock hold time: O(n) for scan + unlink, but madvise happens outside lock.
   */
  for (size_t i = 0; i < k_num_classes; i++) {
    SizeClassAlloc* sc = &a->classes[i];
    EpochState* es = &sc->epochs[epoch];
    
    /* Null current_partial to prevent fast-path allocations into this epoch.
     * Threads will see NULL and fall to slow path, which checks CLOSING state. */
    atomic_store_explicit(&es->current_partial, NULL, memory_order_release);
    
    /* Scan both partial and full lists for empty slabs.
     *
     * Strategy: Two-pass algorithm to avoid O(n²) restart pattern.
     * Pass 1: Count empty slabs (quick, read-only)
     * Pass 2: Collect empty slabs into array (remove from lists)
     * Pass 3: Recycle outside lock (call cache_push, which does madvise)
     *
     * This minimizes lock hold time—no syscalls inside critical section. */
    LOCK_WITH_PROBE(&sc->lock, sc, LOCK_RANK_SIZE_CLASS, "sc->lock");
    
    /* Pass 1: Count empty slabs for array allocation */
    size_t empty_count = 0;
    size_t scanned_count = 0;
    for (Slab* s = es->partial.head; s; s = s->next) {
      scanned_count++;
      /* Empty check: free_count == object_count means all slots free */
      if (atomic_load_explicit(&s->free_count, memory_order_relaxed) == s->object_count) {
        empty_count++;
      }
    }
    for (Slab* s = es->full.head; s; s = s->next) {
      scanned_count++;
      if (atomic_load_explicit(&s->free_count, memory_order_relaxed) == s->object_count) {
        empty_count++;
      }
    }
    
    /* Update telemetry: how many slabs scanned, how many recycled */
    atomic_fetch_add_explicit(&sc->epoch_close_scanned_slabs, scanned_count, memory_order_relaxed);
    if (empty_count > 0) {
      atomic_fetch_add_explicit(&sc->epoch_close_recycled_slabs, empty_count, memory_order_relaxed);
    }
    
    if (empty_count > 0) {
      /* Pass 2: Allocate temporary array to collect empty slabs.
       *
       * Optimization: Use stack allocation for common case (<= 32 slabs).
       * Avoids malloc overhead when closing small epochs. Fall back to heap
       * for large epochs (e.g., long-running batch processing). */
      Slab** empty_slabs = NULL;
      Slab* stack_buf[32];
      
      if (empty_count <= 32) {
        empty_slabs = stack_buf;  /* Fast path: stack allocation */
      } else {
        empty_slabs = (Slab**)malloc(empty_count * sizeof(Slab*));
        if (!empty_slabs) {
          /* Malloc failed. Skip recycling for this size class.
           * Memory stays allocated but epoch still marked CLOSING. */
          UNLOCK_WITH_RANK(&sc->lock);
          return;
        }
      }
      
      /* Pass 2: Collect empty slabs from both lists.
       * Unlink from lists and mark SLAB_LIST_NONE so they can be recycled safely. */
      size_t idx = 0;
      Slab* cur = es->partial.head;
      while (cur) {
        Slab* next = cur->next;  /* Save next before unlinking */
        if (atomic_load_explicit(&cur->free_count, memory_order_relaxed) == cur->object_count) {
          list_remove(&es->partial, cur);
          cur->list_id = SLAB_LIST_NONE;
          sc->total_slabs--;
          empty_slabs[idx++] = cur;
        }
        cur = next;
      }
      
      /* Also scan full list (slabs can be on full list if they were filled,
       * then fully freed but not yet moved to partial). */
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
      
      UNLOCK_WITH_RANK(&sc->lock);
      
      /* Pass 3: Recycle all collected slabs immediately.
       * cache_push() will skip madvise if slab was ever published lock-free. */
      for (size_t j = 0; j < idx; j++) {
        cache_push(sc, empty_slabs[j]);
      }
      
      /* Clean up heap allocation if we used it */
      if (empty_slabs != stack_buf) {
        free(empty_slabs);
      }
    } else {
      /* No empty slabs found, just release lock */
      UNLOCK_WITH_RANK(&sc->lock);
    }
  }
  
  /* Phase 3: Measure reclamation impact.
   *
   * Capture RSS after recycling to quantify how much memory was returned.
   * Delta (rss_before - rss_after) shows effectiveness of epoch_close.
   * Applications can export this to track memory efficiency. */
  uint64_t rss_after = read_rss_bytes_linux();
  a->epoch_meta[epoch].rss_after_close = rss_after;
  
  /* Capture end time and update latency telemetry.
   * Helps capacity planning: "How long does epoch_close take?" */
  struct timespec end_ts;
  clock_gettime(CLOCK_MONOTONIC, &end_ts);
  uint64_t end_ns = (uint64_t)end_ts.tv_sec * 1000000000ULL + (uint64_t)end_ts.tv_nsec;
  uint64_t elapsed_ns = end_ns - start_ns;
  
  /* Update per-class counters (aggregate, not per-epoch).
   * Same elapsed_ns added to all size classes—represents total epoch_close cost. */
  for (size_t i = 0; i < k_num_classes; i++) {
    atomic_fetch_add_explicit(&a->classes[i].epoch_close_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->classes[i].epoch_close_total_ns, elapsed_ns, memory_order_relaxed);
  }
  
  /* Epoch now fully closed and drained.
   *
   * Future frees into this epoch:
   * - Continue normally (bitmap clear, free_count increment)
   * - Empty slabs recycled immediately (CLOSING triggers aggressive recycling)
   *
   * Future allocations into this epoch:
   * - Rejected (alloc_obj_epoch checks CLOSING state)
   *
   * Expected RSS behavior with ENABLE_RSS_RECLAMATION=1:
   * - RSS drops immediately after epoch_close returns
   * - Delta visible in rss_before_close - rss_after_close
   * - Physical pages returned to kernel, available for other processes */
}

/* ------------------------------ Phase 2.3: Semantic Attribution APIs ------------------------------ */

void slab_epoch_set_label(SlabAllocator* a, EpochId epoch, const char* label) {
  if (!a || epoch >= a->epoch_count || !label) return;
  
  EpochMetadata* meta = &a->epoch_meta[epoch];
  
  /* Phase 2.3: Assign or reuse label ID (bounded cardinality) */
  LOCK_WITHOUT_PROBE(&a->label_registry.lock, LOCK_RANK_LABEL_REGISTRY, "label_registry.lock");
  
  uint8_t label_id = 0;  /* Default: unlabeled */
  
  /* Search for existing label (reuse ID) */
  for (uint8_t i = 1; i < a->label_registry.count; i++) {
    if (strncmp(a->label_registry.labels[i], label, 31) == 0) {
      label_id = i;
      break;
    }
  }
  
  /* If not found and space available, allocate new ID */
  if (label_id == 0 && a->label_registry.count < MAX_LABEL_IDS) {
    label_id = a->label_registry.count++;
    strncpy(a->label_registry.labels[label_id], label, 31);
    a->label_registry.labels[label_id][31] = '\0';
  }
  
  /* If registry full, label_id remains 0 (unlabeled / other bucket) */
  
  UNLOCK_WITH_RANK(&a->label_registry.lock);
  
  /* Update epoch metadata (no lock needed, rarely written) */
  LOCK_WITHOUT_PROBE(&a->epoch_label_lock, LOCK_RANK_EPOCH_LABEL, "epoch_label_lock");
  strncpy(meta->label, label, sizeof(meta->label) - 1);
  meta->label[sizeof(meta->label) - 1] = '\0';  /* Ensure null termination */
  meta->label_id = label_id;  /* Phase 2.3: Store stable ID */
  UNLOCK_WITH_RANK(&a->epoch_label_lock);
}

void slab_epoch_inc_refcount(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return;
  
  atomic_fetch_add_explicit(&a->epoch_meta[epoch].domain_refcount, 1, memory_order_relaxed);
}

void slab_epoch_dec_refcount(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return;
  
  /* Atomic decrement with saturation at 0 (prevent underflow) */
  uint64_t prev = atomic_load_explicit(&a->epoch_meta[epoch].domain_refcount, memory_order_relaxed);
  while (prev > 0) {
    if (atomic_compare_exchange_weak_explicit(
            &a->epoch_meta[epoch].domain_refcount, &prev, prev - 1,
            memory_order_relaxed, memory_order_relaxed)) {
      break;  /* Success */
    }
    /* CAS failed, prev was updated with current value, retry */
  }
}

uint64_t slab_epoch_get_refcount(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return 0;
  
  return atomic_load_explicit(&a->epoch_meta[epoch].domain_refcount, memory_order_relaxed);
}

/*
 * Note: Tests moved to smoke_tests.c (Phase 1.6 file organization)
 * This file now contains only the allocator implementation.
 */
