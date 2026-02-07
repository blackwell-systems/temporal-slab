# Phase 2.3 Implementation Plan: Semantic Attribution

**Goal:** Add epoch labels and refcount tracking to enable application bug detection.

**Effort:** 4-6 hours  
**Files to modify:** 6 files  
**Testing:** Extend existing verification  

---

## Current State

**Already implemented:**
- ✅ `EpochMetadata epoch_meta[EPOCH_COUNT]` in `SlabAllocator`
- ✅ `EpochMetadata` struct with `open_since_ns`, `alloc_count`, `label[32]`
- ✅ `uint64_t epoch_era[EPOCH_COUNT]` for monotonic tracking

**Missing:**
- ❌ Public APIs (`slab_epoch_set_label`, `inc_refcount`, `dec_refcount`)
- ❌ JSON output for `epochs[]` array
- ❌ `tslab top --epochs` command
- ❌ Prometheus epoch metrics
- ❌ Grafana epoch panels

---

## Implementation Checklist

### 1. Add Public APIs (include/slab_alloc.h)

**Add after existing epoch APIs:**

```c
/* ==================== Phase 2.3: Semantic Attribution APIs ==================== */

/* Set semantic label for an epoch
 * 
 * Application calls this when opening a domain to tag the epoch with
 * meaningful context (e.g., "request:abc123", "frame:1234").
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   epoch - Epoch ID to label (typically current_epoch)
 *   label - Null-terminated string (max 31 chars, will be truncated)
 * 
 * THREAD SAFETY: Safe (internal mutex protects label writes)
 * COST: Mutex acquire + strncpy (~50ns)
 * 
 * EXAMPLE:
 *   EpochId e = epoch_current(alloc);
 *   slab_epoch_set_label(alloc, e, "request:abc123");
 */
void slab_epoch_set_label(SlabAllocator* alloc, EpochId epoch, const char* label);

/* Increment epoch refcount (domain enter)
 * 
 * Application calls this when entering a domain that uses this epoch.
 * Refcount tracks how many domains are "holding" the epoch open.
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   epoch - Epoch ID
 * 
 * THREAD SAFETY: Safe (atomic increment)
 * COST: Atomic fetch_add with relaxed ordering (~2 cycles)
 * 
 * EXAMPLE:
 *   EpochId e = epoch_current(alloc);
 *   slab_epoch_inc_refcount(alloc, e);  // Domain entered
 *   // ... allocate objects ...
 *   slab_epoch_dec_refcount(alloc, e);  // Domain exited
 */
void slab_epoch_inc_refcount(SlabAllocator* alloc, EpochId epoch);

/* Decrement epoch refcount (domain exit)
 * 
 * Application calls this when exiting a domain.
 * When refcount reaches zero, epoch is eligible for closing.
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   epoch - Epoch ID
 * 
 * THREAD SAFETY: Safe (atomic decrement)
 * COST: Atomic fetch_sub with relaxed ordering (~2 cycles)
 * 
 * WARNING: Must be called exactly once per inc_refcount, or refcount leaks.
 */
void slab_epoch_dec_refcount(SlabAllocator* alloc, EpochId epoch);

/* Get epoch refcount (observability only)
 * 
 * Returns current refcount for monitoring/debugging.
 * 
 * PARAMETERS:
 *   alloc - Allocator instance
 *   epoch - Epoch ID
 * 
 * RETURNS: Current refcount value (may be stale due to concurrent modifications)
 * 
 * THREAD SAFETY: Safe (atomic load with relaxed ordering)
 */
uint64_t slab_epoch_get_refcount(SlabAllocator* alloc, EpochId epoch);
```

---

### 2. Implement APIs (src/slab_alloc.c)

**Add after existing epoch functions:**

```c
/* ==================== Phase 2.3: Semantic Attribution APIs ==================== */

void slab_epoch_set_label(SlabAllocator* alloc, EpochId epoch, const char* label) {
  if (epoch >= EPOCH_COUNT) return;
  if (!label) return;
  
  EpochMetadata* meta = &alloc->epoch_meta[epoch];
  
  /* Protect label writes with a mutex (rare operation, coarse lock acceptable) */
  static pthread_mutex_t label_lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&label_lock);
  strncpy(meta->label, label, sizeof(meta->label) - 1);
  meta->label[sizeof(meta->label) - 1] = '\0';  /* Ensure null termination */
  pthread_mutex_unlock(&label_lock);
}

void slab_epoch_inc_refcount(SlabAllocator* alloc, EpochId epoch) {
  if (epoch >= EPOCH_COUNT) return;
  
  EpochMetadata* meta = &alloc->epoch_meta[epoch];
  atomic_fetch_add_explicit(&meta->alloc_count, 1, memory_order_relaxed);
}

void slab_epoch_dec_refcount(SlabAllocator* alloc, EpochId epoch) {
  if (epoch >= EPOCH_COUNT) return;
  
  EpochMetadata* meta = &alloc->epoch_meta[epoch];
  
  /* Prevent underflow */
  uint64_t prev = atomic_load_explicit(&meta->alloc_count, memory_order_relaxed);
  if (prev > 0) {
    atomic_fetch_sub_explicit(&meta->alloc_count, 1, memory_order_relaxed);
  }
}

uint64_t slab_epoch_get_refcount(SlabAllocator* alloc, EpochId epoch) {
  if (epoch >= EPOCH_COUNT) return 0;
  
  EpochMetadata* meta = &alloc->epoch_meta[epoch];
  return atomic_load_explicit(&meta->alloc_count, memory_order_relaxed);
}
```

---

### 3. Initialize Metadata in epoch_advance() (src/slab_alloc.c)

**Find `epoch_advance()` and add initialization:**

```c
void epoch_advance(SlabAllocator* alloc) {
  /* ... existing code ... */
  
  /* Phase 2.2: Increment era counter */
  uint64_t new_era = atomic_fetch_add_explicit(&alloc->epoch_era_counter, 1, memory_order_relaxed) + 1;
  alloc->epoch_era[next_epoch] = new_era;
  
  /* Phase 2.3: Initialize epoch metadata */
  EpochMetadata* meta = &alloc->epoch_meta[next_epoch];
  
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  meta->open_since_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  
  /* Reset refcount and label */
  atomic_store_explicit(&meta->alloc_count, 0, memory_order_relaxed);
  meta->label[0] = '\0';
  
  /* Phase 2.4 fields stay zero until epoch_close */
  meta->rss_before_close = 0;
  meta->rss_after_close = 0;
  
  /* ... rest of existing code ... */
}
```

---

### 4. Add Epochs Array to JSON Output (src/stats_dump.c)

**Modify `print_json_global()` - add before closing brace:**

```c
static void print_json_global(SlabAllocator* alloc) {
  /* ... existing code ... */
  
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
    uint64_t refcount = atomic_load_explicit(&meta->alloc_count, memory_order_relaxed);
    uint64_t era = alloc->epoch_era[epoch_id];
    EpochLifecycleState state = (EpochLifecycleState)atomic_load_explicit(&alloc->epoch_state[epoch_id], memory_order_relaxed);
    
    /* Calculate age */
    uint64_t age_sec = 0;
    if (meta->open_since_ns > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
      if (now_ns > meta->open_since_ns) {
        age_sec = (now_ns - meta->open_since_ns) / 1000000000ULL;
      }
    }
    
    printf("    {\n");
    printf("      \"epoch_id\": %u,\n", epoch_id);
    printf("      \"epoch_era\": %lu,\n", era);
    printf("      \"state\": \"%s\",\n", state == EPOCH_ACTIVE ? "ACTIVE" : "CLOSING");
    printf("      \"age_sec\": %lu,\n", age_sec);
    printf("      \"refcount\": %lu,\n", refcount);
    printf("      \"label\": \"%s\",\n", meta->label);
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

### 5. Update Schema (schemas/stats_v1.schema.json in tools repo)

**Add to `required` array:**
```json
"required": [
  "schema_version",
  ...existing fields...,
  "classes",
  "epochs"
],
```

**Add to `properties` object:**
```json
"epochs": {
  "type": "array",
  "items": {
    "type": "object",
    "required": ["epoch_id", "epoch_era", "state", "age_sec", "refcount", "label", 
                 "total_partial_slabs", "total_full_slabs", "total_reclaimable_slabs", "estimated_rss_bytes"],
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
```

---

### 6. Add Go Struct (temporal-slab-tools/internal/schema/types.go)

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
	Epochs  []EpochStatsV1 `json:"epochs"`  // NEW
}
```

---

### 7. Implement `tslab top --epochs` (temporal-slab-tools/cmd/top.go)

**Modify `cmdTop` to handle epochs:**

```go
if *epochs {
	// NEW: Implement epochs mode
	fmt.Print(view.RenderTopEpochs(&snap, *by, *n, *desc))
	return nil
}
```

**Create `internal/view/epochs.go`:**

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

	// Sort by metric
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
	fmt.Fprintf(&b, "%-8s %-10s %-8s %-10s %-10s %-12s %s\n", 
		"epoch", "era", "state", "age", "refcount", "rss", "label")
	
	for i := 0; i < n; i++ {
		r := rows[i]
		fmt.Fprintf(&b, "%-8d %-10d %-8s %-10ds %-10d %-12s %s\n",
			r.ID, r.Era, r.State, r.AgeSec, r.Refcount,
			fmt.Sprintf("%.2fMB", float64(r.RSSBytes)/(1024*1024)),
			r.Label)
	}

	return b.String()
}
```

**Update `cmd/top.go` flags:**

```go
by := fs.String("by", "rss_bytes", 
	"metric for classes: slow_path_hits | new_slab_count | madvise_bytes | net_slabs\n"+
	"       metric for epochs:  rss_bytes | age_sec | refcount | reclaimable_bytes")
```

---

### 8. Add Prometheus Epoch Metrics (temporal-slab-tools/cmd/export_prometheus.go)

**Add to `buildProm()` function:**

```go
func buildProm(prefix string, s *StatsDumpV1) ([]promDef, []promSample) {
	// ... existing defs and samples ...
	
	// Epoch metrics (use info metric for labels to avoid cardinality explosion)
	defs = append(defs,
		promDef{name: fmt.Sprintf("%s_epoch_info", prefix), help: "Epoch metadata (epoch id, era, state, label)", typ: promGauge},
		promDef{name: fmt.Sprintf("%s_epoch_age_seconds", prefix), help: "Epoch age in seconds", typ: promGauge},
		promDef{name: fmt.Sprintf("%s_epoch_refcount", prefix), help: "Epoch refcount (live domains)", typ: promGauge},
		promDef{name: fmt.Sprintf("%s_epoch_estimated_rss_bytes", prefix), help: "Epoch estimated RSS", typ: promGauge},
		promDef{name: fmt.Sprintf("%s_epoch_reclaimable_bytes", prefix), help: "Epoch reclaimable bytes", typ: promGauge},
	)
	
	for _, e := range s.Epochs {
		lbl := map[string]string{
			"epoch": fmt.Sprintf("%d", e.EpochID),
			"era":   fmt.Sprintf("%d", e.EpochEra),
			"state": e.State,
		}
		
		// Info metric with label
		infoLbl := map[string]string{
			"epoch": fmt.Sprintf("%d", e.EpochID),
			"era":   fmt.Sprintf("%d", e.EpochEra),
			"state": e.State,
			"label": e.Label,
		}
		samples = append(samples,
			promSample{name: fmt.Sprintf("%s_epoch_info", prefix), labels: infoLbl, value: "1"},
		)
		
		// Numeric metrics
		samples = append(samples,
			promSample{name: fmt.Sprintf("%s_epoch_age_seconds", prefix), labels: lbl, value: fmt.Sprintf("%d", e.AgeSec)},
			promSample{name: fmt.Sprintf("%s_epoch_refcount", prefix), labels: lbl, value: fmt.Sprintf("%d", e.Refcount)},
			promSample{name: fmt.Sprintf("%s_epoch_estimated_rss_bytes", prefix), labels: lbl, value: fmt.Sprintf("%d", e.EstimatedRSSBytes)},
			promSample{name: fmt.Sprintf("%s_epoch_reclaimable_bytes", prefix), labels: lbl, value: fmt.Sprintf("%d", e.TotalReclaimableSlabs*4096)},
		)
	}
	
	return defs, samples
}
```

---

### 9. Add Grafana Epoch Panels (temporal-slab-tools/dashboards/temporal-slab-observability.json)

**Add new row after "Per-class hotspots":**

```json
{
  "collapsed": false,
  "gridPos": { "h": 1, "w": 24, "x": 0, "y": 50 },
  "id": 15,
  "panels": [],
  "title": "Epoch observability (Phase 2.3)",
  "type": "row"
},
{
  "datasource": { "type": "prometheus", "uid": "${datasource}" },
  "description": "Top epochs by estimated RSS",
  "fieldConfig": { "defaults": { "unit": "bytes" }, "overrides": [] },
  "gridPos": { "h": 10, "w": 12, "x": 0, "y": 51 },
  "id": 16,
  "options": {
    "showHeader": true,
    "sortBy": [{ "desc": true, "displayName": "rss" }]
  },
  "targets": [
    {
      "expr": "temporal_slab_epoch_estimated_rss_bytes",
      "format": "table",
      "instant": true,
      "refId": "A"
    },
    {
      "expr": "temporal_slab_epoch_age_seconds",
      "format": "table",
      "instant": true,
      "refId": "B"
    },
    {
      "expr": "temporal_slab_epoch_refcount",
      "format": "table",
      "instant": true,
      "refId": "C"
    }
  ],
  "title": "Top epochs by RSS",
  "transformations": [
    {
      "id": "joinByField",
      "options": { "byField": "epoch", "mode": "outer" }
    },
    {
      "id": "organize",
      "options": {
        "excludeByName": { "Time": true, "__name__": true },
        "renameByName": {
          "Value #A": "rss",
          "Value #B": "age_sec",
          "Value #C": "refcount"
        }
      }
    }
  ],
  "type": "table"
},
{
  "datasource": { "type": "prometheus", "uid": "${datasource}" },
  "description": "Epochs with non-zero refcount older than 5 minutes (potential leaks)",
  "fieldConfig": { "defaults": { "unit": "none" }, "overrides": [] },
  "gridPos": { "h": 10, "w": 12, "x": 12, "y": 51 },
  "id": 17,
  "options": {
    "showHeader": true,
    "sortBy": [{ "desc": true, "displayName": "age_sec" }]
  },
  "targets": [
    {
      "expr": "temporal_slab_epoch_refcount{age_seconds > 300} > 0",
      "format": "table",
      "instant": true,
      "legendFormat": "epoch {{epoch}} ({{label}})",
      "refId": "A"
    }
  ],
  "title": "Refcount leaks (age > 5min, refcount > 0)",
  "type": "table"
}
```

---

## Testing Plan

### 1. Unit Test (test_semantic_attribution.c)

```c
void test_semantic_attribution() {
  SlabAllocator* alloc = slab_allocator_create();
  
  EpochId e = epoch_current(alloc);
  
  // Set label
  slab_epoch_set_label(alloc, e, "test-request-123");
  
  // Inc refcount
  slab_epoch_inc_refcount(alloc, e);
  assert(slab_epoch_get_refcount(alloc, e) == 1);
  
  // Dec refcount
  slab_epoch_dec_refcount(alloc, e);
  assert(slab_epoch_get_refcount(alloc, e) == 0);
  
  slab_allocator_free(alloc);
}
```

### 2. JSON Output Test

```bash
./stats_dump --no-text | jq '.epochs[] | select(.epoch_id == 5)'
```

**Expected:**
```json
{
  "epoch_id": 5,
  "epoch_era": 150,
  "state": "ACTIVE",
  "age_sec": 10,
  "refcount": 2,
  "label": "test-request-123",
  ...
}
```

### 3. CLI Test

```bash
./tslab top --epochs --by refcount --n 5
```

**Expected:**
```
top epochs by refcount
epoch    era        state    age        refcount   rss          label
5        150        ACTIVE   10s        2          4.00MB       test-request-123
```

### 4. Prometheus Test

```bash
./tslab export prometheus | grep epoch_refcount
```

**Expected:**
```
temporal_slab_epoch_refcount{epoch="5",era="150",state="ACTIVE"} 2
```

---

## Estimated Time Breakdown

1. Add APIs (header + impl): **1 hour**
2. Initialize metadata in epoch_advance(): **30 min**
3. Add epochs[] to JSON output: **1 hour**
4. Update schema + Go structs: **30 min**
5. Implement `tslab top --epochs`: **1 hour**
6. Add Prometheus epoch metrics: **45 min**
7. Add Grafana epoch panels: **45 min**
8. Testing + verification: **1 hour**

**Total: ~6.5 hours**

---

## Success Criteria

**Phase 2.3 is complete when:**

✅ APIs exist: `slab_epoch_set_label()`, `inc_refcount()`, `dec_refcount()`  
✅ JSON output includes `epochs[]` array  
✅ `tslab top --epochs` works (by rss_bytes, age_sec, refcount, reclaimable_bytes)  
✅ Prometheus exports epoch metrics with `label` in info metric  
✅ Grafana dashboard shows "Top epochs by RSS" and "Refcount leaks" panels  
✅ Alert rule: `temporal_slab_epoch_refcount{age_seconds > 300} > 0`  

**Killer feature unlocked:**
> "Epoch 12 (background-worker-3) has refcount=2 for 10 minutes. Application bug: domain_enter() called twice, domain_exit() never called."

---

## Next Steps After 2.3

**Phase 2.1:** Add `epoch_close_scanned_slabs`, `epoch_close_recycled_slabs` (2-3 hours)  
**Phase 2.4:** Add `rss_before_close`, `rss_after_close` (2-3 hours)  

**Full semantic observability:** ~10 hours total from current state.

---

**Maintainer:** blackwd  
**Related docs:**
- `OBSERVABILITY_STATUS.md` - Current state and roadmap
- `OBSERVABILITY_DESIGN.md` - Complete phase breakdown
