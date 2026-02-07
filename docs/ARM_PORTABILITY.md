# ARM Portability: Atomics and Memory Ordering

Analysis of temporal-slab's atomic counter implementation for ARM64 (AArch64) architectures.

---

## TL;DR: Will ARM Be Problematic?

**Short answer:** No, but there are important differences to understand.

**Key points:**
- `memory_order_relaxed` is **more expensive on ARM** than x86 (but still cheap)
- ARM's weak memory model requires explicit barriers for ordering
- Counter correctness is **unaffected** (atomicity still guaranteed)
- Performance characteristics change slightly (quantified below)

**Bottom line:** temporal-slab's design is ARM-friendly, but relaxed atomics cost more than x86.

---

## 1. x86 vs ARM: Memory Model Differences

### x86-64: Total Store Order (TSO)

**Properties:**
- Stores are visible to all cores in program order (mostly)
- Loads cannot be reordered with other loads
- Stores cannot be reordered with other stores
- Only load-store reordering is allowed

**Result:** x86 is "strong" memory model—most ordering is automatic.

---

### ARM64: Weakly-Ordered Memory

**Properties:**
- Loads and stores can be reordered freely (unless barriers used)
- Each core has its own view of memory (until synchronization)
- Requires explicit barriers (`dmb`, `dsb`) for ordering

**Result:** ARM is "weak" memory model—must explicitly request ordering.

---

## 2. Atomic Operations on ARM

### `atomic_fetch_add` with `memory_order_relaxed`

**x86 implementation:**
```asm
lock incq (%rdi)    ; Atomic increment with lock prefix
                     ; Implicit cache coherence (MESI protocol)
                     ; No memory barriers
```

**Cost:** 1-2 cycles (lock prefix is nearly free when cache line is owned)

---

**ARM64 implementation:**
```asm
ldaxr  x0, [x1]     ; Load-acquire exclusive (reserves cache line)
add    x0, x0, #1   ; Increment in register
stlxr  w2, x0, [x1] ; Store-release exclusive (writes if reservation valid)
cbnz   w2, retry    ; Retry if store failed (another core modified)
```

**Cost:** 3-10 cycles depending on cache state and contention

**Breakdown:**
- `ldaxr`: Load-acquire exclusive (~2-3 cycles, reserves cache line)
- `add`: Arithmetic (~1 cycle)
- `stlxr`: Store-release exclusive (~2-3 cycles if succeeds)
- Retry loop: If another core modified cache line, retry entire sequence

---

### Why ARM is More Expensive

**1. Load-link/store-conditional (LL/SC) architecture:**
- ARM uses reservation-based atomics (not single instruction)
- Must retry if reservation broken (contention or cache eviction)

**2. Cache coherence protocol differences:**
- x86: MESI protocol with implicit coherence
- ARM: Requires explicit acquire/release semantics for visibility

**3. Memory barriers:**
- x86: `lock` prefix provides barrier-like semantics "for free"
- ARM: Must use `dmb` (data memory barrier) for ordering (but not needed for relaxed)

---

## 3. Does `memory_order_relaxed` Differ on ARM?

### Semantics Are the Same

**C11 standard guarantees:**
- Atomicity (operation is indivisible)
- No torn reads/writes
- No lost updates (RMW is atomic)

These guarantees hold on **both x86 and ARM**.

---

### What Changes: Ordering Guarantees

**Key difference:** ARM's LL/SC instructions (`ldaxr`/`stlxr`) provide **acquire/release** semantics even for `memory_order_relaxed`.

**Why?** ARM's ISA doesn't have "plain atomic RMW" instruction—all atomic RMW uses acquire/release instructions.

---

### Practical Impact on temporal-slab

**temporal-slab's counter usage:**
```c
atomic_fetch_add_explicit(&sc->slow_path_hits, 1, memory_order_relaxed);
```

**On ARM:**
- `ldaxr`/`add`/`stlxr` (~3-10 cycles)
- Acquire/release semantics (stronger than requested, but acceptable)

**Result:** ARM's relaxed atomics are **stronger** than x86's (acquire/release vs no ordering), but **slower** (3-10 cycles vs 1-2 cycles).

---

## 4. Performance Impact on ARM

### Microbenchmark: Atomic Increment Cost

**x86-64:**
```
for (int i = 0; i < 1000000; i++) {
  atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
}
// Time: ~2ms (2ns per increment)
```

**ARM64 (estimated):**
```
for (int i = 0; i < 1000000; i++) {
  atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
}
// Time: ~5-10ms (5-10ns per increment)
```

**Ratio:** ARM is **2.5-5× slower** for atomic increments.

---

### Impact on Allocator Performance

**Recall:** Counters are only updated on **slow path** (<1% of allocations).

**x86:**
- 99% fast path: 0 counter updates
- 1% slow path: 1-2 counter updates × 2ns = 2-4ns overhead
- Total: **<0.1% overhead**

**ARM:**
- 99% fast path: 0 counter updates
- 1% slow path: 1-2 counter updates × 10ns = 10-20ns overhead
- Total: **<0.2% overhead**

**Comparison:**
- Lock acquisition: ~50ns (ARM), ~20ns (x86)
- Counter overhead: ~20ns (ARM), ~4ns (x86)
- **Counter overhead is still negligible compared to lock**

**Result:** ARM's higher atomic cost is still acceptable because counters are on slow path.

---

## 5. Will Contention Be Worse on ARM?

### ARM is Worse Under Contention

**LL/SC can fail spuriously:**
- Cache line eviction breaks reservation
- Context switch breaks reservation
- More retries needed

**x86 behavior:**
- `lock incq` holds cache line exclusively
- Cost: ~10-20 cycles per contended increment

**ARM behavior:**
- `ldaxr`/`stlxr` reserves cache line, can fail
- Cost: ~20-50 cycles per contended increment (more retries)

---

### Mitigation: Per-Size-Class Counters

**temporal-slab design:**
```c
struct SlabAllocator {
  SizeClassAlloc classes[8];  // 8 independent size classes
};

struct SizeClassAlloc {
  _Atomic uint64_t slow_path_hits;  // Per-class counter
};
```

**Key property:** Threads allocating **different sizes** hit **different counters**.

**Result:** Per-size-class sharding prevents contention on both x86 and ARM.

---

## 6. Memory Barriers: Do We Need Stronger Ordering?

### Why Relaxed is Still Sufficient

**Counters are observational, not operational:**
```c
// Allocator correctness path
pthread_mutex_lock(&sc->lock);   // Full barrier (acquire)
  // Manipulate partial/full lists
pthread_mutex_unlock(&sc->lock); // Full barrier (release)

// Counter update (outside critical section)
atomic_fetch_add_explicit(&sc->slow_path_hits, 1, memory_order_relaxed);
```

**Key insight:** Allocator correctness is protected by **mutexes** (which provide acquire/release barriers).

**Counter updates don't participate in correctness:**
- If counter increment is reordered, allocator behavior is **unaffected**
- If stats reader sees stale counter value, it's **acceptable** (monitoring tolerance)

**Result:** `memory_order_relaxed` is sufficient on ARM because counters are **observational only**.

---

## 7. Cache Line Alignment: ARM-Specific Concerns

### False Sharing

**x86 cache line:** 64 bytes  
**ARM cache line:** 64 bytes (ARMv8), 128 bytes (some implementations)

**temporal-slab counters:**
```c
struct SizeClassAlloc {
  pthread_mutex_t lock;                 // ~40B
  _Atomic uint64_t slow_path_hits;      // 8B  ← Could be on same cache line as lock
  // ...
};
```

**Potential problem:** If `lock` and `slow_path_hits` are on same cache line:
- Thread A locks mutex → cache line becomes exclusive
- Thread B increments counter → **cache line ping-pong**

---

### Mitigation: Explicit Cache Line Padding

**Solution:** Use `_Alignas` to separate counters from lock:
```c
struct SizeClassAlloc {
  pthread_mutex_t lock;
  
  _Alignas(64) _Atomic uint64_t slow_path_hits;  // Force new cache line
  _Atomic uint64_t new_slab_count;
  // ...
};
```

**Benefit:**
- Guarantees counters are on **different cache line** from lock
- Prevents false sharing on both x86 and ARM
- Minimal memory cost (64 bytes padding per size class = 512 bytes total)

**Status:** Recommended optimization for ARM deployment.

---

## 8. ARM-Specific Optimizations

### 1. Use ARMv8.1 LSE Instructions

**ARMv8.1 Large System Extensions provide:**
- `ldadd` (atomic add) instruction (single instruction instead of loop)
- `cas` (compare-and-swap) instruction

**Benefit:** Atomic increments drop from 3-10 cycles to **2-3 cycles** (closer to x86).

**Compiler flag:**
```bash
gcc -march=armv8-a+lse
```

**Availability:** ARMv8.1+ CPU (2016+), widely available on modern ARM servers.

---

### 2. Cache Line Padding (Recommended)

**Add explicit padding between lock and counters:**
```c
struct SizeClassAlloc {
  pthread_mutex_t lock;
  char padding1[64 - sizeof(pthread_mutex_t) % 64];  // Align to 64B
  
  _Alignas(64) _Atomic uint64_t slow_path_hits;
  _Atomic uint64_t new_slab_count;
  // ...
};
```

**Benefit:** Prevents false sharing on ARM's cache coherence protocol.

---

### 3. Keep `memory_order_relaxed` (Already Optimal)

**Current:**
```c
atomic_fetch_add_explicit(&sc->slow_path_hits, 1, memory_order_relaxed);
```

**Why it matters:**
- `memory_order_seq_cst` requires `dmb` barriers on ARM (~10-20 cycles)
- `memory_order_relaxed` uses `ldaxr`/`stlxr` only (~3-10 cycles)
- **Already optimal**

---

## 9. Performance Estimates: x86 vs ARM

### Allocation Microbenchmark

**Scenario:** 1 million allocations (99% fast path, 1% slow path)

**x86-64:**
```
Fast path:  990,000 × 10ns = 9.9ms
Slow path:   10,000 × 100ns (lock + counter) = 1.0ms
Total: 10.9ms
```

**ARM64 (without LSE):**
```
Fast path:  990,000 × 15ns = 14.85ms
Slow path:   10,000 × 150ns (lock + counter) = 1.5ms
Total: 16.35ms
```

**ARM64 (with LSE):**
```
Fast path:  990,000 × 15ns = 14.85ms
Slow path:   10,000 × 120ns (lock + faster counter) = 1.2ms
Total: 16.05ms
```

**Ratio:** ARM is ~1.5× slower overall (CPU architecture), but counter overhead is still <1%.

---

### Counter-Only Overhead

**x86:**
- Counter overhead: 1% slow-path × 2ns per counter = **0.02ns per allocation**

**ARM (with LSE):**
- Counter overhead: 1% slow-path × 3ns per counter = **0.03ns per allocation**

**ARM (without LSE):**
- Counter overhead: 1% slow-path × 10ns per counter = **0.1ns per allocation**

**Context:** Allocation itself is ~10-15ns, so counter overhead is **still <1%**.

---

## 10. Summary: ARM Portability

### Will ARM Be Problematic?

**No, but understand these differences:**

1. **Atomics are more expensive on ARM:**
   - x86: 1-2 cycles per atomic increment
   - ARM: 3-10 cycles (no LSE), 2-3 cycles (with LSE)
   - **Still negligible on slow path**

2. **Memory model is weaker:**
   - ARM requires explicit barriers for ordering
   - `memory_order_relaxed` is sufficient for temporal-slab
   - Counters don't participate in correctness

3. **Contention behavior differs:**
   - ARM's LL/SC can fail spuriously (more retries)
   - Per-size-class sharding prevents contention
   - **Rarely an issue in practice**

4. **Cache coherence is explicit:**
   - ARM requires acquire/release for visibility
   - `ldaxr`/`stlxr` provide this automatically
   - **No code changes needed**

---

### Recommended ARM Optimizations

**1. Use ARMv8.1 LSE instructions:**
```bash
gcc -march=armv8-a+lse
```
Benefit: Atomic increments drop from 3-10 cycles to 2-3 cycles.

**2. Add cache line padding:**
```c
struct SizeClassAlloc {
  pthread_mutex_t lock;
  _Alignas(64) _Atomic uint64_t slow_path_hits;  // New cache line
  // ...
};
```
Benefit: Prevents lock/counter contention on same cache line.

**3. Keep `memory_order_relaxed`:**
No changes needed—relaxed ordering is correct and sufficient.

---

### Performance Expectations on ARM

**Overall allocator:**
- ARM is ~1.5× slower than x86 (CPU architecture, not atomics)
- Counter overhead remains **<1% on ARM** (same as x86)

**Counter-specific:**
- x86: 0.02ns overhead per allocation
- ARM (LSE): 0.03ns overhead per allocation
- ARM (no LSE): 0.1ns overhead per allocation

**Conclusion:** Counter overhead is **acceptable on ARM**—design is portable.

---

### What Stays the Same

**These properties hold on both x86 and ARM:**
- Atomicity (no torn reads/writes)
- No lost updates (RMW is atomic)
- Per-size-class sharding (no contention)
- Hot path is counter-free (99% of allocations)
- Sub-1% CPU overhead for observability

**Result:** temporal-slab's observability design is **ARM-friendly**.

---

**Related docs:**
- `observability_design.md` - Phase 2 observability architecture
- `stats_dump_reference.md` - JSON field definitions
- `../temporal-slab-tools/docs/METRICS_PERFORMANCE.md` - x86 performance analysis
