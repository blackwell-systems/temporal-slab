#include "epoch_domain.h"
#include "slab_alloc_internal.h"  /* For accessing epoch_era[] */
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

/* Thread-local storage for domain stack */
static __thread epoch_domain_t* tls_current_domain = NULL;

epoch_domain_t* epoch_domain_create(SlabAllocator* alloc) {
    if (!alloc) return NULL;
    
    epoch_domain_t* domain = malloc(sizeof(epoch_domain_t));
    if (!domain) return NULL;
    
    domain->alloc = alloc;
    /* Wrap current epoch (no advance) - caller controls phase boundaries */
    domain->epoch_id = epoch_current(alloc);
    domain->epoch_era = atomic_load_explicit(&alloc->epoch_era[domain->epoch_id], memory_order_acquire);  /* Capture era at creation */
    domain->refcount = 0;
    domain->auto_close = false;  // Default: explicit epoch_close (safer)
    
    return domain;
}

epoch_domain_t* epoch_domain_wrap(SlabAllocator* alloc, EpochId epoch_id, bool auto_close) {
    if (!alloc) return NULL;
    
    epoch_domain_t* domain = malloc(sizeof(epoch_domain_t));
    if (!domain) return NULL;
    
    domain->alloc = alloc;
    domain->epoch_id = epoch_id;
    domain->epoch_era = atomic_load_explicit(&alloc->epoch_era[epoch_id], memory_order_acquire);  /* Capture era at wrap time */
    domain->refcount = 0;
    domain->auto_close = auto_close;
    
    return domain;
}

void epoch_domain_enter(epoch_domain_t* domain) {
    if (!domain) return;
    
    /* On first enter (0->1 transition), notify allocator */
    if (domain->refcount == 0) {
        slab_epoch_inc_refcount(domain->alloc, domain->epoch_id);
    }
    
    domain->refcount++;
    
    /* On first enter, set as current domain */
    if (domain->refcount == 1) {
        /* TODO: For nested domains, maintain a stack instead of single pointer */
        tls_current_domain = domain;
    }
}

void epoch_domain_exit(epoch_domain_t* domain) {
    if (!domain) return;
    
    assert(domain->refcount > 0 && "epoch_domain_exit called without matching enter");
    
    domain->refcount--;
    
    /* On last exit (1->0 transition), notify allocator and perform cleanup */
    if (domain->refcount == 0) {
        /* Decrement global refcount */
        slab_epoch_dec_refcount(domain->alloc, domain->epoch_id);
        
        if (domain->auto_close) {
            /* Validate era before auto-closing (prevent closing wrong epoch after wrap) */
            uint64_t current_era = atomic_load_explicit(&domain->alloc->epoch_era[domain->epoch_id], memory_order_acquire);
            if (current_era == domain->epoch_era) {
                /* Era matches - safe to close */
                uint64_t global_refcount = slab_epoch_get_refcount(domain->alloc, domain->epoch_id);
                if (global_refcount == 0) {
                    epoch_close(domain->alloc, domain->epoch_id);
                }
            }
            /* Era mismatch: epoch wrapped and reused, skip auto-close */
        }
        
        /* Clear thread-local if this was current */
        if (tls_current_domain == domain) {
            tls_current_domain = NULL;
        }
    }
}

epoch_domain_t* epoch_domain_current(void) {
    return tls_current_domain;
}

SlabAllocator* epoch_domain_allocator(void) {
    epoch_domain_t* domain = epoch_domain_current();
    return domain ? domain->alloc : NULL;
}

void epoch_domain_destroy(epoch_domain_t* domain) {
    if (!domain) return;
    
    assert(domain->refcount == 0 && "epoch_domain_destroy called while domain is active");
    
    if (domain->auto_close) {
        /* Validate era before closing (prevent closing wrong epoch after wrap) */
        uint64_t current_era = atomic_load_explicit(&domain->alloc->epoch_era[domain->epoch_id], memory_order_acquire);
        if (current_era == domain->epoch_era) {
            /* Era matches - safe to close */
            epoch_close(domain->alloc, domain->epoch_id);
        }
        /* Era mismatch: epoch wrapped and reused, skip close */
    }
    
    free(domain);
}

void epoch_domain_force_close(epoch_domain_t* domain) {
    if (!domain) return;
    
    /* Safety: Only allow force-close when refcount is already 0 (no active scopes) */
    assert(domain->refcount == 0 && "epoch_domain_force_close called with active scopes - use epoch_domain_exit instead");
    
    /* Validate era before closing (prevent closing wrong epoch after wrap) */
    uint64_t current_era = atomic_load_explicit(&domain->alloc->epoch_era[domain->epoch_id], memory_order_acquire);
    if (current_era == domain->epoch_era) {
        epoch_close(domain->alloc, domain->epoch_id);
    }
    /* Era mismatch: epoch wrapped and reused, skip close */
    
    /* Clear TLS if this was the current domain (though refcount=0 means we already exited) */
    if (tls_current_domain == domain) {
        tls_current_domain = NULL;
    }
}
