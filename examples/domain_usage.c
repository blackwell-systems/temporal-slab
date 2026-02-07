/**
 * Epoch Domain Usage Examples
 * 
 * Demonstrates structured temporal memory management patterns.
 */

#include "slab_alloc.h"
#include "epoch_domain.h"
#include <stdio.h>
#include <string.h>

/* Example 1: Request-scoped allocation (web server pattern) */
void example_request_scope(SlabAllocator* alloc) {
    printf("\n=== Example 1: Request-Scoped Allocation ===\n");
    
    /* Create domain for request lifetime */
    epoch_domain_t* request = epoch_domain_create(alloc);
    
    epoch_domain_enter(request);
    {
        /* All allocations in this scope tied to request lifetime */
        char* session = slab_malloc_epoch(alloc, 128, request->epoch_id);
        char* cache_entry = slab_malloc_epoch(alloc, 256, request->epoch_id);
        char* response_buffer = slab_malloc_epoch(alloc, 504, request->epoch_id);
        
        strcpy(session, "user_session_abc123");
        printf("  Session: %s\n", session);
        printf("  Allocated: session, cache, response buffer\n");
        
        /* No individual frees needed */
    }
    epoch_domain_exit(request);  /* Automatic epoch_close() */
    
    printf("  Domain exited - all request memory reclaimed\n");
    
    epoch_domain_destroy(request);
}

/* Example 2: Reusable frame domain (game engine pattern) */
void example_reusable_frame(SlabAllocator* alloc) {
    printf("\n=== Example 2: Reusable Frame Domain ===\n");
    
    /* Create once, reuse for every frame (manual close) */
    epoch_advance(alloc);
    EpochId frame_epoch = epoch_current(alloc);
    epoch_domain_t* frame = epoch_domain_wrap(alloc, frame_epoch, false);  /* Manual control */
    
    for (int i = 0; i < 3; i++) {
        
        epoch_domain_enter(frame);
        {
            /* Frame-local allocations */
            char* render_data = slab_malloc_epoch(alloc, 384, frame->epoch_id);
            char* debug_info = slab_malloc_epoch(alloc, 128, frame->epoch_id);
            
            if (render_data && debug_info) {
                snprintf(debug_info, 128, "Frame %d rendered", i);
                printf("  %s\n", debug_info);
            } else {
                printf("  ERROR: Allocation failed\n");
            }
            
            /* No per-frame cleanup needed */
        }
        epoch_domain_exit(frame);  /* Deterministic reclamation */
        
        printf("  Frame %d memory reclaimed\n", i);
    }
    
    epoch_domain_destroy(frame);
    printf("  Frame domain destroyed\n");
}

/* Example 3: Nested domains (transaction + query scope) */
void execute_query(epoch_domain_t* query_domain) {
    epoch_domain_enter(query_domain);
    {
        char* result_set = slab_malloc_epoch(query_domain->alloc, 256, query_domain->epoch_id);
        char* index_buffer = slab_malloc_epoch(query_domain->alloc, 256, query_domain->epoch_id);
        
        strcpy(result_set, "SELECT * FROM users...");
        snprintf(index_buffer, 512, "index_data");
        printf("    Query executed: %s\n", result_set);
    }
    epoch_domain_exit(query_domain);
    printf("    Query memory reclaimed\n");
}

void example_nested_domains(SlabAllocator* alloc) {
    printf("\n=== Example 3: Nested Transaction + Query Domains ===\n");
    
    epoch_domain_t* transaction = epoch_domain_create(alloc);
    
    epoch_domain_enter(transaction);
    {
        char* txn_log = slab_malloc_epoch(alloc, 512, transaction->epoch_id);
        if (txn_log) {
            strcpy(txn_log, "Transaction BEGIN");
            printf("  Transaction started\n");
        } else {
            printf("  ERROR: Transaction allocation failed\n");
        }
        
        /* Nested query scope */
        epoch_domain_t* query = epoch_domain_create(alloc);
        execute_query(query);
        epoch_domain_destroy(query);
        
        /* Transaction continues with its own allocations */
        char* commit_data = slab_malloc_epoch(alloc, 256, transaction->epoch_id);
        if (commit_data) {
            strcpy(commit_data, "COMMIT");
            printf("  Transaction committed\n");
        }
    }
    epoch_domain_exit(transaction);
    printf("  Transaction memory reclaimed\n");
    
    epoch_domain_destroy(transaction);
}

/* Example 4: Explicit lifetime control */
void example_explicit_control(SlabAllocator* alloc) {
    printf("\n=== Example 4: Explicit Lifetime Control ===\n");
    
    /* Create domain without auto-close */
    epoch_advance(alloc);
    EpochId epoch = epoch_current(alloc);
    epoch_domain_t* domain = epoch_domain_wrap(alloc, epoch, false);
    
    /* Multiple enter/exit cycles without cleanup */
    for (int i = 0; i < 3; i++) {
        epoch_domain_enter(domain);
        {
            char* buffer = slab_malloc_epoch(alloc, 128, epoch_current(alloc));
            snprintf(buffer, 128, "Batch %d", i);
            printf("  Allocated: %s\n", buffer);
        }
        epoch_domain_exit(domain);
        printf("  Exited domain (memory still allocated)\n");
    }
    
    /* Explicit cleanup */
    epoch_domain_force_close(domain);
    printf("  Force closed - all batches reclaimed\n");
    
    epoch_domain_destroy(domain);
}

int main(void) {
    SlabAllocator* alloc = slab_allocator_create();
    
    if (!alloc) {
        fprintf(stderr, "ERROR: Failed to create allocator\n");
        return 1;
    }
    
    printf("Allocator created successfully at %p\n", (void*)alloc);
    
    example_request_scope(alloc);
    printf("Example 1 completed\n");
    
    example_reusable_frame(alloc);
    printf("Example 2 completed\n");
    
    example_nested_domains(alloc);
    printf("Example 3 completed\n");
    
    example_explicit_control(alloc);
    printf("Example 4 completed\n");
    
    slab_allocator_free(alloc);
    
    printf("\n=== All examples completed successfully ===\n");
    return 0;
}
