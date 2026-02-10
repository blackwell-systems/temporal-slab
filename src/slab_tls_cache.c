#include "slab_alloc_internal.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>

#if ENABLE_TLS_CACHE

__thread TLSCache _tls_cache[8];
__thread bool _tls_initialized = false;
__thread bool _tls_in_refill = false;  /* Prevent recursion during refill */
__thread bool _tls_in_flush = false;   /* Prevent recursion during flush */
__thread bool _tls_bypass = false;     /* Bypass TLS during flush to prevent deadlock */

ThreadRegistryEntry _thread_registry[MAX_THREADS];
uint32_t _thread_count = 0;
pthread_mutex_t _thread_registry_lock = PTHREAD_MUTEX_INITIALIZER;

void tls_init_thread(void) {
    if (_tls_initialized) return;
    
    memset(_tls_cache, 0, sizeof(_tls_cache));
    
    pthread_mutex_lock(&_thread_registry_lock);
    
    if (_thread_count < MAX_THREADS) {
        _thread_registry[_thread_count].tid = pthread_self();
        _thread_registry[_thread_count].caches = _tls_cache;
        _thread_count++;
    }
    
    pthread_mutex_unlock(&_thread_registry_lock);
    
    _tls_initialized = true;
}

/* Check if TLS alloc should be bypassed due to low hit rate */
static inline int tls_should_bypass_alloc(TLSCache* tls) {
    if (tls->bypass_alloc) {
        if (tls->window_ops >= tls->bypass_ends_at) {
            tls->bypass_alloc = 0;  /* Re-enable after bypass window */
            tls->window_hits = 0;   /* Reset window state for clean measurement */
            tls->window_ops = 0;
        }
        return 1;
    }
    return 0;
}

/* Record alloc attempt and check if we should enable bypass */
static inline void tls_record_alloc(TLSCache* tls, int hit) {
    tls->window_ops++;
    if (hit) tls->window_hits++;
    
    /* Check hit rate every TLS_WINDOW_OPS operations */
    if ((tls->window_ops & (TLS_WINDOW_OPS - 1)) == 0) {
        uint32_t hits = tls->window_hits;
        uint32_t pct = (hits * 100) / TLS_WINDOW_OPS;
        
        /* Warmup protection: Don't enable bypass during first measurement window.
         * Allow at least one refill opportunity before deciding TLS is ineffective. */
        if (tls->window_ops > TLS_WINDOW_OPS && pct < TLS_MIN_HIT_PCT) {
            /* Hit rate too low - disable alloc (free caching already disabled) */
            tls->bypass_alloc = 1;
            tls->bypass_ends_at = tls->window_ops + TLS_BYPASS_OPS;
        }
        
        tls->window_hits = 0;
    }
}

void* tls_try_alloc(SlabAllocator* a, uint32_t sc, uint32_t epoch_id, SlabHandle* out_h) {
    if (_tls_bypass) return NULL;  /* Skip TLS during epoch flush */
    
    if (!_tls_initialized) {
        tls_init_thread();
    }
    
    TLSCache* tls = &_tls_cache[sc];
    tls->tls_alloc_attempts++;
    
    /* HARD BYPASS: first instruction after cache lookup */
    if (tls->bypass_alloc) {
        tls->tls_alloc_bypassed++;
        return NULL;
    }
    
    /* Fast path: TLS cache hit */
    if (tls->count > 0) {
        TLSItem item = tls->items[tls->count - 1];
        
#if TLS_DEBUG_VALIDATE
        /* Debug: assert one-shot invariant */
        if (tls->tls_popped > tls->tls_refilled_added) {
            fprintf(stderr, "TLS INVARIANT VIOLATION: popped=%u > added=%u\n",
                    tls->tls_popped, tls->tls_refilled_added);
            abort();
        }
#endif
        
        /* Epoch check: reject stale cached handles from closed epochs */
        if (item.h != 0 && item.epoch_id == epoch_id) {
            /* Valid entry - consume it (one-shot pop) */
            tls->count--;
            tls->tls_popped++;
            *out_h = item.h;
            tls->tls_alloc_hits++;
            tls_record_alloc(tls, 1);  /* Count as hit */
            
#if TLS_DEBUG_VALIDATE
            /* Poison consumed entry to detect double-use */
            tls->items[tls->count].h = 0xDEADBEEFDEADBEEFULL;
            tls->items[tls->count].p = (void*)0xDEADBEEFDEADBEEFULL;
            tls->items[tls->count].epoch_id = 0xDEADBEEF;
#endif
            
            return item.p;  /* Zero-overhead return: no unpack, no validation */
        }
        
        /* Entry failed validation (wrong epoch) - discard it and treat as miss */
        if (tls->count > 0 && item.epoch_id != epoch_id) {
            tls->count--;
            tls->tls_popped++;
            tls->tls_epoch_rejects++;
        }
    }
    
    /* Cache miss: fall through to global allocator */
    tls_record_alloc(tls, 0);
    return NULL;
}

bool tls_try_free(SlabAllocator* a, uint32_t sc, SlabHandle h) {
    /* TLS free caching disabled - always return false to use global free path.
     * 
     * Why: Caching frees locally creates metadata divergence. The global allocator
     * believes slots are allocated when TLS has them cached, causing:
     * - Slabs appear artificially full (free_count=0, but slots in TLS cache)
     * - Zombie partial slabs (metadata lies about availability)
     * - Global selection logic breaks
     * 
     * Solution: TLS alloc-only caching. Frees always update global state. */
    (void)a; (void)sc; (void)h;
    return false;
}

void tls_refill(SlabAllocator* a, uint32_t sc, uint32_t epoch_id) {
    TLSCache* tls = &_tls_cache[sc];
    
    /* Set flag to prevent recursive TLS lookup during refill */
    _tls_in_refill = true;
    
    for (uint32_t i = 0; i < TLS_REFILL_BATCH && tls->count < TLS_CACHE_CAP; i++) {
        SlabHandle h;
        SizeClassAlloc* sca = &a->classes[sc];
        uint32_t size = sca->object_size;
        void* ptr = alloc_obj_epoch(a, size, epoch_id, &h);
        
        if (!ptr) break;
        
        /* Re-check epoch state before caching.
         * Race: epoch_close() could have marked CLOSING after our initial check.
         * If so, free immediately instead of caching to prevent zombie slabs. */
        uint32_t state = atomic_load_explicit(&a->epoch_state[epoch_id], memory_order_acquire);
        if (state != EPOCH_ACTIVE) {
            free_obj(a, h);
            break;
        }
        
        /* Store handle, pointer, and epoch for validation at pop time */
        tls->items[tls->count].h = h;
        tls->items[tls->count].p = ptr;
        tls->items[tls->count].epoch_id = epoch_id;
        tls->count++;
        tls->tls_refilled_added++;
    }
    
    tls->tls_alloc_refills++;
    _tls_in_refill = false;
}

void tls_flush_batch(SlabAllocator* a, uint32_t sc, uint32_t batch_size) {
    TLSCache* tls = &_tls_cache[sc];
    
    uint32_t to_flush = (batch_size < tls->count) ? batch_size : tls->count;
    
    for (uint32_t i = 0; i < to_flush; i++) {
        free_obj(a, tls->items[--tls->count].h);
    }
}

void tls_flush_epoch_all_threads(SlabAllocator* a, uint32_t epoch_id) {
    pthread_mutex_lock(&_thread_registry_lock);
    
    /* Bypass TLS caching during flush to prevent deadlock.
     * If free_obj() tried to call tls_try_free(), it would attempt to acquire
     * _thread_registry_lock again (for thread init), causing deadlock. */
    _tls_bypass = true;
    
    for (uint32_t t = 0; t < _thread_count; t++) {
        TLSCache* caches = _thread_registry[t].caches;
        
        for (uint32_t sc = 0; sc < 8; sc++) {
            TLSCache* tls = &caches[sc];
            
            /* Flush handles from this epoch */
            for (uint32_t i = 0; i < tls->count; ) {
                uint32_t item_epoch = (uint32_t)(tls->items[i].h >> 32);
                if (item_epoch == epoch_id) {
                    free_obj(a, tls->items[i].h);
                    
                    /* Remove by swapping with last item */
                    tls->items[i] = tls->items[--tls->count];
                } else {
                    i++;
                }
            }
        }
    }
    
    _tls_bypass = false;
    pthread_mutex_unlock(&_thread_registry_lock);
}

void tls_print_stats(void) {
    pthread_mutex_lock(&_thread_registry_lock);
    
    printf("\n=== TLS Cache Statistics ===\n");
    
    /* Aggregate across all threads and size classes (alloc only) */
    uint64_t total_alloc_attempts = 0;
    uint64_t total_alloc_bypassed = 0;
    uint64_t total_alloc_hits = 0;
    uint64_t total_alloc_refills = 0;
    uint64_t total_popped = 0;
    uint64_t total_refilled_added = 0;
    uint64_t total_epoch_rejects = 0;
    
    for (uint32_t t = 0; t < _thread_count; t++) {
        TLSCache* caches = _thread_registry[t].caches;
        
        for (uint32_t sc = 0; sc < 8; sc++) {
            TLSCache* tls = &caches[sc];
            
            total_alloc_attempts += tls->tls_alloc_attempts;
            total_alloc_bypassed += tls->tls_alloc_bypassed;
            total_alloc_hits += tls->tls_alloc_hits;
            total_alloc_refills += tls->tls_alloc_refills;
            total_popped += tls->tls_popped;
            total_refilled_added += tls->tls_refilled_added;
            total_epoch_rejects += tls->tls_epoch_rejects;
        }
    }
    
    pthread_mutex_unlock(&_thread_registry_lock);
    
    /* Print aggregate summary */
    double hit_rate = total_alloc_attempts > 0 
        ? (100.0 * total_alloc_hits) / total_alloc_attempts 
        : 0.0;
    double bypass_rate = total_alloc_attempts > 0
        ? (100.0 * total_alloc_bypassed) / total_alloc_attempts
        : 0.0;
    
    printf("Alloc: attempts=%lu bypassed=%lu (%.1f%%) hits=%lu (%.1f%%) refills=%lu\n",
           total_alloc_attempts, total_alloc_bypassed, bypass_rate,
           total_alloc_hits, hit_rate, total_alloc_refills);
    printf("Free:  TLS disabled (metadata divergence - see slab_alloc.c:1779)\n");
    printf("\nOne-shot invariant: popped=%lu added=%lu (delta=%ld)\n",
           total_popped, total_refilled_added, 
           (int64_t)total_refilled_added - (int64_t)total_popped);
    printf("Epoch rejects: %lu (stale cached handles discarded)\n", total_epoch_rejects);
    printf("=============================\n\n");
}

#endif /* ENABLE_TLS_CACHE */
