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
            tls->bypass_free = 0;
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
        
        if (pct < TLS_MIN_HIT_PCT) {
            /* Hit rate too low - disable both alloc and free */
            tls->bypass_alloc = 1;
            tls->bypass_free = 1;  /* Also bypass free (no reuse expected) */
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
        uint32_t idx = --tls->count;
        TLSItem item = tls->items[idx];
        
        /* Epoch check: reject stale cached handles from closed epochs */
        if (item.h != 0) {
            uint32_t cached_epoch = (uint32_t)(item.h >> 32);
            if (cached_epoch == epoch_id) {
                *out_h = item.h;
                tls->tls_alloc_hits++;
                tls_record_alloc(tls, 1);  /* Count as hit */
                return item.p;  /* Zero-overhead return: no unpack, no validation */
            }
        }
    }
    
    /* Cache miss: fall through to global allocator */
    tls_record_alloc(tls, 0);
    return NULL;
}

bool tls_try_free(SlabAllocator* a, uint32_t sc, SlabHandle h) {
    if (_tls_bypass) return false;  /* Skip TLS during epoch flush */
    if (_tls_in_refill) return false;  /* Skip TLS during refill (prevents recursion) */
    if (_tls_in_flush) return false;   /* Skip TLS during cache flush (prevents recursion) */
    
    if (!_tls_initialized) {
        tls_init_thread();
    }
    
    TLSCache* tls = &_tls_cache[sc];
    
    /* HARD BYPASS: first instruction after cache lookup */
    if (tls->bypass_free) {
        tls->tls_free_bypassed++;
        return false;
    }
    
    /* Dribble flush: when at high watermark, flush small batch to low watermark */
    if (tls->count >= TLS_FLUSH_HI) {
        _tls_in_flush = true;
        uint32_t target = TLS_FLUSH_LO;
        uint32_t n = TLS_FLUSH_DRIBBLE;
        
        while (n-- && tls->count > target) {
            SlabHandle flush_h = tls->items[--tls->count].h;
            free_obj(a, flush_h);
        }
        
        _tls_in_flush = false;
        tls->tls_free_dribbles++;
    }
    
    /* Cache the freed handle (we need pointer, so validate) */
    uint32_t slab_id, gen, slot, size_class_check;
    handle_unpack(h, &slab_id, &gen, &slot, &size_class_check);
    
    Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
    if (!s) {
        return false;
    }
    
    /* Store both handle and pointer for future fast alloc */
    void* p = slab_slot_ptr(s, slot);
    tls->items[tls->count].h = h;
    tls->items[tls->count].p = p;
    tls->count++;
    tls->tls_free_cached++;
    
    return true;
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
        
        /* Store both handle and pointer for fast alloc hits */
        tls->items[tls->count].h = h;
        tls->items[tls->count].p = ptr;
        tls->count++;
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
    
    /* Aggregate across all threads and size classes */
    uint64_t total_alloc_attempts = 0;
    uint64_t total_alloc_bypassed = 0;
    uint64_t total_alloc_hits = 0;
    uint64_t total_alloc_refills = 0;
    uint64_t total_free_cached = 0;
    uint64_t total_free_dribbles = 0;
    uint64_t total_free_bypassed = 0;
    
    for (uint32_t t = 0; t < _thread_count; t++) {
        TLSCache* caches = _thread_registry[t].caches;
        
        for (uint32_t sc = 0; sc < 8; sc++) {
            TLSCache* tls = &caches[sc];
            
            total_alloc_attempts += tls->tls_alloc_attempts;
            total_alloc_bypassed += tls->tls_alloc_bypassed;
            total_alloc_hits += tls->tls_alloc_hits;
            total_alloc_refills += tls->tls_alloc_refills;
            total_free_cached += tls->tls_free_cached;
            total_free_dribbles += tls->tls_free_dribbles;
            total_free_bypassed += tls->tls_free_bypassed;
        }
    }
    
    pthread_mutex_unlock(&_thread_registry_lock);
    
    /* Print aggregate summary */
    double hit_rate = total_alloc_attempts > 0 
        ? (100.0 * total_alloc_hits) / total_alloc_attempts 
        : 0.0;
    double bypass_rate_alloc = total_alloc_attempts > 0
        ? (100.0 * total_alloc_bypassed) / total_alloc_attempts
        : 0.0;
    double bypass_rate_free = (total_free_cached + total_free_bypassed) > 0
        ? (100.0 * total_free_bypassed) / (total_free_cached + total_free_bypassed)
        : 0.0;
    
    printf("Alloc: attempts=%lu bypassed=%lu (%.1f%%) hits=%lu (%.1f%%) refills=%lu\n",
           total_alloc_attempts, total_alloc_bypassed, bypass_rate_alloc,
           total_alloc_hits, hit_rate, total_alloc_refills);
    printf("Free:  cached=%lu dribbles=%lu bypassed=%lu (%.1f%%)\n",
           total_free_cached, total_free_dribbles, 
           total_free_bypassed, bypass_rate_free);
    printf("=============================\n\n");
}

#endif /* ENABLE_TLS_CACHE */
