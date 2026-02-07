#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define SLAB_PAGE_SIZE 4096u

/* Forward declarations - internal structures (opaque to users) */
typedef struct Slab Slab;
typedef struct SlabList SlabList;
typedef struct SizeClassAlloc SizeClassAlloc;
typedef struct SlabAllocator SlabAllocator;

/* Epoch ID type for temporal memory management */
typedef uint32_t EpochId;

/* Handle returned to caller (opaque 64-bit encoded value)
 * 
 * Format version 1 (v1) encoding:
 *   [63:42] slab_id (22 bits) - max 4M slabs
 *   [41:18] generation (24 bits) - wraps after 16M reuses per slab
 *   [17:10] slot (8 bits) - max 255 objects per slab
 *   [9:2]   size_class (8 bits) - max 255 size classes
 *   [1:0]   version (2 bits) - format version (v1=0b01)
 * 
 * Key properties:
 * - Portable: No raw pointers (works on all platforms)
 * - ABA-safe: 24-bit generation prevents stale handle reuse
 * - Versioned: 2-bit version field allows future format changes
 * 
 * Design constraints:
 * - 8-bit slot limits max objects per slab to 255
 * - This bounds min object size to ~16 bytes (4096 / 255)
 * - Current size classes (64-768 bytes) well within limits
 */
typedef uint64_t SlabHandle;


/* Performance counters */
typedef struct PerfCounters {
  uint64_t slow_path_hits;
  uint64_t new_slab_count;
  uint64_t list_move_partial_to_full;
  uint64_t list_move_full_to_partial;
  uint64_t current_partial_miss;        /* For backward compatibility */
  uint64_t current_partial_null;        /* Fast path saw NULL current_partial */
  uint64_t current_partial_full;        /* Fast path saw full current_partial */
  
  /* Phase 2: RSS reclamation metrics */
  uint64_t empty_slab_recycled;        /* Slabs pushed to cache */
  uint64_t empty_slab_overflowed;      /* Slabs pushed to overflow (cache full) */
} PerfCounters;


/* Public API */
void allocator_init(SlabAllocator* a);
void allocator_destroy(SlabAllocator* a);

/* Basic allocation (non-epoch aware) */
void* alloc_obj(SlabAllocator* a, uint32_t size, SlabHandle* out);
bool free_obj(SlabAllocator* a, SlabHandle h);

/* Epoch-aware allocation for temporal memory management */
void* alloc_obj_epoch(SlabAllocator* a, uint32_t size, EpochId epoch, SlabHandle* out);

/* Epoch management APIs
 * 
 * EPOCH SEMANTICS:
 * 
 * Epochs provide temporal grouping of allocations for controlled RSS reclamation.
 * The allocator maintains a ring buffer of 16 epochs (indices 0-15).
 * 
 * - ACTIVE epoch: Accepts new allocations
 * - CLOSING epoch: No new allocations, draining only, empty slabs are recycled
 * 
 * epoch_advance():
 *   Marks current epoch as CLOSING, advances to next epoch (mod 16)
 *   Use when rotating through lifecycle phases (frames, requests, batches)
 * 
 * epoch_close(epoch):
 *   Marks specific epoch as CLOSING without rotation
 *   Proactively scans for empty slabs and recycles them
 *   Use for explicit reclamation boundaries
 * 
 * EPOCH WRAP BEHAVIOR:
 * 
 * Epochs are ring buffer indices, not unique lifetime identifiers.
 * When epoch 15 → 0, epoch 0 transitions CLOSING→ACTIVE.
 * 
 * Old "epoch 0" slabs (still draining) coexist with new "epoch 0" allocations:
 * - Old slabs: Removed from epoch lists, in cache or draining
 * - New slabs: Fresh allocations into newly-ACTIVE epoch 0
 * 
 * This is SAFE because:
 * - Slab list membership determines behavior, not epoch_id value alone
 * - Epoch state transitions are atomic and allocation-gated
 * - Wrap-around after 16 epoch_advance() calls means old slabs are pathologically long-lived
 * 
 * If true "epoch as unique phase" semantics are needed, consider adding epoch era
 * tracking (stamp slabs with generation+era tuple). Current design prioritizes simplicity.
 */
EpochId epoch_current(SlabAllocator* a);
void epoch_advance(SlabAllocator* a);
void epoch_close(SlabAllocator* a, EpochId epoch);

/* Convenience wrappers */
SlabAllocator* slab_allocator_create(void);
void slab_allocator_free(SlabAllocator* a);

/* Utility functions */
uint32_t slab_object_count(uint32_t obj_size);
uint64_t read_rss_bytes_linux(void);
uint64_t now_ns(void);

/* Performance counter access */
void get_perf_counters(SlabAllocator* a, uint32_t size_class, PerfCounters* out);

#endif /* SLAB_ALLOC_H */
