#include "epoch_domain.h"
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
    epoch_advance(alloc);
    domain->epoch_id = epoch_current(alloc);
    domain->refcount = 0;
    domain->auto_close = true;  // Default: auto-close on last exit
    
    return domain;
}

epoch_domain_t* epoch_domain_wrap(SlabAllocator* alloc, EpochId epoch_id, bool auto_close) {
    if (!alloc) return NULL;
    
    epoch_domain_t* domain = malloc(sizeof(epoch_domain_t));
    if (!domain) return NULL;
    
    domain->alloc = alloc;
    domain->epoch_id = epoch_id;
    domain->refcount = 0;
    domain->auto_close = auto_close;
    
    return domain;
}

void epoch_domain_enter(epoch_domain_t* domain) {
    if (!domain) return;
    
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
    
    /* On last exit, perform cleanup */
    if (domain->refcount == 0) {
        if (domain->auto_close) {
            epoch_close(domain->alloc, domain->epoch_id);
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
        epoch_close(domain->alloc, domain->epoch_id);
    }
    
    free(domain);
}

void epoch_domain_force_close(epoch_domain_t* domain) {
    if (!domain) return;
    
    epoch_close(domain->alloc, domain->epoch_id);
    domain->refcount = 0;
    
    if (tls_current_domain == domain) {
        tls_current_domain = NULL;
    }
}
