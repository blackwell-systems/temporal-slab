/* epoch_domain.h */

#ifndef EPOCH_DOMAIN_H
#define EPOCH_DOMAIN_H

#include "slab_alloc.h"
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/**
 * Epoch Domain: Structured Temporal Memory Management
 *
 * Contract (IMPORTANT):
 * - Domains are thread-local scopes: enter/exit/destroy MUST occur on the creating thread.
 * - Nesting is supported via a TLS stack (LIFO).
 * - Domains may share an underlying epoch, but the domain object itself is not cross-thread.
 */

typedef struct epoch_domain {
    SlabAllocator* alloc;      /* Allocator instance */
    EpochId epoch_id;          /* Underlying epoch (ring index 0-15) */
    uint64_t epoch_era;        /* Era captured at create/wrap time (wrap-around safety) */
    uint32_t refcount;         /* Nesting depth (thread-local by contract) */
    bool auto_close;           /* Close epoch on last exit? */

    /* Enforce thread-local contract */
    pthread_t owner_tid;
} epoch_domain_t;

/**
 * Create a new epoch domain wrapping the allocator's current epoch.
 *
 * @param alloc Allocator instance
 * @return New domain, or NULL on failure
 */
epoch_domain_t* epoch_domain_create(SlabAllocator* alloc);

/**
 * Create a domain with explicit epoch ID (advanced usage).
 *
 * @param alloc Allocator instance
 * @param epoch_id Existing epoch to wrap
 * @param auto_close Whether to close epoch on last exit
 * @return New domain, or NULL on failure
 */
epoch_domain_t* epoch_domain_wrap(SlabAllocator* alloc, EpochId epoch_id, bool auto_close);

/**
 * Enter a domain scope (nesting-safe).
 * Pushes the domain onto the TLS domain stack.
 */
void epoch_domain_enter(epoch_domain_t* domain);

/**
 * Exit a domain scope (nesting-safe).
 * Pops the domain from the TLS domain stack (must be LIFO).
 * If auto_close is enabled and refcount reaches 0, may call epoch_close()
 * after validating the epoch era and global epoch domain refcount.
 */
void epoch_domain_exit(epoch_domain_t* domain);

/**
 * Get the current thread-local domain (innermost / top of TLS stack).
 */
epoch_domain_t* epoch_domain_current(void);

/**
 * Get the allocator from the current domain.
 */
SlabAllocator* epoch_domain_allocator(void);

/**
 * Destroy a domain.
 *
 * Precondition: refcount == 0 and domain is not present on TLS stack.
 * If auto_close is enabled, closes epoch only if era still matches.
 */
void epoch_domain_destroy(epoch_domain_t* domain);

/**
 * Force epoch closure (advanced / explicit cleanup).
 *
 * Precondition: refcount == 0 and domain is not present on TLS stack.
 * Closes epoch only if era still matches.
 */
void epoch_domain_force_close(epoch_domain_t* domain);

#endif /* EPOCH_DOMAIN_H */
