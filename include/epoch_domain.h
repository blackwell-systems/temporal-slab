#ifndef EPOCH_DOMAIN_H
#define EPOCH_DOMAIN_H

#include "slab_alloc.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Epoch Domain: Structured Temporal Memory Management
 * 
 * Provides scoped, composable memory lifetime management on top of epochs.
 * Domains formalize the relationship between allocation scope and epoch lifecycle,
 * enabling RAII-style automatic cleanup and nested temporal scopes.
 * 
 * Key properties:
 * - Automatic epoch_close() on domain exit
 * - Nestable scopes with refcount tracking
 * - Thread-local context for implicit allocation binding
 * - Zero overhead over raw epoch usage
 * 
 * Example usage:
 * 
 *   // Request-scoped memory
 *   epoch_domain_t* request = epoch_domain_create(alloc);
 *   epoch_domain_enter(request);
 *       handle_request(conn);
 *   epoch_domain_exit(request);  // Automatic reclamation
 * 
 *   // Reusable frame domain
 *   epoch_domain_t* frame = epoch_domain_create(alloc);
 *   for (int i = 0; i < frames; i++) {
 *       epoch_domain_enter(frame);
 *           render_frame();
 *       epoch_domain_exit(frame);  // Deterministic cleanup
 *   }
 */

typedef struct epoch_domain {
    SlabAllocator* alloc;      // Allocator instance
    EpochId epoch_id;          // Underlying epoch
    uint32_t refcount;         // Nesting depth
    bool auto_close;           // Close epoch on last exit?
} epoch_domain_t;

/**
 * Create a new epoch domain.
 * Allocates a new epoch from the allocator and wraps it in a domain.
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
 * Enter a domain scope.
 * Increments refcount and sets thread-local context.
 * Can be called multiple times for nested scopes.
 * 
 * @param domain Domain to enter
 */
void epoch_domain_enter(epoch_domain_t* domain);

/**
 * Exit a domain scope.
 * Decrements refcount. If auto_close is enabled and refcount reaches 0,
 * automatically calls epoch_close() to reclaim memory.
 * 
 * @param domain Domain to exit
 */
void epoch_domain_exit(epoch_domain_t* domain);

/**
 * Get the current thread-local domain.
 * Returns the innermost active domain, or NULL if none.
 * 
 * @return Current domain, or NULL
 */
epoch_domain_t* epoch_domain_current(void);

/**
 * Get the allocator from the current domain.
 * Convenience function for implicit allocation binding.
 * 
 * @return Allocator from current domain, or NULL
 */
SlabAllocator* epoch_domain_allocator(void);

/**
 * Destroy a domain.
 * If auto_close is enabled, calls epoch_close(). 
 * Frees domain structure.
 * 
 * WARNING: Domain must not be active (refcount must be 0).
 * 
 * @param domain Domain to destroy
 */
void epoch_domain_destroy(epoch_domain_t* domain);

/**
 * Force epoch closure regardless of refcount.
 * Use with caution - for explicit cleanup scenarios.
 * 
 * @param domain Domain to close
 */
void epoch_domain_force_close(epoch_domain_t* domain);

#endif /* EPOCH_DOMAIN_H */
