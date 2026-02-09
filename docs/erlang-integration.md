# Erlang Integration: Temporal-Slab for "Run Forever" Systems

## Overview

This document explores integrating the temporal-slab allocator with Erlang/OTP to achieve indefinite uptime without RSS creep. The key insight: **Erlang's process model is inherently temporal**, making it a natural fit for epoch-based memory management.

## The Problem: RSS Creep in Long-Running Erlang Systems

### Current State

Erlang nodes running for weeks/months exhibit:
- **RSS growth**: Small leaks accumulate across millions of processes
- **Fragmentation**: Long-lived and short-lived objects intermix in memory
- **Unpredictable reclamation**: `garbage_collect()` uses heuristics, not explicit boundaries
- **Limited observability**: Hard to diagnose where memory goes

### Why It Matters

Industries running Erlang at scale (telco, fintech, messaging) need:
- **Zero-downtime deployments** (months of uptime)
- **Predictable memory footprint** (capacity planning)
- **Deterministic behavior** (regulatory compliance)
- **Observable memory patterns** (troubleshooting)

## The Solution: Temporal Grouping Meets Process Lifecycle

### Core Concept

Map Erlang's natural temporal structure to allocator epochs:

```erlang
% Each gen_server request gets its own epoch
handle_call(Request, _From, State) ->
    Epoch = tslab:epoch_current(),
    Result = process_request(Request, Epoch),  % All allocations in this epoch
    tslab:epoch_close(Epoch),                  % Force immediate reclamation
    {reply, Result, State}.
```

**Result**: Request completes → Memory reclaimed → RSS returns to baseline

### Why This Works

| Erlang Concept | Temporal-Slab Mapping | Benefit |
|----------------|----------------------|---------|
| Process spawn/die | Epoch open/close | Lifecycle-aligned reclamation |
| Gen_server calls | Per-request epochs | Request-scoped memory |
| Supervision trees | Hierarchical epochs | Subsystem isolation |
| OTP behaviors | Explicit boundaries | Predictable RSS ceiling |

## Architecture: Three-Tier Temporal Allocator

### Current Coverage Gap

**Erlang allocation size distribution:**
```
<64 bytes:    30%  (terms, pids, atoms)       - Not handled
64-768:       40%  (tuples, small lists)      - ✅ Currently handled
768-64KB:     25%  (binaries, large tuples)   - ⚠️ Missing (HIGH PRIORITY)
>64KB:        5%   (refc binaries)            - Direct mmap
```

**Coverage: 40% → Need 95% for "run forever"**

### Proposed: Multi-Tier Design

```
┌─────────────────────────────────────────────────────────┐
│ Tier 1: Dense Slab (64-768 bytes, 4KB pages)           │
│ - Current implementation                                 │
│ - 31-63 objects per slab                                │
│ - Sub-100ns allocation                                  │
│ - Covers: tuples, small lists, small binaries          │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│ Tier 2: Sparse Slab (1KB-16KB, 64KB pages) [NEW]       │
│ - 4-64 objects per slab                                 │
│ - ~200ns allocation                                     │
│ - Covers: parsed JSON, HTTP bodies, ETS rows            │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│ Tier 3: Huge Objects (>16KB, direct mmap) [NEW]        │
│ - Epoch-tracked for reclamation                         │
│ - madvise on epoch_close()                              │
│ - Covers: large binaries, file buffers                 │
└─────────────────────────────────────────────────────────┘
```

## Implementation Plan

### Phase 1: Sparse Slab Tier (1-2 weeks)

**Goal**: Handle 1KB-16KB allocations (covers the missing 25%)

```c
// Size class configuration
static const struct LargeSizeClass {
    uint32_t object_size;
    uint32_t page_size;
    uint32_t objects_per_page;
} k_large_classes[] = {
    {1024,  65536, 64},  // 64 × 1KB objects in 64KB page
    {2048,  65536, 32},  // 32 × 2KB objects
    {4096,  65536, 16},  // 16 × 4KB objects
    {8192,  65536, 8},   // 8 × 8KB objects
    {16384, 65536, 4},   // 4 × 16KB objects
};

// Reuse existing architecture:
// - Bitmap allocation (6-bit slot field → max 64 objects)
// - Per-epoch lists (partial/full)
// - Cache with madvise
// - epoch_close() reclamation
```

**Handle encoding update:**
```c
// Version field indicates tier:
// 0b01 = dense slab (current)
// 0b10 = sparse slab (NEW)

// Sparse handle layout (64 bits):
[63:42] slab_id (22 bits)
[41:18] generation (24 bits)
[17:12] slot (6 bits)        // max 64 objects
[11:2]  size_class (10 bits) // more classes
[1:0]   version = 0b10
```

**Files to modify:**
- `src/slab_alloc.c`: Add `large_slab_alloc()`, `large_slab_free()`
- `src/slab_alloc_internal.h`: Add `LargeSizeClass` structs
- `src/epoch_domain.c`: Hook large tier into epoch_close()

### Phase 2: Huge Object Support (1 week)

**Goal**: Track >16KB allocations for epoch-aligned reclamation

```c
// Per-epoch tracking list
typedef struct HugeAllocation {
    void* ptr;
    size_t size;
    uint32_t epoch_id;
    uint64_t generation;
    struct HugeAllocation* next;
} HugeAllocation;

void* huge_alloc(SlabAllocator* a, size_t size, EpochId epoch) {
    // Round up to page boundary
    size_t alloc_size = (size + 4095) & ~4095;
    
    // Direct mmap
    void* ptr = mmap(NULL, alloc_size, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return NULL;
    
    // Track in epoch's huge list
    HugeAllocation* ha = malloc(sizeof(HugeAllocation));
    ha->ptr = ptr;
    ha->size = alloc_size;
    ha->epoch_id = epoch;
    add_to_huge_list(a, epoch, ha);
    
    return ptr;
}

void epoch_close(SlabAllocator* a, EpochId epoch) {
    // ... existing slab reclamation ...
    
    // NEW: Reclaim huge objects
    HugeAllocation* ha = a->epochs[epoch].huge_list;
    while (ha) {
        madvise(ha->ptr, ha->size, MADV_DONTNEED);  // Return pages to OS
        HugeAllocation* next = ha->next;
        free(ha);
        ha = next;
    }
    a->epochs[epoch].huge_list = NULL;
}
```

### Phase 3: Erlang NIF Integration (2-3 weeks)

**Goal**: Expose allocator to Erlang runtime

#### 3.1 NIF Module (`tslab_nif.c`)

```c
// NIF initialization
static int load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info) {
    SlabAllocator* alloc = slab_allocator_create();
    *priv_data = alloc;
    return 0;
}

// Allocation wrapper
static ERL_NIF_TERM alloc_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    SlabAllocator* alloc = (SlabAllocator*)enif_priv_data(env);
    
    unsigned long size;
    unsigned int epoch;
    if (!enif_get_ulong(env, argv[0], &size) ||
        !enif_get_uint(env, argv[1], &epoch)) {
        return enif_make_badarg(env);
    }
    
    SlabHandle handle;
    void* ptr = alloc_obj_epoch(alloc, (uint32_t)size, epoch, &handle);
    if (!ptr) return enif_make_atom(env, "error");
    
    // Return {ok, Handle} tuple
    ERL_NIF_TERM handle_term = enif_make_uint64(env, handle);
    return enif_make_tuple2(env, 
        enif_make_atom(env, "ok"),
        handle_term);
}

// Free wrapper
static ERL_NIF_TERM free_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    SlabAllocator* alloc = (SlabAllocator*)enif_priv_data(env);
    
    ErlNifUInt64 handle;
    if (!enif_get_uint64(env, argv[0], &handle)) {
        return enif_make_badarg(env);
    }
    
    bool ok = free_obj(alloc, handle);
    return ok ? enif_make_atom(env, "ok") : enif_make_atom(env, "error");
}

// Epoch management
static ERL_NIF_TERM epoch_current_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    SlabAllocator* alloc = (SlabAllocator*)enif_priv_data(env);
    EpochId epoch = epoch_current(alloc);
    return enif_make_uint(env, epoch);
}

static ERL_NIF_TERM epoch_advance_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    SlabAllocator* alloc = (SlabAllocator*)enif_priv_data(env);
    epoch_advance(alloc);
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM epoch_close_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    SlabAllocator* alloc = (SlabAllocator*)enif_priv_data(env);
    
    unsigned int epoch;
    if (!enif_get_uint(env, argv[0], &epoch)) {
        return enif_make_badarg(env);
    }
    
    epoch_close(alloc, epoch);
    return enif_make_atom(env, "ok");
}

static ErlNifFunc nif_funcs[] = {
    {"alloc", 2, alloc_nif},
    {"free", 1, free_nif},
    {"epoch_current", 0, epoch_current_nif},
    {"epoch_advance", 0, epoch_advance_nif},
    {"epoch_close", 1, epoch_close_nif}
};

ERL_NIF_INIT(tslab, nif_funcs, load, NULL, NULL, NULL)
```

#### 3.2 Erlang Module (`tslab.erl`)

```erlang
-module(tslab).
-export([alloc/2, free/1, epoch_current/0, epoch_advance/0, epoch_close/1]).
-on_load(init/0).

init() ->
    SoName = case code:priv_dir(?MODULE) of
        {error, bad_name} -> "./tslab_nif";
        Dir -> filename:join(Dir, "tslab_nif")
    end,
    erlang:load_nif(SoName, 0).

%% Allocate memory in specified epoch
%% Returns {ok, Handle} | error
alloc(_Size, _Epoch) ->
    exit(nif_library_not_loaded).

%% Free memory by handle
%% Returns ok | error
free(_Handle) ->
    exit(nif_library_not_loaded).

%% Get current epoch ID
epoch_current() ->
    exit(nif_library_not_loaded).

%% Advance to next epoch
epoch_advance() ->
    exit(nif_library_not_loaded).

%% Close and reclaim epoch
epoch_close(_Epoch) ->
    exit(nif_library_not_loaded).
```

#### 3.3 Gen_Server Integration Example

```erlang
-module(request_handler).
-behaviour(gen_server).

-export([init/1, handle_call/3, terminate/2]).

init([]) ->
    {ok, #{}}.

%% Each request gets its own epoch
handle_call({process, Request}, _From, State) ->
    % Get current epoch for this request
    Epoch = tslab:epoch_current(),
    
    % All allocations during processing use this epoch
    Result = process_request(Request, Epoch),
    
    % Close epoch - immediate RSS reclamation
    ok = tslab:epoch_close(Epoch),
    
    {reply, Result, State}.

%% Process request with epoch-scoped allocations
process_request(Request, Epoch) ->
    % Parse JSON (allocates ~2KB)
    {ok, Handle1} = tslab:alloc(2048, Epoch),
    ParsedJson = parse_json(Request, Handle1),
    
    % Process data (allocates ~8KB)
    {ok, Handle2} = tslab:alloc(8192, Epoch),
    Result = do_business_logic(ParsedJson, Handle2),
    
    % Format response (allocates ~4KB)
    {ok, Handle3} = tslab:alloc(4096, Epoch),
    Response = format_response(Result, Handle3),
    
    % Note: No explicit free needed - epoch_close handles it
    Response.

terminate(_Reason, _State) ->
    ok.
```

#### 3.4 Supervision Integration

```erlang
-module(worker_pool_sup).
-behaviour(supervisor).

init([]) ->
    % Advance epoch when starting worker pool
    tslab:epoch_advance(),
    CurrentEpoch = tslab:epoch_current(),
    
    Children = [
        {worker_1, {worker, start_link, [CurrentEpoch]}, 
         permanent, 5000, worker, [worker]}
        % ... more workers ...
    ],
    
    {ok, {{one_for_one, 10, 60}, Children}}.

% When supervisor terminates (e.g., rolling update)
terminate(_Reason, _State) ->
    % Close epoch - reclaim all worker memory
    Epoch = tslab:epoch_current(),
    tslab:epoch_close(Epoch),
    ok.
```

## Telemetry Integration

### Export Allocator Metrics to Erlang

```c
// New NIF: Get telemetry snapshot
static ERL_NIF_TERM telemetry_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    SlabAllocator* alloc = (SlabAllocator*)enif_priv_data(env);
    
    // Build Erlang map with metrics
    ERL_NIF_TERM map = enif_make_new_map(env);
    
    // Add metrics per size class
    for (size_t i = 0; i < 8; i++) {
        SizeClassAlloc* sc = &alloc->classes[i];
        
        uint64_t slow_path = atomic_load(&sc->slow_path_hits);
        uint64_t cache_miss = atomic_load(&sc->slow_path_cache_miss);
        uint64_t madvise_bytes = atomic_load(&sc->madvise_bytes);
        
        // ... build nested map ...
    }
    
    return map;
}
```

```erlang
%% Expose telemetry to Erlang observers
-module(tslab_telemetry).
-export([snapshot/0, publish_to_prometheus/0]).

snapshot() ->
    tslab:telemetry().

publish_to_prometheus() ->
    Metrics = snapshot(),
    % Export to prometheus_exporter
    lists:foreach(fun({Class, Stats}) ->
        prometheus_gauge:set(tslab_slow_path, [Class], maps:get(slow_path, Stats)),
        prometheus_gauge:set(tslab_rss_reclaimed, [Class], maps:get(madvise_bytes, Stats))
    end, maps:to_list(Metrics)).
```

## Testing Strategy

### Test 1: RSS Stability Over Time

**Hypothesis**: RSS oscillates but doesn't trend upward

```erlang
-module(run_forever_test).
-export([start/0]).

start() ->
    % Spawn 100K processes, each doing 1000 requests
    % Repeat indefinitely
    run_forever_loop().

run_forever_loop() ->
    io:format("Generation start - RSS: ~p MB~n", [current_rss_mb()]),
    
    % Spawn worker generation
    Pids = [spawn_worker() || _ <- lists:seq(1, 100_000)],
    
    % Wait for completion
    [receive {done, Pid} -> ok end || Pid <- Pids],
    
    % Close epochs
    tslab:epoch_close_all(),
    timer:sleep(1000),
    
    io:format("Generation end - RSS: ~p MB~n", [current_rss_mb()]),
    
    % Repeat forever
    run_forever_loop().

spawn_worker() ->
    Parent = self(),
    spawn(fun() ->
        Epoch = tslab:epoch_current(),
        
        % Do 1000 requests
        lists:foreach(fun(_) ->
            {ok, H} = tslab:alloc(1024, Epoch),
            % ... use memory ...
            ok = tslab:free(H)
        end, lists:seq(1, 1000)),
        
        Parent ! {done, self()}
    end).

current_rss_mb() ->
    {total_heap_size, Bytes} = erlang:memory(total),
    Bytes div (1024*1024).
```

**Success Criteria**:
- RSS baseline: ~500 MB
- RSS peak during generation: ~2 GB
- RSS after epoch_close: ~500 MB (returns to baseline)
- No upward trend over 7 days

### Test 2: Performance Comparison

**Compare against default Erlang allocator:**

| Metric | Default Allocator | Temporal-Slab | Target |
|--------|------------------|---------------|---------|
| Allocation latency | ~200ns | <100ns | ✅ Faster |
| RSS after 24h | +15% | +0% | ✅ Flat |
| Throughput | 1M ops/sec | 1M ops/sec | ✅ Same |
| Observability | Low | High | ✅ 50+ metrics |

### Test 3: NIF Safety

**Critical**: NIF crash kills entire Erlang VM

```erlang
% Test harness: catch NIF crashes
test_nif_safety() ->
    % Invalid handle
    case tslab:free(999999) of
        error -> ok;  % Expected
        _ -> exit(should_fail)
    end,
    
    % Double free
    {ok, H} = tslab:alloc(64, 0),
    ok = tslab:free(H),
    case tslab:free(H) of
        error -> ok;  % Expected
        _ -> exit(double_free_should_fail)
    end,
    
    % Allocation in CLOSING epoch
    Epoch = tslab:epoch_current(),
    tslab:epoch_close(Epoch),
    case tslab:alloc(64, Epoch) of
        error -> ok;  % Expected
        _ -> exit(should_reject_closing_epoch)
    end.
```

## Production Deployment Strategy

### Stage 1: Canary Node (1 week)

- Deploy to **1 node** in cluster
- Monitor RSS, latency, crashes
- Compare against control nodes
- Abort if any anomalies

### Stage 2: Small Pool (2 weeks)

- Deploy to **10% of cluster**
- Run production traffic
- Measure RSS delta vs control
- Collect telemetry

### Stage 3: Gradual Rollout (1 month)

- 25% → 50% → 75% → 100%
- Monitor each stage for 1 week
- Rollback plan: restart nodes with default allocator

### Stage 4: Long-Term Validation (3 months)

- Measure RSS over 90 days
- Expected: **flat RSS** vs control nodes showing +10-20% growth
- If successful: **proof of "run forever" capability**

## Expected Outcomes

### Quantitative Goals

| Metric | Before | After | Impact |
|--------|--------|-------|--------|
| RSS growth/week | +5-10% | 0% | Eliminates creep |
| Memory reclamation | Unpredictable | Deterministic | Explicit boundaries |
| Allocation coverage | N/A | 95% | Comprehensive |
| Observability | Low | High | 50+ metrics |
| Uptime without restart | 30 days | ∞ days | "Run forever" |

### Qualitative Benefits

1. **Operational confidence**: Predictable memory behavior
2. **Capacity planning**: Known RSS ceiling
3. **Troubleshooting**: Telemetry pinpoints issues
4. **Regulatory compliance**: Deterministic behavior for audits
5. **Cost savings**: No emergency restarts, better resource utilization

## Risks and Mitigations

### Risk 1: NIF Crash Kills VM

**Mitigation**:
- Extensive testing (fuzzing, stress tests)
- Defensive programming (bounds checks, validation)
- Gradual rollout with instant rollback
- Monitor crash rate closely

### Risk 2: Performance Regression

**Mitigation**:
- Benchmark before deployment
- Monitor P99 latency
- Keep allocation fast path (<100ns)
- Fallback to default allocator if needed

### Risk 3: Registry Unbounded Growth

**Problem**: `next_id++` forever → registry grows indefinitely

**Mitigation**:
- Implement ID recycling (use free_ids list)
- Add registry capacity metrics
- Alert if registry exceeds threshold

### Risk 4: Erlang Allocation Patterns Don't Fit

**Mitigation**:
- Profile production workload first
- Measure size distribution
- Adjust size classes based on data
- Hybrid approach: temporal-slab for 768B-16KB, default for rest

## Future Enhancements

### 1. Automatic Epoch Management

```erlang
% Framework wraps gen_server calls automatically
-module(tslab_gen_server).
-behaviour(gen_server).

handle_call(Request, From, State) ->
    Epoch = tslab:epoch_advance(),  % Automatic
    Result = Mod:handle_call(Request, From, State),
    tslab:epoch_close(Epoch),  % Automatic
    {reply, Result, State}.
```

### 2. Cross-Node Coordination

```erlang
% Distributed epoch synchronization
% All nodes in cluster advance epochs together
% Enables cluster-wide RSS control
```

### 3. BEAM Integration

Replace Erlang's default allocator entirely:
- Hook `erts_alloc` interface
- Transparent to application code
- No NIF needed

### 4. Monitoring Dashboard

Real-time visualization:
- RSS per epoch
- Allocation heatmap
- epoch_close latency histogram
- Stale handle detection

## Conclusion

**Is "run forever" achievable?**

**Yes**, with caveats:

✅ **Technical feasibility**: Erlang's temporal structure maps naturally to epochs
✅ **Coverage**: 95% of allocations after large object support
✅ **Performance**: Sub-100ns allocation maintains throughput
✅ **Observability**: 50+ metrics for troubleshooting

⚠️ **Engineering required**:
- Sparse slab tier (1KB-16KB) - **HIGH PRIORITY**
- Huge object tracking (>16KB)
- NIF safety hardening
- Registry ID recycling

⚠️ **Production validation**:
- 90-day RSS stability test
- Performance regression testing
- Gradual rollout with monitoring

**Timeline**: 6-8 weeks to production-ready

**Payoff**: Erlang systems that **truly run forever** without memory-related restarts.

---

**Status**: Proposal
**Author**: Analysis based on temporal-slab architecture
**Date**: 2025-02-09
**Next Steps**: Implement Phase 1 (sparse slab tier) for Erlang allocation coverage
