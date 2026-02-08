#include "slab_stats.h"
#include "slab_alloc_internal.h"
#include <string.h>

void slab_stats_global(SlabAllocator* alloc, SlabGlobalStats* out) {
  memset(out, 0, sizeof(*out));
  
  out->version = SLAB_STATS_VERSION;
  
  /* Read global epoch state */
  out->current_epoch = atomic_load_explicit(&alloc->current_epoch, memory_order_relaxed);
  
  /* Count active vs closing epochs */
  for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
    EpochLifecycleState state = atomic_load_explicit(&alloc->epoch_state[e], memory_order_relaxed);
    if (state == EPOCH_ACTIVE) {
      out->active_epoch_count++;
    } else {
      out->closing_epoch_count++;
    }
  }
  
  /* Aggregate counters across all size classes */
  for (uint32_t cls = 0; cls < 8; cls++) {
    SizeClassAlloc* sc = &alloc->classes[cls];
    
    /* Slab lifecycle totals */
    uint64_t allocated = atomic_load_explicit(&sc->new_slab_count, memory_order_relaxed);
    uint64_t recycled = atomic_load_explicit(&sc->empty_slab_recycled, memory_order_relaxed);
    out->total_slabs_allocated += allocated;
    out->total_slabs_recycled += recycled;
    
    /* Performance totals */
    out->total_slow_path_hits += atomic_load_explicit(&sc->slow_path_hits, memory_order_relaxed);
    out->total_cache_overflows += atomic_load_explicit(&sc->empty_slab_overflowed, memory_order_relaxed);
    
    /* Phase 2.0: Slow-path attribution */
    out->total_slow_cache_miss += atomic_load_explicit(&sc->slow_path_cache_miss, memory_order_relaxed);
    out->total_slow_epoch_closed += atomic_load_explicit(&sc->slow_path_epoch_closed, memory_order_relaxed);
    
    /* Phase 2.0: RSS reclamation tracking */
    out->total_madvise_calls += atomic_load_explicit(&sc->madvise_calls, memory_order_relaxed);
    out->total_madvise_bytes += atomic_load_explicit(&sc->madvise_bytes, memory_order_relaxed);
    out->total_madvise_failures += atomic_load_explicit(&sc->madvise_failures, memory_order_relaxed);
    
    /* Phase 2.2: Lock-free contention totals */
    out->total_bitmap_alloc_cas_retries += atomic_load_explicit(&sc->bitmap_alloc_cas_retries, memory_order_relaxed);
    out->total_bitmap_free_cas_retries += atomic_load_explicit(&sc->bitmap_free_cas_retries, memory_order_relaxed);
    out->total_current_partial_cas_failures += atomic_load_explicit(&sc->current_partial_cas_failures, memory_order_relaxed);
    out->total_bitmap_alloc_attempts += atomic_load_explicit(&sc->bitmap_alloc_attempts, memory_order_relaxed);
    out->total_bitmap_free_attempts += atomic_load_explicit(&sc->bitmap_free_attempts, memory_order_relaxed);
    out->total_current_partial_cas_attempts += atomic_load_explicit(&sc->current_partial_cas_attempts, memory_order_relaxed);
  }
  
  /* Derived metrics (handle underflow gracefully) */
  if (out->total_slabs_recycled > out->total_slabs_allocated) {
    out->net_slabs = 0;  /* Underflow: cache recycling exceeded new allocations */
  } else {
    out->net_slabs = out->total_slabs_allocated - out->total_slabs_recycled;
  }
  out->estimated_slab_rss_bytes = out->net_slabs * SLAB_PAGE_SIZE;
  
  /* Actual RSS from OS */
  out->rss_bytes_current = read_rss_bytes_linux();
}

void slab_stats_class(SlabAllocator* alloc, uint32_t size_class, SlabClassStats* out) {
  if (size_class >= 8) {
    memset(out, 0, sizeof(*out));
    return;
  }
  
  SizeClassAlloc* sc = &alloc->classes[size_class];
  
  out->version = SLAB_STATS_VERSION;
  out->class_index = size_class;
  out->object_size = sc->object_size;
  
  /* Read atomic counters (relaxed ordering, non-atomic snapshot) */
  out->slow_path_hits = atomic_load_explicit(&sc->slow_path_hits, memory_order_relaxed);
  out->new_slab_count = atomic_load_explicit(&sc->new_slab_count, memory_order_relaxed);
  out->list_move_partial_to_full = atomic_load_explicit(&sc->list_move_partial_to_full, memory_order_relaxed);
  out->list_move_full_to_partial = atomic_load_explicit(&sc->list_move_full_to_partial, memory_order_relaxed);
  out->current_partial_null = atomic_load_explicit(&sc->current_partial_null, memory_order_relaxed);
  out->current_partial_full = atomic_load_explicit(&sc->current_partial_full, memory_order_relaxed);
  out->empty_slab_recycled = atomic_load_explicit(&sc->empty_slab_recycled, memory_order_relaxed);
  out->empty_slab_overflowed = atomic_load_explicit(&sc->empty_slab_overflowed, memory_order_relaxed);
  
  /* Phase 2.0: Slow-path attribution counters */
  out->slow_path_cache_miss = atomic_load_explicit(&sc->slow_path_cache_miss, memory_order_relaxed);
  out->slow_path_epoch_closed = atomic_load_explicit(&sc->slow_path_epoch_closed, memory_order_relaxed);
  
  /* Phase 2.0: RSS reclamation tracking */
  out->madvise_calls = atomic_load_explicit(&sc->madvise_calls, memory_order_relaxed);
  out->madvise_bytes = atomic_load_explicit(&sc->madvise_bytes, memory_order_relaxed);
  out->madvise_failures = atomic_load_explicit(&sc->madvise_failures, memory_order_relaxed);
  
  /* Phase 2.1: Epoch-close telemetry */
  out->epoch_close_calls = atomic_load_explicit(&sc->epoch_close_calls, memory_order_relaxed);
  out->epoch_close_scanned_slabs = atomic_load_explicit(&sc->epoch_close_scanned_slabs, memory_order_relaxed);
  out->epoch_close_recycled_slabs = atomic_load_explicit(&sc->epoch_close_recycled_slabs, memory_order_relaxed);
  out->epoch_close_total_ns = atomic_load_explicit(&sc->epoch_close_total_ns, memory_order_relaxed);
  
  /* Phase 2.2: Lock-free contention metrics */
  out->bitmap_alloc_cas_retries = atomic_load_explicit(&sc->bitmap_alloc_cas_retries, memory_order_relaxed);
  out->bitmap_free_cas_retries = atomic_load_explicit(&sc->bitmap_free_cas_retries, memory_order_relaxed);
  out->current_partial_cas_failures = atomic_load_explicit(&sc->current_partial_cas_failures, memory_order_relaxed);
  out->bitmap_alloc_attempts = atomic_load_explicit(&sc->bitmap_alloc_attempts, memory_order_relaxed);
  out->bitmap_free_attempts = atomic_load_explicit(&sc->bitmap_free_attempts, memory_order_relaxed);
  out->current_partial_cas_attempts = atomic_load_explicit(&sc->current_partial_cas_attempts, memory_order_relaxed);
  
  /* Phase 2.2: Lock contention (Tier 0 trylock probe) */
  out->lock_fast_acquire = atomic_load_explicit(&sc->lock_fast_acquire, memory_order_relaxed);
  out->lock_contended = atomic_load_explicit(&sc->lock_contended, memory_order_relaxed);
  
  /* Phase 2.2+: Adaptive bitmap scanning observability */
  out->scan_adapt_checks = sc->scan_adapt.checks;
  out->scan_adapt_switches = sc->scan_adapt.switches;
  out->scan_mode = sc->scan_adapt.mode;
  
  /* Phase 2.2: Compute derived contention metrics (avoid divide by zero) */
  if (out->bitmap_alloc_attempts > 0) {
    out->avg_alloc_cas_retries_per_attempt = (double)out->bitmap_alloc_cas_retries / out->bitmap_alloc_attempts;
  } else {
    out->avg_alloc_cas_retries_per_attempt = 0.0;
  }
  
  if (out->bitmap_free_attempts > 0) {
    out->avg_free_cas_retries_per_attempt = (double)out->bitmap_free_cas_retries / out->bitmap_free_attempts;
  } else {
    out->avg_free_cas_retries_per_attempt = 0.0;
  }
  
  if (out->current_partial_cas_attempts > 0) {
    out->current_partial_cas_failure_rate = (double)out->current_partial_cas_failures / out->current_partial_cas_attempts;
  } else {
    out->current_partial_cas_failure_rate = 0.0;
  }
  
  uint64_t total_lock_ops = out->lock_fast_acquire + out->lock_contended;
  if (total_lock_ops > 0) {
    out->lock_contention_rate = (double)out->lock_contended / total_lock_ops;
  } else {
    out->lock_contention_rate = 0.0;
  }
  
#ifdef ENABLE_LABEL_CONTENTION
  /* Phase 2.3: Per-label contention attribution */
  for (uint8_t lid = 0; lid < 16; lid++) {
    out->lock_fast_acquire_by_label[lid] = atomic_load_explicit(&sc->lock_fast_acquire_by_label[lid], memory_order_relaxed);
    out->lock_contended_by_label[lid] = atomic_load_explicit(&sc->lock_contended_by_label[lid], memory_order_relaxed);
    out->bitmap_alloc_cas_retries_by_label[lid] = atomic_load_explicit(&sc->bitmap_alloc_cas_retries_by_label[lid], memory_order_relaxed);
    out->bitmap_free_cas_retries_by_label[lid] = atomic_load_explicit(&sc->bitmap_free_cas_retries_by_label[lid], memory_order_relaxed);
  }
#endif
  
  /* Cache state snapshot (brief lock) */
  pthread_mutex_lock(&sc->cache_lock);
  out->cache_size = (uint32_t)sc->cache_size;
  out->cache_capacity = (uint32_t)sc->cache_capacity;
  out->cache_overflow_len = (uint32_t)sc->cache_overflow_len;
  pthread_mutex_unlock(&sc->cache_lock);
  
  /* Aggregate slab counts across all epochs (brief lock) */
  out->total_partial_slabs = 0;
  out->total_full_slabs = 0;
  
  pthread_mutex_lock(&sc->lock);
  for (uint32_t e = 0; e < EPOCH_COUNT; e++) {
    out->total_partial_slabs += (uint32_t)sc->epochs[e].partial.len;
    out->total_full_slabs += (uint32_t)sc->epochs[e].full.len;
  }
  pthread_mutex_unlock(&sc->lock);
  
  /* Derived metrics */
  uint64_t total_recycled_or_overflowed = out->empty_slab_recycled + out->empty_slab_overflowed;
  if (total_recycled_or_overflowed > 0) {
    out->recycle_rate_pct = 100.0 * out->empty_slab_recycled / total_recycled_or_overflowed;
  } else {
    out->recycle_rate_pct = 0.0;
  }
  
  /* Handle underflow gracefully (steady-state recycling can exceed new allocations) */
  if (out->empty_slab_recycled > out->new_slab_count) {
    out->net_slabs = 0;
  } else {
    out->net_slabs = out->new_slab_count - out->empty_slab_recycled;
  }
  out->estimated_rss_bytes = out->net_slabs * SLAB_PAGE_SIZE;
}

void slab_stats_epoch(SlabAllocator* alloc, uint32_t size_class, EpochId epoch, SlabEpochStats* out) {
  if (size_class >= 8 || epoch >= EPOCH_COUNT) {
    memset(out, 0, sizeof(*out));
    return;
  }
  
  SizeClassAlloc* sc = &alloc->classes[size_class];
  
  out->version = SLAB_STATS_VERSION;
  out->class_index = size_class;
  out->object_size = sc->object_size;
  out->epoch_id = epoch;
  
  /* Phase 2.2: Era stamping for monotonic observability */
  out->epoch_era = atomic_load_explicit(&alloc->epoch_era[epoch], memory_order_acquire);
  
  /* Read epoch lifecycle state */
  out->state = atomic_load_explicit(&alloc->epoch_state[epoch], memory_order_relaxed);
  
  /* Phase 2.3: Read epoch metadata */
  out->open_since_ns = alloc->epoch_meta[epoch].open_since_ns;
  out->alloc_count = atomic_load_explicit(&alloc->epoch_meta[epoch].domain_refcount, memory_order_relaxed);
  memcpy(out->label, alloc->epoch_meta[epoch].label, sizeof(out->label));
  
  /* Phase 2.4: Read RSS delta tracking */
  out->rss_before_close = alloc->epoch_meta[epoch].rss_before_close;
  out->rss_after_close = alloc->epoch_meta[epoch].rss_after_close;
  
  /* Read list lengths (brief lock) */
  pthread_mutex_lock(&sc->lock);
  
  EpochState* es = &sc->epochs[epoch];
  out->partial_slab_count = (uint32_t)es->partial.len;
  out->full_slab_count = (uint32_t)es->full.len;
  
  pthread_mutex_unlock(&sc->lock);
  
  /* Phase 2.1: O(1) reclaimable count (no scan needed) */
  out->reclaimable_slab_count = atomic_load_explicit(&es->empty_partial_count, memory_order_relaxed);
  
  /* Derived metrics */
  out->estimated_rss_bytes = (uint64_t)(out->partial_slab_count + out->full_slab_count) * SLAB_PAGE_SIZE;
  out->reclaimable_bytes = (uint64_t)out->reclaimable_slab_count * SLAB_PAGE_SIZE;
}
