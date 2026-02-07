/* epoch_domain.c */

#include "epoch_domain.h"
#include "slab_alloc_internal.h"  /* For accessing epoch_era[] */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

/* ------------------------------ TLS domain stack (nesting-safe) ------------------------------ */

#ifndef EPOCH_DOMAIN_STACK_MAX
#define EPOCH_DOMAIN_STACK_MAX 32
#endif

static __thread epoch_domain_t* tls_domain_stack[EPOCH_DOMAIN_STACK_MAX];
static __thread uint32_t       tls_domain_depth = 0;

static inline epoch_domain_t* tls_top(void) {
    return (tls_domain_depth == 0) ? NULL : tls_domain_stack[tls_domain_depth - 1];
}

static inline void tls_push(epoch_domain_t* d) {
    assert(tls_domain_depth < EPOCH_DOMAIN_STACK_MAX && "epoch domain nesting overflow");
    tls_domain_stack[tls_domain_depth++] = d;
}

static inline void tls_pop_expected(epoch_domain_t* expected) {
    assert(tls_domain_depth > 0 && "epoch_domain_exit with empty TLS stack");
    assert(tls_domain_stack[tls_domain_depth - 1] == expected && "epoch_domain_exit out of order (non-LIFO)");
    tls_domain_depth--;
}

/* Debug: ensure a domain isn't still on this thread's TLS stack */
static inline void tls_assert_not_present(epoch_domain_t* d) {
#ifndef NDEBUG
    for (uint32_t i = 0; i < tls_domain_depth; i++) {
        assert(tls_domain_stack[i] != d && "domain still present on TLS stack");
    }
#else
    (void)d;
#endif
}

/* ------------------------------ API ------------------------------ */

epoch_domain_t* epoch_domain_create(SlabAllocator* alloc) {
    if (!alloc) return NULL;

    epoch_domain_t* domain = (epoch_domain_t*)malloc(sizeof(epoch_domain_t));
    if (!domain) return NULL;

    domain->alloc = alloc;

    /* Wrap current epoch (no advance) - caller controls phase boundaries */
    domain->epoch_id = epoch_current(alloc);
    domain->epoch_era = atomic_load_explicit(&alloc->epoch_era[domain->epoch_id], memory_order_acquire);

    domain->refcount = 0;
    domain->auto_close = false;  /* Default: explicit epoch_close (safer) */

    /* Contract: domain is thread-local scoped. Enforce ownership. */
    domain->owner_tid = pthread_self();

    return domain;
}

epoch_domain_t* epoch_domain_wrap(SlabAllocator* alloc, EpochId epoch_id, bool auto_close) {
    if (!alloc) return NULL;

    epoch_domain_t* domain = (epoch_domain_t*)malloc(sizeof(epoch_domain_t));
    if (!domain) return NULL;

    domain->alloc = alloc;
    domain->epoch_id = epoch_id;
    domain->epoch_era = atomic_load_explicit(&alloc->epoch_era[epoch_id], memory_order_acquire);

    domain->refcount = 0;
    domain->auto_close = auto_close;

    /* Contract: domain is thread-local scoped. Enforce ownership. */
    domain->owner_tid = pthread_self();

    return domain;
}

void epoch_domain_enter(epoch_domain_t* domain) {
    if (!domain) return;

    /* Enforce thread-local contract */
    assert(pthread_equal(domain->owner_tid, pthread_self()) && "epoch_domain_enter: domain used from non-owner thread");

    /* On first enter (0->1 transition), notify allocator */
    if (domain->refcount == 0) {
        slab_epoch_inc_refcount(domain->alloc, domain->epoch_id);
    }

    domain->refcount++;

    /* Always push for nesting correctness (even re-entrance) */
    tls_push(domain);
}

void epoch_domain_exit(epoch_domain_t* domain) {
    if (!domain) return;

    /* Enforce thread-local contract */
    assert(pthread_equal(domain->owner_tid, pthread_self()) && "epoch_domain_exit: domain used from non-owner thread");

    assert(domain->refcount > 0 && "epoch_domain_exit called without matching enter");

    /* Must unwind in LIFO order */
    tls_pop_expected(domain);

    domain->refcount--;

    /* On last exit (1->0 transition), notify allocator and perform cleanup */
    if (domain->refcount == 0) {
        slab_epoch_dec_refcount(domain->alloc, domain->epoch_id);

        if (domain->auto_close) {
            /* Validate era before auto-closing (prevent closing wrong epoch after wrap) */
            uint64_t current_era =
                atomic_load_explicit(&domain->alloc->epoch_era[domain->epoch_id], memory_order_acquire);

            if (current_era == domain->epoch_era) {
                /* Era matches - safe to close if no other active domains for this epoch */
                uint64_t global_refcount = slab_epoch_get_refcount(domain->alloc, domain->epoch_id);
                if (global_refcount == 0) {
                    epoch_close(domain->alloc, domain->epoch_id);
                }
            }
            /* Era mismatch: epoch wrapped and reused, skip auto-close */
        }
    }
}

epoch_domain_t* epoch_domain_current(void) {
    return tls_top();
}

SlabAllocator* epoch_domain_allocator(void) {
    epoch_domain_t* domain = epoch_domain_current();
    return domain ? domain->alloc : NULL;
}

void epoch_domain_destroy(epoch_domain_t* domain) {
    if (!domain) return;

    /* Enforce thread-local contract (destroy on owner thread) */
    assert(pthread_equal(domain->owner_tid, pthread_self()) && "epoch_domain_destroy: domain destroyed from non-owner thread");

    assert(domain->refcount == 0 && "epoch_domain_destroy called while domain is active");
    tls_assert_not_present(domain);

    if (domain->auto_close) {
        /* Validate era before closing (prevent closing wrong epoch after wrap) */
        uint64_t current_era =
            atomic_load_explicit(&domain->alloc->epoch_era[domain->epoch_id], memory_order_acquire);

        if (current_era == domain->epoch_era) {
            epoch_close(domain->alloc, domain->epoch_id);
        }
        /* Era mismatch: epoch wrapped and reused, skip close */
    }

    free(domain);
}

void epoch_domain_force_close(epoch_domain_t* domain) {
    if (!domain) return;

    /* Enforce thread-local contract */
    assert(pthread_equal(domain->owner_tid, pthread_self()) && "epoch_domain_force_close: domain used from non-owner thread");

    /* Safety: Only allow force-close when refcount is already 0 (no active scopes) */
    assert(domain->refcount == 0 && "epoch_domain_force_close called with active scopes - use epoch_domain_exit instead");
    tls_assert_not_present(domain);

    /* Validate era before closing (prevent closing wrong epoch after wrap) */
    uint64_t current_era =
        atomic_load_explicit(&domain->alloc->epoch_era[domain->epoch_id], memory_order_acquire);

    if (current_era == domain->epoch_era) {
        epoch_close(domain->alloc, domain->epoch_id);
    }
    /* Era mismatch: epoch wrapped and reused, skip close */
}
