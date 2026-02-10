#include "slab_alloc_internal.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>

/* Debug logging causes severe lock contention in multi-threaded scenarios.
 * Only enable for single-threaded debugging. */
#define TLS_DEBUG_LOG 0

#if TLS_DEBUG_LOG
#define TLS_LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)
#else
#define TLS_LOG(...) do { } while(0)
#endif

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

void* tls_try_alloc(SlabAllocator* a, uint32_t sc, uint32_t epoch_id, SlabHandle* out_h) {
    TLS_LOG("[DEBUG] tls_try_alloc: ENTER sc=%u epoch=%u bypass=%d\n", sc, epoch_id, _tls_bypass);
    
    if (_tls_bypass) {
        TLS_LOG("[DEBUG] tls_try_alloc: BYPASS - returning NULL\n");
        return NULL;  /* Skip TLS during flush */
    }
    
    if (!_tls_initialized) {
        TLS_LOG("[DEBUG] tls_try_alloc: initializing thread\n");
        tls_init_thread();
    }
    
    TLSCache* tls = &_tls_cache[sc];
    TLS_LOG("[DEBUG] tls_try_alloc: tls->count=%u\n", tls->count);
    
    if (tls->count > 0) {
        TLS_LOG("[DEBUG] tls_try_alloc: cache has handles, popping\n");
        
        uint32_t idx = --tls->count;
        SlabHandle h = tls->handles[idx];
        uint32_t cached_epoch = tls->epoch_id[idx];
        
        TLS_LOG("[DEBUG] tls_try_alloc: idx=%u handle=0x%lx cached_epoch=%u\n", idx, (unsigned long)h, cached_epoch);
        
        if (cached_epoch == epoch_id) {
            TLS_LOG("[DEBUG] tls_try_alloc: epoch MATCH\n");
            
            *out_h = h;
            
            uint32_t slab_id, gen, slot, size_class;
            handle_unpack(h, &slab_id, &gen, &slot, &size_class);
            
            TLS_LOG("[DEBUG] tls_try_alloc: unpacked slab_id=%u gen=%u slot=%u\n", slab_id, gen, slot);
            
            Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
            if (!s) {
                TLS_LOG("[DEBUG] tls_try_alloc: VALIDATION FAILED - returning NULL\n");
                return NULL;
            }
            
            void* ptr = slab_slot_ptr(s, slot);
            TLS_LOG("[DEBUG] tls_try_alloc: SUCCESS - returning %p\n", ptr);
            return ptr;
        } else {
            TLS_LOG("[DEBUG] tls_try_alloc: epoch MISMATCH (cached=%u requested=%u) - returning NULL\n", cached_epoch, epoch_id);
        }
        
    } else {
        TLS_LOG("[DEBUG] tls_try_alloc: cache EMPTY - returning NULL\n");
    }
    
    return NULL;
}

bool tls_try_free(SlabAllocator* a, uint32_t sc, SlabHandle h) {
    TLS_LOG("[DEBUG] tls_try_free: ENTER sc=%u handle=0x%lx bypass=%d in_flush=%d\n", sc, (unsigned long)h, _tls_bypass, _tls_in_flush);
    
    if (_tls_bypass) return false;  /* Skip TLS during epoch flush */
    if (_tls_in_refill) return false;  /* Skip TLS during refill (prevents recursion) */
    if (_tls_in_flush) return false;   /* Skip TLS during cache flush (prevents recursion) */
    
    if (!_tls_initialized) {
        tls_init_thread();
    }
    
    TLSCache* tls = &_tls_cache[sc];
    TLS_LOG("[DEBUG] tls_try_free: tls->count=%u\n", tls->count);
    
    if (tls->count < TLS_CACHE_SIZE) {
        uint32_t slab_id, gen, slot, size_class_check;
        handle_unpack(h, &slab_id, &gen, &slot, &size_class_check);
        
        Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
        if (!s) {
            TLS_LOG("[DEBUG] tls_try_free: validation failed\n");
            return false;
        }
        
        TLS_LOG("[DEBUG] tls_try_free: cached, count now %u\n", tls->count + 1);
        tls->handles[tls->count] = h;
        tls->epoch_id[tls->count] = s->epoch_id;
        tls->count++;
        return true;
    }
    
    TLS_LOG("[DEBUG] tls_try_free: cache FULL, flushing batch\n");
    _tls_in_flush = true;
    tls_flush_batch(a, sc, TLS_FLUSH_BATCH);
    _tls_in_flush = false;
    TLS_LOG("[DEBUG] tls_try_free: flush complete, count now %u\n", tls->count);
    
    tls->handles[tls->count] = h;
    tls->epoch_id[tls->count] = 0;
    tls->count++;
    TLS_LOG("[DEBUG] tls_try_free: cached after flush\n");
    return true;
}

void tls_refill(SlabAllocator* a, uint32_t sc, uint32_t epoch_id) {
    TLSCache* tls = &_tls_cache[sc];
    
    TLS_LOG("[DEBUG] tls_refill: sc=%u epoch=%u count=%u\n", sc, epoch_id, tls->count);
    
    /* Set flag to prevent recursive TLS lookup during refill */
    _tls_in_refill = true;
    
    for (uint32_t i = 0; i < TLS_REFILL_BATCH && tls->count < TLS_CACHE_SIZE; i++) {
        TLS_LOG("[DEBUG] tls_refill: iteration %u\n", i);
        SlabHandle h;
        SizeClassAlloc* sca = &a->classes[sc];
        uint32_t size = sca->object_size;
        void* ptr = alloc_obj_epoch(a, size, epoch_id, &h);
        
        TLS_LOG("[DEBUG] tls_refill: alloc_obj_epoch returned %p\n", ptr);
        
        if (!ptr) break;
        
        /* Re-check epoch state before caching the handle.
         * Race window: epoch_close() could have marked CLOSING and flushed caches
         * after our initial check but before we got here. If so, free immediately
         * instead of caching. This prevents zombie slabs from cached handles. */
        uint32_t state = atomic_load_explicit(&a->epoch_state[epoch_id], memory_order_acquire);
        TLS_LOG("[DEBUG] tls_refill: epoch state = %u\n", state);
        if (state != EPOCH_ACTIVE) {
            TLS_LOG("[DEBUG] tls_refill: epoch closed, freeing handle\n");
            free_obj(a, h);  /* Epoch closed while we were allocating - don't cache */
            break;
        }
        
        tls->handles[tls->count] = h;
        tls->epoch_id[tls->count] = epoch_id;
        tls->count++;
    }
    
    TLS_LOG("[DEBUG] tls_refill: done, final count=%u\n", tls->count);
    _tls_in_refill = false;
}

void tls_flush_batch(SlabAllocator* a, uint32_t sc, uint32_t batch_size) {
    TLSCache* tls = &_tls_cache[sc];
    
    uint32_t to_flush = (batch_size < tls->count) ? batch_size : tls->count;
    
    for (uint32_t i = 0; i < to_flush; i++) {
        free_obj(a, tls->handles[--tls->count]);
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
            
            for (uint32_t i = 0; i < tls->count; ) {
                if (tls->epoch_id[i] == epoch_id) {
                    free_obj(a, tls->handles[i]);
                    
                    tls->handles[i] = tls->handles[--tls->count];
                    tls->epoch_id[i] = tls->epoch_id[tls->count];
                } else {
                    i++;
                }
            }
        }
    }
    
    _tls_bypass = false;
    pthread_mutex_unlock(&_thread_registry_lock);
}

#endif /* ENABLE_TLS_CACHE */
