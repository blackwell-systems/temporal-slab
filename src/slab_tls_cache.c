#include "slab_alloc_internal.h"
#include <string.h>
#include <errno.h>

#if ENABLE_TLS_CACHE

__thread TLSCache _tls_cache[8];
__thread bool _tls_initialized = false;

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
    if (!_tls_initialized) {
        tls_init_thread();
    }
    
    TLSCache* tls = &_tls_cache[sc];
    
    if (tls->count > 0) {
        uint32_t idx = --tls->count;
        SlabHandle h = tls->handles[idx];
        uint32_t cached_epoch = tls->epoch_id[idx];
        
        if (cached_epoch == epoch_id) {
            *out_h = h;
            
            uint32_t slab_id, gen, slot, size_class;
            handle_unpack(h, &slab_id, &gen, &slot, &size_class);
            
            Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
            if (!s) {
                return NULL;
            }
            
            SizeClassAlloc* sca = &a->classes[sc];
            return slab_slot_addr(s, slot, sca->object_size);
        }
        
    }
    
    return NULL;
}

bool tls_try_free(SlabAllocator* a, uint32_t sc, SlabHandle h) {
    if (!_tls_initialized) {
        tls_init_thread();
    }
    
    TLSCache* tls = &_tls_cache[sc];
    
    if (tls->count < TLS_CACHE_SIZE) {
        uint32_t slab_id, gen, slot, size_class_check;
        handle_unpack(h, &slab_id, &gen, &slot, &size_class_check);
        
        Slab* s = reg_lookup_validate(&a->reg, slab_id, gen);
        if (!s) return false;
        
        tls->handles[tls->count] = h;
        tls->epoch_id[tls->count] = s->epoch_id;
        tls->count++;
        return true;
    }
    
    tls_flush_batch(a, sc, TLS_FLUSH_BATCH);
    
    tls->handles[tls->count] = h;
    tls->epoch_id[tls->count] = 0;
    tls->count++;
    return true;
}

void tls_refill(SlabAllocator* a, uint32_t sc, uint32_t epoch_id) {
    TLSCache* tls = &_tls_cache[sc];
    
    for (uint32_t i = 0; i < TLS_REFILL_BATCH && tls->count < TLS_CACHE_SIZE; i++) {
        SlabHandle h;
        void* ptr = alloc_obj_epoch_internal(a, sc, epoch_id, &h);
        
        if (!ptr) break;
        
        tls->handles[tls->count] = h;
        tls->epoch_id[tls->count] = epoch_id;
        tls->count++;
    }
}

void tls_flush_batch(SlabAllocator* a, uint32_t sc, uint32_t batch_size) {
    TLSCache* tls = &_tls_cache[sc];
    
    uint32_t to_flush = (batch_size < tls->count) ? batch_size : tls->count;
    
    for (uint32_t i = 0; i < to_flush; i++) {
        free_obj_internal(a, tls->handles[--tls->count]);
    }
}

void tls_flush_epoch_all_threads(SlabAllocator* a, uint32_t epoch_id) {
    pthread_mutex_lock(&_thread_registry_lock);
    
    for (uint32_t t = 0; t < _thread_count; t++) {
        TLSCache* caches = _thread_registry[t].caches;
        
        for (uint32_t sc = 0; sc < 8; sc++) {
            TLSCache* tls = &caches[sc];
            
            for (uint32_t i = 0; i < tls->count; ) {
                if (tls->epoch_id[i] == epoch_id) {
                    free_obj_internal(a, tls->handles[i]);
                    
                    tls->handles[i] = tls->handles[--tls->count];
                    tls->epoch_id[i] = tls->epoch_id[tls->count];
                } else {
                    i++;
                }
            }
        }
    }
    
    pthread_mutex_unlock(&_thread_registry_lock);
}

#endif /* ENABLE_TLS_CACHE */
