# Phase 2.3 Implementation Plan (CORRECTED)

**Incorporates 6 critical fixes to prevent regressions and correctness bugs.**

**Effort:** 6.5 hours  
**Files to modify:** 7 files  

---

## The 6 Fixes Applied

1. ✅ Use per-allocator mutex (not static global)
2. ✅ Rename `alloc_count` → `refcount` (semantic clarity)
3. ✅ Use `CLOCK_MONOTONIC` for age (not REALTIME)
4. ✅ Escape JSON strings in labels (prevent invalid JSON)
5. ✅ Keep `schema_version: 1`, make epochs optional (backward compat)
6. ✅ Fix Grafana PromQL (label matchers can't do numeric comparisons)

---

## Step 1: Fix EpochMetadata Struct (src/slab_alloc_internal.h)

**Current:**
```c
typedef struct EpochMetadata {
  uint64_t open_since_ns;       /* Timestamp when epoch became ACTIVE */
  _Atomic uint64_t alloc_count; /* Number of live allocations */  ← RENAME THIS
  char label[32];
  
  uint64_t rss_before_close;
  uint64_t rss_after_close;
} EpochMetadata;
```

**Change to:**
```c
typedef struct EpochMetadata {
  uint64_t open_since_ns;       /* CLOCK_MONOTONIC timestamp when epoch became ACTIVE */
  _Atomic uint64_t refcount;    /* Domain refcount (enter/exit tracking) */
  char label[32];               /* Semantic label (protected by allocator->epoch_label_lock) */
  
  uint64_t rss_before_close;    /* Phase 2.4 */
  uint64_t rss_after_close;     /* Phase 2.4 */
} EpochMetadata;
```

---

## Step 2: Add Label Lock to SlabAllocator (src/slab_alloc_internal.h)

**Add field to `SlabAllocator`:**
```c
struct SlabAllocator {
  SizeClassAlloc classes[8];
  
  _Atomic uint32_t current_epoch;
  uint32_t epoch_count;
  
  _Atomic uint32_t epoch_state[EPOCH_COUNT];
  
  _Atomic uint64_t epoch_era_counter;
  uint64_t epoch_era[EPOCH_COUNT];
  
  EpochMetadata epoch_meta[EPOCH_COUNT];
  
  pthread_mutex_t epoch_label_lock;  /* NEW: Protects label writes (rare) */
  
  SlabRegistry reg;
};
```

---

## Step 3: Initialize Label Lock (src/slab_alloc.c)

**In `slab_allocator_create()`, add:**
```c
SlabAllocator* slab_allocator_create(void) {
  /* ... existing initialization ... */
  
  /* Initialize epoch metadata */
  for (uint32_t i = 0; i < EPOCH_COUNT; i++) {
    a->epoch_meta[i].open_since_ns = 0;
    atomic_init(&a->epoch_meta[i].refcount, 0);  /* Changed from alloc_count */
    a->epoch_meta[i].label[0] = '\0';
    a->epoch_meta[i].rss_before_close = 0;
    a->epoch_meta[i].rss_after_close = 0;
  }
  
  /* Initialize label lock */
  pthread_mutex_init(&a->epoch_label_lock, NULL);
  
  /* ... rest of initialization ... */
}
```

**In `slab_allocator_free()`, add:**
```c
void slab_allocator_free(SlabAllocator* a) {
  /* ... existing cleanup ... */
  
  pthread_mutex_destroy(&a->epoch_label_lock);
  
  /* ... rest of cleanup ... */
}
```

---

## Step 4: Fix epoch_advance() to Use CLOCK_MONOTONIC (src/slab_alloc.c)

**Current code at line 1235:**
```c
/* Phase 2.3: Reset metadata for new epoch */
a->epoch_meta[new_epoch].open_since_ns = now_ns();
atomic_store_explicit(&a->epoch_meta[new_epoch].alloc_count, 0, memory_order_relaxed);
a->epoch_meta[new_epoch].label[0] = '\0';
```

**Change to:**
```c
/* Phase 2.3: Reset metadata for new epoch */
struct timespec ts;
clock_gettime(CLOCK_MONOTONIC, &ts);  /* MONOTONIC not REALTIME */
a->epoch_meta[new_epoch].open_since_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

atomic_store_explicit(&a->epoch_meta[new_epoch].refcount, 0, memory_order_relaxed);  /* Renamed */
a->epoch_meta[new_epoch].label[0] = '\0';
```

---

## Step 5: Implement Public APIs (include/slab_alloc.h + src/slab_alloc.c)

**Add to public header (include/slab_alloc.h):**

```c
/* ==================== Phase 2.3: Semantic Attribution APIs ==================== */

void slab_epoch_set_label(SlabAllocator* alloc, EpochId epoch, const char* label);
void slab_epoch_inc_refcount(SlabAllocator* alloc, EpochId epoch);
void slab_epoch_dec_refcount(SlabAllocator* alloc, EpochId epoch);
uint64_t slab_epoch_get_refcount(SlabAllocator* alloc, EpochId epoch);
```

**Implement in src/slab_alloc.c:**

```c
/* ==================== Phase 2.3: Semantic Attribution APIs ==================== */

void slab_epoch_set_label(SlabAllocator* a, EpochId epoch, const char* label) {
  if (!a || epoch >= a->epoch_count || !label) return;
  
  EpochMetadata* meta = &a->epoch_meta[epoch];
  
  pthread_mutex_lock(&a->epoch_label_lock);
  strncpy(meta->label, label, sizeof(meta->label) - 1);
  meta->label[sizeof(meta->label) - 1] = '\0';
  pthread_mutex_unlock(&a->epoch_label_lock);
}

void slab_epoch_inc_refcount(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return;
  
  atomic_fetch_add_explicit(&a->epoch_meta[epoch].refcount, 1, memory_order_relaxed);
}

void slab_epoch_dec_refcount(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return;
  
  uint64_t prev = atomic_load_explicit(&a->epoch_meta[epoch].refcount, memory_order_relaxed);
  if (prev > 0) {
    atomic_fetch_sub_explicit(&a->epoch_meta[epoch].refcount, 1, memory_order_relaxed);
  }
}

uint64_t slab_epoch_get_refcount(SlabAllocator* a, EpochId epoch) {
  if (!a || epoch >= a->epoch_count) return 0;
  
  return atomic_load_explicit(&a->epoch_meta[epoch].refcount, memory_order_relaxed);
}
```

---

## Step 6: Add JSON String Escaper (src/stats_dump.c)

**Add helper function before `print_json_global()`:**

```c
/* JSON string escaper (prevent invalid JSON from label contents) */
static void print_json_string(const char* s) {
  if (!s) {
    printf("\"\"");
    return;
  }
  
  printf("\"");
  for (const char* p = s; *p && (p - s) < 31; p++) {
    switch (*p) {
      case '"':  printf("\\\""); break;
      case '\\': printf("\\\\"); break;
      case '\n': printf("\\n"); break;
      case '\r': printf("\\r"); break;
      case '\t': printf("\\t"); break;
      default:
        if (*p >= 32 && *p <= 126) {
          putchar(*p);
        } else {
          printf("\\u%04x", (unsigned char)*p);
        }
    }
  }
  printf("\"");
}
```

---

## Step 7: Add Epochs Array to JSON (src/stats_dump.c)

**Modify `print_json_global()` - add before final closing brace:**

```c
static void print_json_global(SlabAllocator* alloc) {
  /* ... existing code for global + classes ... */
  
  printf("  \"classes\": [\n");
  /* ... existing classes loop ... */
  printf("  ],\n");  /* CHANGED: Added comma */
  
  /* NEW: Epochs array */
  printf("  \"epochs\": [\n");
  
  for (uint32_t epoch_id = 0; epoch_id < 16; epoch_id++) {
    /* Aggregate epoch stats across all size classes */
    uint64_t total_partial = 0;
    uint64_t total_full = 0;
    uint64_t total_reclaimable = 0;
    
    for (uint32_t cls = 0; cls < 8; cls++) {
      SlabEpochStats es;
      slab_stats_epoch(alloc, cls, epoch_id, &es);
      
      total_partial += es.partial_slab_count;
      total_full += es.full_slab_count;
      total_reclaimable += es.reclaimable_slab_count;
    }
    
    /* Get metadata */
    EpochMetadata* meta = &alloc->epoch_meta[epoch_id];
    uint64_t refcount = atomic_load_explicit(&meta->refcount, memory_order_relaxed);
    uint64_t era = alloc->epoch_era[epoch_id];
    EpochLifecycleState state = (EpochLifecycleState)atomic_load_explicit(
        &alloc->epoch_state[epoch_id], memory_order_relaxed);
    
    /* Calculate age using CLOCK_MONOTONIC */
    uint64_t age_sec = 0;
    if (meta->open_since_ns > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);  /* MONOTONIC not REALTIME */
      uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
      if (now_ns >= meta->open_since_ns) {
        age_sec = (now_ns - meta->open_since_ns) / 1000000000ULL;
      }
    }
    
    /* Copy label safely */
    char label_copy[32];
    pthread_mutex_lock(&alloc->epoch_label_lock);
    strncpy(label_copy, meta->label, sizeof(label_copy) - 1);
    label_copy[sizeof(label_copy) - 1] = '\0';
    pthread_mutex_unlock(&alloc->epoch_label_lock);
    
    printf("    {\n");
    printf("      \"epoch_id\": %u,\n", epoch_id);
    printf("      \"epoch_era\": %lu,\n", era);
    printf("      \"state\": \"%s\",\n", state == EPOCH_ACTIVE ? "ACTIVE" : "CLOSING");
    printf("      \"age_sec\": %lu,\n", age_sec);
    printf("      \"refcount\": %lu,\n", refcount);
    printf("      \"label\": ");
    print_json_string(label_copy);
    printf(",\n");
    printf("      \"total_partial_slabs\": %lu,\n", total_partial);
    printf("      \"total_full_slabs\": %lu,\n", total_full);
    printf("      \"total_reclaimable_slabs\": %lu,\n", total_reclaimable);
    printf("      \"estimated_rss_bytes\": %lu\n", (total_partial + total_full) * 4096);
    printf("    }%s\n", epoch_id < 15 ? "," : "");
  }
  
  printf("  ]\n");
  printf("}\n");
}
```

---

## Step 8: Update Schema (temporal-slab-tools/schemas/stats_v1.schema.json)

**Make epochs OPTIONAL (backward compat):**

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "required": [
    "schema_version",
    ...existing required fields...,
    "classes"
  ],
  "properties": {
    ...existing properties...,
    "epochs": {
      "type": "array",
      "description": "Per-epoch statistics (Phase 2.3). Optional for backward compatibility.",
      "items": {
        "type": "object",
        "required": ["epoch_id", "epoch_era", "state", "age_sec", "refcount", 
                     "label", "total_partial_slabs", "total_full_slabs", 
                     "total_reclaimable_slabs", "estimated_rss_bytes"],
        "properties": {
          "epoch_id": { "type": "integer", "minimum": 0, "maximum": 15 },
          "epoch_era": { "type": "integer", "minimum": 0 },
          "state": { "type": "string", "enum": ["ACTIVE", "CLOSING"] },
          "age_sec": { "type": "integer", "minimum": 0 },
          "refcount": { "type": "integer", "minimum": 0 },
          "label": { "type": "string", "maxLength": 31 },
          "total_partial_slabs": { "type": "integer", "minimum": 0 },
          "total_full_slabs": { "type": "integer", "minimum": 0 },
          "total_reclaimable_slabs": { "type": "integer", "minimum": 0 },
          "estimated_rss_bytes": { "type": "integer", "minimum": 0 }
        }
      }
    }
  }
}
```

---

## Step 9: Update Go Structs (temporal-slab-tools/internal/schema/types.go)

**Add new type:**
```go
type EpochStatsV1 struct {
	EpochID               uint32 `json:"epoch_id"`
	EpochEra              uint64 `json:"epoch_era"`
	State                 string `json:"state"`
	AgeSec                uint64 `json:"age_sec"`
	Refcount              uint64 `json:"refcount"`
	Label                 string `json:"label"`
	TotalPartialSlabs     uint64 `json:"total_partial_slabs"`
	TotalFullSlabs        uint64 `json:"total_full_slabs"`
	TotalReclaimableSlabs uint64 `json:"total_reclaimable_slabs"`
	EstimatedRSSBytes     uint64 `json:"estimated_rss_bytes"`
}
```

**Add field to `StatsSnapshotV1`:**
```go
type StatsSnapshotV1 struct {
	// ... existing fields ...
	Classes []ClassStatsV1 `json:"classes"`
	Epochs  []EpochStatsV1 `json:"epochs,omitempty"`  /* omitempty for optional */
}
```

---

## Step 10: Implement `tslab top --epochs` (temporal-slab-tools/internal/view/epochs.go)

**Create new file:**
```go
package view

import (
	"fmt"
	"sort"
	"strings"

	"github.com/blackwd/temporal-slab-tools/internal/schema"
)

type epochRow struct {
	ID          uint32
	Era         uint64
	State       string
	AgeSec      uint64
	Refcount    uint64
	Label       string
	RSSBytes    uint64
	Reclaimable uint64
}

func RenderTopEpochs(s *schema.StatsSnapshotV1, by string, n int, desc bool) string {
	if len(s.Epochs) == 0 {
		return "epochs array not present in JSON (requires Phase 2.3)\n"
	}
	
	rows := make([]epochRow, 0, len(s.Epochs))

	for _, e := range s.Epochs {
		rows = append(rows, epochRow{
			ID:          e.EpochID,
			Era:         e.EpochEra,
			State:       e.State,
			AgeSec:      e.AgeSec,
			Refcount:    e.Refcount,
			Label:       e.Label,
			RSSBytes:    e.EstimatedRSSBytes,
			Reclaimable: e.TotalReclaimableSlabs * 4096,
		})
	}

	sort.Slice(rows, func(i, j int) bool {
		var vi, vj uint64
		switch by {
		case "rss_bytes":
			vi, vj = rows[i].RSSBytes, rows[j].RSSBytes
		case "age_sec":
			vi, vj = rows[i].AgeSec, rows[j].AgeSec
		case "refcount":
			vi, vj = rows[i].Refcount, rows[j].Refcount
		case "reclaimable_bytes":
			vi, vj = rows[i].Reclaimable, rows[j].Reclaimable
		default:
			vi, vj = rows[i].RSSBytes, rows[j].RSSBytes
		}

		if desc {
			if vi == vj {
				return rows[i].ID < rows[j].ID
			}
			return vi > vj
		}
		if vi == vj {
			return rows[i].ID < rows[j].ID
		}
		return vi < vj
	})

	if n <= 0 {
		n = 5
	}
	if n > len(rows) {
		n = len(rows)
	}

	var b strings.Builder
	fmt.Fprintf(&b, "top epochs by %s\n", by)
	fmt.Fprintf(&b, "%-6s %-10s %-8s %-8s %-9s %-10s %s\n", 
		"epoch", "era", "state", "age", "refcount", "rss", "label")
	
	for i := 0; i < n; i++ {
		r := rows[i]
		rssMB := fmt.Sprintf("%.2fMB", float64(r.RSSBytes)/(1024*1024))
		ageStr := fmt.Sprintf("%ds", r.AgeSec)
		fmt.Fprintf(&b, "%-6d %-10d %-8s %-8s %-9d %-10s %s\n",
			r.ID, r.Era, r.State, ageStr, r.Refcount, rssMB, r.Label)
	}

	return b.String()
}
```

**Update `cmd/top.go`:**
```go
if *epochs {
	fmt.Print(view.RenderTopEpochs(&snap, *by, *n, *desc))
	return nil
}
```

**Update `cmd/top.go` flags:**
```go
by := fs.String("by", "slow_path_hits", 
	"metric for classes: slow_path_hits | new_slab_count | madvise_bytes | net_slabs\n"+
	"       metric for epochs:  rss_bytes | age_sec | refcount | reclaimable_bytes")
```

---

## Step 11: Add Prometheus Epoch Metrics (temporal-slab-tools/cmd/export_prometheus.go)

**Add to `StatsDumpV1` struct:**
```go
type StatsDumpV1 struct {
	// ... existing fields ...
	Classes []ClassStatsV1 `json:"classes"`
	Epochs  []EpochStatsV1 `json:"epochs,omitempty"`
}

type EpochStatsV1 struct {
	EpochID               uint32 `json:"epoch_id"`
	EpochEra              uint64 `json:"epoch_era"`
	State                 string `json:"state"`
	AgeSec                uint64 `json:"age_sec"`
	Refcount              uint64 `json:"refcount"`
	Label                 string `json:"label"`
	TotalPartialSlabs     uint64 `json:"total_partial_slabs"`
	TotalFullSlabs        uint64 `json:"total_full_slabs"`
	TotalReclaimableSlabs uint64 `json:"total_reclaimable_slabs"`
	EstimatedRSSBytes     uint64 `json:"estimated_rss_bytes"`
}
```

**Add to `buildProm()` at the end:**
```go
func buildProm(prefix string, s *StatsDumpV1) ([]promDef, []promSample) {
	// ... existing defs and samples ...
	
	// Epoch metrics (use info metric for labels to avoid cardinality explosion)
	if len(s.Epochs) > 0 {
		defs = append(defs,
			promDef{name: fmt.Sprintf("%s_epoch_info", prefix), 
				help: "Epoch metadata (use label for text)", typ: promGauge},
			promDef{name: fmt.Sprintf("%s_epoch_age_seconds", prefix), 
				help: "Epoch age in seconds", typ: promGauge},
			promDef{name: fmt.Sprintf("%s_epoch_refcount", prefix), 
				help: "Domain refcount (number of active domain enters)", typ: promGauge},
			promDef{name: fmt.Sprintf("%s_epoch_estimated_rss_bytes", prefix), 
				help: "Epoch estimated RSS bytes", typ: promGauge},
			promDef{name: fmt.Sprintf("%s_epoch_reclaimable_bytes", prefix), 
				help: "Epoch reclaimable bytes", typ: promGauge},
		)
		
		for _, e := range s.Epochs {
			lbl := map[string]string{
				"epoch": fmt.Sprintf("%d", e.EpochID),
				"era":   fmt.Sprintf("%d", e.EpochEra),
				"state": e.State,
			}
			
			// Info metric with label (for dashboard drill-down)
			infoLbl := map[string]string{
				"epoch": fmt.Sprintf("%d", e.EpochID),
				"era":   fmt.Sprintf("%d", e.EpochEra),
				"state": e.State,
				"label": e.Label,
			}
			samples = append(samples,
				promSample{name: fmt.Sprintf("%s_epoch_info", prefix), labels: infoLbl, value: "1"},
			)
			
			// Numeric metrics (no label to avoid cardinality)
			samples = append(samples,
				promSample{name: fmt.Sprintf("%s_epoch_age_seconds", prefix), 
					labels: lbl, value: fmt.Sprintf("%d", e.AgeSec)},
				promSample{name: fmt.Sprintf("%s_epoch_refcount", prefix), 
					labels: lbl, value: fmt.Sprintf("%d", e.Refcount)},
				promSample{name: fmt.Sprintf("%s_epoch_estimated_rss_bytes", prefix), 
					labels: lbl, value: fmt.Sprintf("%d", e.EstimatedRSSBytes)},
				promSample{name: fmt.Sprintf("%s_epoch_reclaimable_bytes", prefix), 
					labels: lbl, value: fmt.Sprintf("%d", e.TotalReclaimableSlabs*4096)},
			)
		}
	}
	
	return defs, samples
}
```

---

## Step 12: Fix Grafana PromQL Queries (temporal-slab-tools/dashboards/temporal-slab-observability.json)

**Add epoch panels with CORRECTED queries:**

```json
{
  "datasource": { "type": "prometheus", "uid": "${datasource}" },
  "description": "Epochs with refcount leak (age > 5min AND refcount > 0)",
  "targets": [
    {
      "expr": "(temporal_slab_epoch_age_seconds > 300) and on (epoch, era, state) (temporal_slab_epoch_refcount > 0)",
      "format": "table",
      "instant": true,
      "refId": "A"
    }
  ],
  "title": "Refcount leaks (age > 5min, refcount > 0)",
  "type": "table"
}
```

**Correct pattern for "epochs in CLOSING state with refcount":**
```promql
(temporal_slab_epoch_refcount{state="CLOSING"} > 0)
and on (epoch, era)
(temporal_slab_epoch_age_seconds > 60)
```

---

## Testing Checklist

### 1. Allocator Unit Tests

```c
// Test refcount tracking
EpochId e = epoch_current(alloc);
slab_epoch_inc_refcount(alloc, e);
assert(slab_epoch_get_refcount(alloc, e) == 1);
slab_epoch_dec_refcount(alloc, e);
assert(slab_epoch_get_refcount(alloc, e) == 0);

// Test label with special chars
slab_epoch_set_label(alloc, e, "request:\"abc\"");  // Contains quotes
```

### 2. JSON Output Validation

```bash
./stats_dump --no-text | jq '.epochs[] | select(.refcount > 0)'
./stats_dump --no-text | jq '.epochs[] | select(.age_sec > 10)'
```

### 3. CLI Verification

```bash
./tslab top --epochs --by refcount --n 5
./tslab top --epochs --by age_sec
./tslab top --epochs --by rss_bytes
```

### 4. Prometheus Export

```bash
./tslab export prometheus | grep epoch_refcount
./tslab export prometheus | grep epoch_info.*label
```

### 5. Schema Validation

```bash
# Verify old stats_dump (no epochs) still validates
./old_stats_dump --no-text | ./validate_schema schemas/stats_v1.schema.json

# Verify new stats_dump (with epochs) validates
./new_stats_dump --no-text | ./validate_schema schemas/stats_v1.schema.json
```

---

## Summary of Critical Fixes

| Issue | Original Plan | Corrected Plan | Why It Matters |
|-------|---------------|----------------|----------------|
| **Label lock** | `static pthread_mutex_t` (global) | `SlabAllocator->epoch_label_lock` | Prevents cross-allocator contention |
| **Field name** | `alloc_count` | `refcount` | Semantic clarity (domain tracking, not allocation count) |
| **Clock** | `CLOCK_REALTIME` | `CLOCK_MONOTONIC` | Age stable across NTP adjustments |
| **JSON escaping** | `printf("\"%s\"", label)` | `print_json_string(label)` | Prevents invalid JSON from quotes/backslashes |
| **Schema version** | Make epochs required | Make epochs optional | Backward compatibility with v1 tools |
| **PromQL** | `{age_seconds > 300}` | `(age_seconds > 300) and on (...)` | Label matchers can't do numeric comparisons |

---

## Implementation Order (Regression-Free)

**Day 1 (Core allocator, no external changes):**
1. Fix `EpochMetadata` struct (rename alloc_count → refcount)
2. Add `epoch_label_lock` to `SlabAllocator`
3. Initialize lock in create/destroy
4. Fix `epoch_advance()` to use `CLOCK_MONOTONIC`
5. Implement 4 public APIs
6. **Test:** Compile, run unit tests (no JSON changes yet)

**Day 2 (JSON output + tools):**
7. Add `print_json_string()` escaper
8. Add `epochs[]` array to JSON output
9. **Test:** Verify JSON is valid (`jq` parse succeeds)
10. Update schema (epochs optional)
11. Update Go structs
12. Implement `tslab top --epochs`
13. **Test:** CLI works with and without epochs array

**Day 3 (Prometheus + Grafana):**
14. Add Prometheus epoch metrics
15. Add Grafana epoch panels (corrected PromQL)
16. Update VERIFICATION.sh
17. **Test:** Full verification suite passes

---

## Success Criteria

**Phase 2.3 complete when:**

✅ APIs compile and pass unit tests  
✅ JSON output is valid (escaping works)  
✅ Schema validates both old and new output  
✅ `tslab top --epochs` works (4 sort modes)  
✅ Prometheus exports epoch metrics  
✅ Grafana dashboard shows epoch panels  
✅ PromQL queries are syntactically correct  
✅ Full verification suite passes (8+ tests)  

**Killer feature unlocked:**
```promql
(temporal_slab_epoch_age_seconds > 300) 
and on (epoch, era) 
(temporal_slab_epoch_refcount{state="CLOSING"} > 0)
```
**Alert:** "Epoch in CLOSING state for >5 minutes with non-zero refcount (domain leak detected)."

---

**Maintainer:** blackwd  
**Supersedes:** PHASE_2_3_IMPLEMENTATION.md (this version incorporates 6 critical fixes)
