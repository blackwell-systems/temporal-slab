/*
 * synthetic_bench.c - Canonical benchmark harness for temporal-slab allocator
 *
 * Parameterized workload generator with 6 built-in patterns designed to
 * demonstrate allocator behavior and Phase 2.0-2.5 observability features.
 *
 * Features (updated Feb 10 2026):
 * - Phase 2.2 contention metrics (lock_contention_pct, cas_retry_rate, scan_mode)
 * - Phase 2.4 RSS delta tracking (rss_delta_mb computed from before/after)
 * - Phase 2.5 slowpath sampling (optional ENABLE_SLOWPATH_SAMPLING, wait_ns metric)
 * - New PATTERN_CONTENTION (8-thread stress test for adaptive scanning validation)
 *
 * Compile (basic):
 *   cd /home/blackwd/code/ZNS-Slab/src
 *   make slab_lib.o
 *   gcc -O3 -std=c11 -pthread -Wall -Wextra -pedantic -I../include \
 *       -DENABLE_RSS_RECLAMATION=1 ../workloads/synthetic_bench.c slab_lib.o \
 *       -o ../workloads/synthetic_bench
 *
 * Compile (with slowpath sampling):
 *   gcc -O3 -std=c11 -pthread -Wall -Wextra -pedantic -I../include \
 *       -DENABLE_RSS_RECLAMATION=1 -DENABLE_SLOWPATH_SAMPLING=1 \
 *       ../workloads/synthetic_bench.c slab_lib.o -o ../workloads/synthetic_bench
 *
 * Usage:
 *   ./synthetic_bench --allocator=tslab --pattern=burst --duration_s=60
 *   ./synthetic_bench --allocator=malloc --pattern=steady --duration_s=60
 *   ./synthetic_bench --allocator=tslab --pattern=contention --threads=8 --duration_s=30
 */

#define _GNU_SOURCE
#include <slab_alloc.h>
#include <slab_stats.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <errno.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef enum {
    PATTERN_BURST,
    PATTERN_STEADY,
    PATTERN_LEAK,
    PATTERN_HOTSPOT,
    PATTERN_KERNEL,
    PATTERN_CONTENTION,
} WorkloadPattern;

typedef enum {
    ALLOCATOR_TSLAB,
    ALLOCATOR_MALLOC,
} AllocatorBackend;

typedef enum {
    EPOCH_POLICY_PER_REQ,   /* New epoch per request, close on complete */
    EPOCH_POLICY_BATCH,     /* Same epoch for N requests */
    EPOCH_POLICY_MANUAL,    /* Never advance automatically */
} EpochPolicy;

typedef enum {
    FREE_POLICY_WITHIN_REQ, /* Free all objects before request completes */
    FREE_POLICY_LAG,        /* Free N requests later */
    FREE_POLICY_LEAK,       /* Leak a percentage of objects */
} FreePolicy;

typedef struct {
    AllocatorBackend allocator;
    WorkloadPattern pattern;
    uint32_t duration_s;
    uint32_t threads;
    uint32_t req_rate;         /* Requests per second per thread */
    uint32_t objs_min;
    uint32_t objs_max;
    size_t size;               /* Object size (single-class for MVP) */
    EpochPolicy epoch_policy;
    uint32_t batch_size;       /* For EPOCH_POLICY_BATCH */
    FreePolicy free_policy;
    uint32_t lag_window;       /* For FREE_POLICY_LAG */
    float leak_pct;            /* For FREE_POLICY_LEAK (0.0-1.0) */
    uint32_t rss_sample_ms;    /* RSS sampling interval (0 = disabled) */
} BenchConfig;

/* ============================================================================
 * Allocator Backend Abstraction
 * ============================================================================ */

typedef struct {
    void* ctx;
    void* (*alloc_fn)(void* ctx, size_t size, uint32_t epoch, SlabHandle* out_handle);
    void (*free_fn)(void* ctx, void* ptr, SlabHandle handle);
    void (*close_epoch_fn)(void* ctx, uint32_t epoch);
    uint32_t (*advance_epoch_fn)(void* ctx);
    uint32_t (*current_epoch_fn)(void* ctx);
} Backend;

/* Temporal-slab backend */
static void* tslab_alloc(void* ctx, size_t size, uint32_t epoch, SlabHandle* out_handle) {
    SlabAllocator* a = (SlabAllocator*)ctx;
    return alloc_obj_epoch(a, size, epoch, out_handle);
}

static void tslab_free(void* ctx, void* ptr, SlabHandle handle) {
    SlabAllocator* a = (SlabAllocator*)ctx;
    (void)ptr;  /* Unused for handle-based free */
    free_obj(a, handle);
}

static void tslab_close_epoch(void* ctx, uint32_t epoch) {
    SlabAllocator* a = (SlabAllocator*)ctx;
    epoch_close(a, epoch);
}

static uint32_t tslab_advance_epoch(void* ctx) {
    SlabAllocator* a = (SlabAllocator*)ctx;
    epoch_advance(a);
    return epoch_current(a);
}

static uint32_t tslab_current_epoch(void* ctx) {
    SlabAllocator* a = (SlabAllocator*)ctx;
    return epoch_current(a);
}

/* malloc backend (baseline comparison) */
static void* malloc_alloc(void* ctx, size_t size, uint32_t epoch, SlabHandle* out_handle) {
    (void)ctx; (void)epoch; (void)out_handle;
    return malloc(size);
}

static void malloc_free(void* ctx, void* ptr, SlabHandle handle) {
    (void)ctx; (void)handle;
    free(ptr);
}

static void malloc_close_epoch(void* ctx, uint32_t epoch) {
    (void)ctx; (void)epoch;
    /* malloc has no epoch concept - no-op */
}

static uint32_t malloc_advance_epoch(void* ctx) {
    (void)ctx;
    return 0;  /* malloc has no epochs */
}

static uint32_t malloc_current_epoch(void* ctx) {
    (void)ctx;
    return 0;
}

static Backend* backend_create(BenchConfig* cfg) {
    Backend* b = calloc(1, sizeof(Backend));
    if (!b) return NULL;

    if (cfg->allocator == ALLOCATOR_TSLAB) {
        SlabAllocator* a = slab_allocator_create();
        if (!a) {
            free(b);
            return NULL;
        }
        b->ctx = a;
        b->alloc_fn = tslab_alloc;
        b->free_fn = tslab_free;
        b->close_epoch_fn = tslab_close_epoch;
        b->advance_epoch_fn = tslab_advance_epoch;
        b->current_epoch_fn = tslab_current_epoch;
    } else {
        b->ctx = NULL;  /* malloc doesn't need context */
        b->alloc_fn = malloc_alloc;
        b->free_fn = malloc_free;
        b->close_epoch_fn = malloc_close_epoch;
        b->advance_epoch_fn = malloc_advance_epoch;
        b->current_epoch_fn = malloc_current_epoch;
    }

    return b;
}

static void backend_destroy(Backend* b) {
    if (!b) return;
    if (b->ctx) {
        /* Assume temporal-slab if ctx is non-NULL */
        slab_allocator_free((SlabAllocator*)b->ctx);
    }
    free(b);
}

/* ============================================================================
 * Pending Free Buffer (for lag simulation)
 * ============================================================================ */

typedef struct {
    void* ptr;
    SlabHandle handle;
} PendingFree;

typedef struct {
    PendingFree* items;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} FreeBuffer;

static FreeBuffer* free_buffer_create(uint32_t capacity) {
    FreeBuffer* fb = calloc(1, sizeof(FreeBuffer));
    if (!fb) return NULL;
    fb->items = calloc(capacity, sizeof(PendingFree));
    if (!fb->items) {
        free(fb);
        return NULL;
    }
    fb->capacity = capacity;
    return fb;
}

static void free_buffer_destroy(FreeBuffer* fb) {
    if (!fb) return;
    free(fb->items);
    free(fb);
}

static bool free_buffer_enqueue(FreeBuffer* fb, void* ptr, SlabHandle handle) {
    if (fb->count >= fb->capacity) return false;
    fb->items[fb->tail].ptr = ptr;
    fb->items[fb->tail].handle = handle;
    fb->tail = (fb->tail + 1) % fb->capacity;
    fb->count++;
    return true;
}

static bool free_buffer_dequeue(FreeBuffer* fb, void** out_ptr, SlabHandle* out_handle) {
    if (fb->count == 0) return false;
    *out_ptr = fb->items[fb->head].ptr;
    *out_handle = fb->items[fb->head].handle;
    fb->head = (fb->head + 1) % fb->capacity;
    fb->count--;
    return true;
}

/* ============================================================================
 * Request Simulation
 * ============================================================================ */

typedef struct {
    Backend* backend;
    BenchConfig* config;
    FreeBuffer* free_buffer;
    uint64_t requests_completed;
    uint64_t objects_allocated;
    uint64_t objects_freed;
    uint64_t objects_leaked;
    uint32_t current_epoch;
    uint32_t reqs_in_current_epoch;
    volatile bool stop;
} WorkerState;

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void simulate_request(WorkerState* ws) {
    BenchConfig* cfg = ws->config;
    Backend* b = ws->backend;

    /* Determine object count for this request */
    uint32_t obj_count;
    if (cfg->objs_min == cfg->objs_max) {
        obj_count = cfg->objs_min;
    } else {
        obj_count = cfg->objs_min + (rand() % (cfg->objs_max - cfg->objs_min + 1));
    }

    /* Allocate objects */
    void** ptrs = alloca(obj_count * sizeof(void*));
    SlabHandle* handles = alloca(obj_count * sizeof(SlabHandle));

    for (uint32_t i = 0; i < obj_count; i++) {
        ptrs[i] = b->alloc_fn(b->ctx, cfg->size, ws->current_epoch, &handles[i]);
        if (!ptrs[i]) {
            fprintf(stderr, "Allocation failed at request %lu, object %u\n",
                    ws->requests_completed, i);
            ws->stop = true;
            return;
        }
        /* Touch memory to force RSS */
        memset(ptrs[i], 0x42, cfg->size);
        ws->objects_allocated++;
    }

    /* Free policy: immediate, lag, or leak */
    for (uint32_t i = 0; i < obj_count; i++) {
        bool should_leak = false;
        if (cfg->free_policy == FREE_POLICY_LEAK && cfg->leak_pct > 0.0f) {
            float r = (float)rand() / (float)RAND_MAX;
            should_leak = (r < cfg->leak_pct);
        }

        if (should_leak) {
            ws->objects_leaked++;
        } else if (cfg->free_policy == FREE_POLICY_LAG) {
            if (!free_buffer_enqueue(ws->free_buffer, ptrs[i], handles[i])) {
                fprintf(stderr, "Free buffer full - increasing lag window\n");
                b->free_fn(b->ctx, ptrs[i], handles[i]);
                ws->objects_freed++;
            }
        } else {
            /* FREE_POLICY_WITHIN_REQ or FREE_POLICY_LEAK (non-leaked objects) */
            b->free_fn(b->ctx, ptrs[i], handles[i]);
            ws->objects_freed++;
        }
    }

    /* Drain lag buffer if applicable */
    if (cfg->free_policy == FREE_POLICY_LAG) {
        /* Drain one request worth of frees */
        for (uint32_t i = 0; i < obj_count; i++) {
            void* ptr;
            SlabHandle handle;
            if (free_buffer_dequeue(ws->free_buffer, &ptr, &handle)) {
                b->free_fn(b->ctx, ptr, handle);
                ws->objects_freed++;
            } else {
                break;  /* Buffer empty */
            }
        }
    }

    ws->requests_completed++;
    ws->reqs_in_current_epoch++;

    /* Epoch policy: per-request, batch, or manual */
    if (cfg->epoch_policy == EPOCH_POLICY_PER_REQ) {
        b->close_epoch_fn(b->ctx, ws->current_epoch);
        ws->current_epoch = b->advance_epoch_fn(b->ctx);
        ws->reqs_in_current_epoch = 0;
    } else if (cfg->epoch_policy == EPOCH_POLICY_BATCH) {
        if (ws->reqs_in_current_epoch >= cfg->batch_size) {
            b->close_epoch_fn(b->ctx, ws->current_epoch);
            ws->current_epoch = b->advance_epoch_fn(b->ctx);
            ws->reqs_in_current_epoch = 0;
        }
    }
    /* EPOCH_POLICY_MANUAL: never advance automatically */
}

static void* worker_thread(void* arg) {
    WorkerState* ws = (WorkerState*)arg;
    BenchConfig* cfg = ws->config;

    uint64_t ns_per_req = 1000000000ULL / cfg->req_rate;
    uint64_t next_req_time = get_time_ns();

    while (!ws->stop) {
        uint64_t now = get_time_ns();
        if (now >= next_req_time) {
            simulate_request(ws);
            next_req_time += ns_per_req;
        } else {
            /* Sleep until next request */
            uint64_t sleep_ns = next_req_time - now;
            if (sleep_ns > 1000000) {  /* > 1ms */
                struct timespec ts = {
                    .tv_sec = 0,
                    .tv_nsec = sleep_ns
                };
                nanosleep(&ts, NULL);
            }
        }
    }

    return NULL;
}

/* ============================================================================
 * Pattern Presets
 * ============================================================================ */

static void apply_pattern_preset(BenchConfig* cfg, WorkloadPattern pattern) {
    cfg->pattern = pattern;

    switch (pattern) {
    case PATTERN_BURST:
        /* Epoch-bound request bursts: RSS sawtooth + madvise spikes */
        cfg->req_rate = 2000;
        cfg->objs_min = 80;
        cfg->objs_max = 200;
        cfg->size = 128;
        cfg->free_policy = FREE_POLICY_WITHIN_REQ;
        cfg->epoch_policy = EPOCH_POLICY_PER_REQ;
        break;

    case PATTERN_STEADY:
        /* Steady-state churn: RSS plateau */
        cfg->req_rate = 5000;
        cfg->objs_min = 50;
        cfg->objs_max = 50;
        cfg->size = 128;
        cfg->free_policy = FREE_POLICY_LAG;
        cfg->lag_window = 8;
        cfg->epoch_policy = EPOCH_POLICY_BATCH;
        cfg->batch_size = 16;
        break;

    case PATTERN_LEAK:
        /* Pathological lifetime mismatch: epoch age/refcount anomalies */
        cfg->req_rate = 2000;
        cfg->objs_min = 80;
        cfg->objs_max = 200;
        cfg->size = 128;
        cfg->free_policy = FREE_POLICY_LEAK;
        cfg->leak_pct = 0.01f;  /* 1% leak */
        cfg->epoch_policy = EPOCH_POLICY_PER_REQ;
        break;

    case PATTERN_HOTSPOT:
        /* Mixed size-class hotspots (future: multi-size support) */
        cfg->req_rate = 4000;
        cfg->objs_min = 120;
        cfg->objs_max = 120;
        cfg->size = 128;  /* MVP: single size */
        cfg->free_policy = FREE_POLICY_WITHIN_REQ;
        cfg->epoch_policy = EPOCH_POLICY_PER_REQ;
        break;

    case PATTERN_KERNEL:
        /* Kernel interaction demonstration: strong madvise→RSS correlation */
        cfg->req_rate = 2000;
        cfg->objs_min = 300;
        cfg->objs_max = 300;
        cfg->size = 256;
        cfg->free_policy = FREE_POLICY_WITHIN_REQ;
        cfg->epoch_policy = EPOCH_POLICY_PER_REQ;
        break;
    
    case PATTERN_CONTENTION:
        /* Multi-threaded stress: demonstrate adaptive scanning + Phase 2.2 metrics */
        cfg->threads = 8;
        cfg->req_rate = 10000;
        cfg->objs_min = 50;
        cfg->objs_max = 50;
        cfg->size = 128;
        cfg->free_policy = FREE_POLICY_WITHIN_REQ;
        cfg->epoch_policy = EPOCH_POLICY_BATCH;
        cfg->batch_size = 100;
        break;
    }
}

/* ============================================================================
 * Command-Line Parsing
 * ============================================================================ */

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  --allocator=<tslab|malloc>     Backend allocator (default: tslab)\n");
    printf("  --pattern=<burst|steady|leak|hotspot|kernel|contention>\n");
    printf("                                 Workload pattern (default: burst)\n");
    printf("  --duration_s=N                 Run duration in seconds (default: 60)\n");
    printf("  --threads=N                    Number of worker threads (default: 1)\n");
    printf("  --req_rate=N                   Requests/sec per thread (default: 2000)\n");
    printf("  --objs_min=N                   Min objects per request (default: 80)\n");
    printf("  --objs_max=N                   Max objects per request (default: 200)\n");
    printf("  --size=N                       Object size in bytes (default: 128)\n");
    printf("  --epoch_policy=<per_req|batch:N|manual>\n");
    printf("                                 Epoch management policy (default: per_req)\n");
    printf("  --free_policy=<within_req|lag:N|leak:pct>\n");
    printf("                                 Free timing policy (default: within_req)\n");
    printf("  --rss_sample_ms=N              RSS sampling interval (0=disabled)\n");
    printf("  --help                         Show this help\n\n");
    printf("Pattern presets:\n");
    printf("  burst:   RSS sawtooth, madvise spikes\n");
    printf("  steady:  RSS plateau, stable cache reuse\n");
    printf("  leak:       Epoch age/refcount anomalies\n");
    printf("  contention: Multi-threaded stress (8T, adaptive scanning)\n");
    printf("  hotspot: Per-class hotspots\n");
    printf("  kernel:  Strong madvise→RSS correlation\n");
}

static bool parse_args(int argc, char** argv, BenchConfig* cfg) {
    /* Defaults */
    memset(cfg, 0, sizeof(BenchConfig));
    cfg->allocator = ALLOCATOR_TSLAB;
    cfg->pattern = PATTERN_BURST;
    cfg->duration_s = 60;
    cfg->threads = 1;
    cfg->rss_sample_ms = 0;

    /* Apply default pattern preset */
    apply_pattern_preset(cfg, PATTERN_BURST);

    static struct option long_opts[] = {
        {"allocator", required_argument, 0, 'a'},
        {"pattern", required_argument, 0, 'p'},
        {"duration_s", required_argument, 0, 'd'},
        {"threads", required_argument, 0, 't'},
        {"req_rate", required_argument, 0, 'r'},
        {"objs_min", required_argument, 0, 'm'},
        {"objs_max", required_argument, 0, 'M'},
        {"size", required_argument, 0, 's'},
        {"epoch_policy", required_argument, 0, 'e'},
        {"free_policy", required_argument, 0, 'f'},
        {"rss_sample_ms", required_argument, 0, 'R'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'a':
            if (strcmp(optarg, "tslab") == 0) {
                cfg->allocator = ALLOCATOR_TSLAB;
            } else if (strcmp(optarg, "malloc") == 0) {
                cfg->allocator = ALLOCATOR_MALLOC;
            } else {
                fprintf(stderr, "Unknown allocator: %s\n", optarg);
                return false;
            }
            break;
        case 'p':
            if (strcmp(optarg, "burst") == 0) {
                apply_pattern_preset(cfg, PATTERN_BURST);
            } else if (strcmp(optarg, "steady") == 0) {
                apply_pattern_preset(cfg, PATTERN_STEADY);
            } else if (strcmp(optarg, "leak") == 0) {
                apply_pattern_preset(cfg, PATTERN_LEAK);
            } else if (strcmp(optarg, "hotspot") == 0) {
                apply_pattern_preset(cfg, PATTERN_HOTSPOT);
            } else if (strcmp(optarg, "kernel") == 0) {
                apply_pattern_preset(cfg, PATTERN_KERNEL);
            } else if (strcmp(optarg, "contention") == 0) {
                apply_pattern_preset(cfg, PATTERN_CONTENTION);
            } else {
                fprintf(stderr, "Unknown pattern: %s\n", optarg);
                return false;
            }
            break;
        case 'd':
            cfg->duration_s = atoi(optarg);
            break;
        case 't':
            cfg->threads = atoi(optarg);
            break;
        case 'r':
            cfg->req_rate = atoi(optarg);
            break;
        case 'm':
            cfg->objs_min = atoi(optarg);
            break;
        case 'M':
            cfg->objs_max = atoi(optarg);
            break;
        case 's':
            cfg->size = atoi(optarg);
            break;
        case 'e':
            if (strcmp(optarg, "per_req") == 0) {
                cfg->epoch_policy = EPOCH_POLICY_PER_REQ;
            } else if (strncmp(optarg, "batch:", 6) == 0) {
                cfg->epoch_policy = EPOCH_POLICY_BATCH;
                cfg->batch_size = atoi(optarg + 6);
            } else if (strcmp(optarg, "manual") == 0) {
                cfg->epoch_policy = EPOCH_POLICY_MANUAL;
            } else {
                fprintf(stderr, "Unknown epoch_policy: %s\n", optarg);
                return false;
            }
            break;
        case 'f':
            if (strcmp(optarg, "within_req") == 0) {
                cfg->free_policy = FREE_POLICY_WITHIN_REQ;
            } else if (strncmp(optarg, "lag:", 4) == 0) {
                cfg->free_policy = FREE_POLICY_LAG;
                cfg->lag_window = atoi(optarg + 4);
            } else if (strncmp(optarg, "leak:", 5) == 0) {
                cfg->free_policy = FREE_POLICY_LEAK;
                cfg->leak_pct = atof(optarg + 5) / 100.0f;
            } else {
                fprintf(stderr, "Unknown free_policy: %s\n", optarg);
                return false;
            }
            break;
        case 'R':
            cfg->rss_sample_ms = atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            return false;
        }
    }

    return true;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char** argv) {
    BenchConfig cfg;
    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    /* Print configuration */
    printf("Synthetic Benchmark Configuration\n");
    printf("=================================\n");
    printf("Allocator:      %s\n", cfg.allocator == ALLOCATOR_TSLAB ? "tslab" : "malloc");
    printf("Pattern:        ");
    switch (cfg.pattern) {
    case PATTERN_BURST:   printf("burst\n"); break;
    case PATTERN_STEADY:  printf("steady\n"); break;
    case PATTERN_LEAK:    printf("leak\n"); break;
    case PATTERN_HOTSPOT: printf("hotspot\n"); break;
    case PATTERN_KERNEL:  printf("kernel\n"); break;
    }
    printf("Duration:       %u seconds\n", cfg.duration_s);
    printf("Threads:        %u\n", cfg.threads);
    printf("Req rate:       %u req/s per thread\n", cfg.req_rate);
    printf("Objects/req:    %u-%u\n", cfg.objs_min, cfg.objs_max);
    printf("Object size:    %zu bytes\n", cfg.size);
    printf("\n");

    /* Create backend */
    Backend* backend = backend_create(&cfg);
    if (!backend) {
        fprintf(stderr, "Failed to create backend\n");
        return 1;
    }

    /* Create worker state (single-threaded MVP) */
    WorkerState ws = {
        .backend = backend,
        .config = &cfg,
        .free_buffer = NULL,
        .requests_completed = 0,
        .objects_allocated = 0,
        .objects_freed = 0,
        .objects_leaked = 0,
        .current_epoch = backend->current_epoch_fn(backend->ctx),
        .reqs_in_current_epoch = 0,
        .stop = false,
    };

    if (cfg.free_policy == FREE_POLICY_LAG) {
        uint32_t buffer_capacity = cfg.lag_window * cfg.objs_max * 2;
        ws.free_buffer = free_buffer_create(buffer_capacity);
        if (!ws.free_buffer) {
            fprintf(stderr, "Failed to create free buffer\n");
            backend_destroy(backend);
            return 1;
        }
    }

    /* Start benchmark */
    printf("Starting benchmark...\n");
    uint64_t start_time = get_time_ns();
    uint64_t end_time = start_time + (cfg.duration_s * 1000000000ULL);

    pthread_t thread;
    pthread_create(&thread, NULL, worker_thread, &ws);

    /* Main thread: monitor, export stats, and stop after duration */
    uint64_t last_progress = start_time;
    uint64_t last_stats_export = start_time;
    
    while (get_time_ns() < end_time) {
        sleep(1);
        
        uint64_t now = get_time_ns();
        
        /* Export stats every 5 seconds (for live dashboard) */
        if (cfg.allocator == ALLOCATOR_TSLAB && (now - last_stats_export) >= 5000000000ULL) {
            FILE* f = fopen("/tmp/synthetic_bench_stats.json", "w");
            if (f) {
                SlabAllocator* a = (SlabAllocator*)backend->ctx;
                SlabGlobalStats gs;
                slab_stats_global(a, &gs);
                
                fprintf(f, "{\n");
                fprintf(f, "  \"schema_version\": 1,\n");
                fprintf(f, "  \"timestamp_ns\": %lu,\n", now);
                fprintf(f, "  \"pid\": %d,\n", getpid());
                fprintf(f, "  \"page_size\": 4096,\n");
                fprintf(f, "  \"epoch_count\": 16,\n");
                fprintf(f, "  \"version\": %u,\n", gs.version);
                fprintf(f, "  \"current_epoch\": %u,\n", gs.current_epoch);
                fprintf(f, "  \"active_epoch_count\": %u,\n", gs.active_epoch_count);
                fprintf(f, "  \"closing_epoch_count\": %u,\n", gs.closing_epoch_count);
                fprintf(f, "  \"total_slabs_allocated\": %lu,\n", gs.total_slabs_allocated);
                fprintf(f, "  \"total_slabs_recycled\": %lu,\n", gs.total_slabs_recycled);
                fprintf(f, "  \"net_slabs\": %lu,\n", gs.net_slabs);
                fprintf(f, "  \"rss_bytes_current\": %lu,\n", gs.rss_bytes_current);
                fprintf(f, "  \"estimated_slab_rss_bytes\": %lu,\n", gs.estimated_slab_rss_bytes);
                fprintf(f, "  \"total_slow_path_hits\": %lu,\n", gs.total_slow_path_hits);
                fprintf(f, "  \"total_cache_overflows\": %lu,\n", gs.total_cache_overflows);
                fprintf(f, "  \"total_slow_cache_miss\": %lu,\n", gs.total_slow_cache_miss);
                fprintf(f, "  \"total_slow_epoch_closed\": %lu,\n", gs.total_slow_epoch_closed);
                fprintf(f, "  \"total_madvise_calls\": %lu,\n", gs.total_madvise_calls);
                fprintf(f, "  \"total_madvise_bytes\": %lu,\n", gs.total_madvise_bytes);
                fprintf(f, "  \"total_madvise_failures\": %lu,\n", gs.total_madvise_failures);
                
                fprintf(f, "  \"benchmark_requests_completed\": %lu,\n", ws.requests_completed);
                fprintf(f, "  \"benchmark_objects_allocated\": %lu,\n", ws.objects_allocated);
                fprintf(f, "  \"benchmark_objects_freed\": %lu,\n", ws.objects_freed);
                fprintf(f, "  \"benchmark_objects_leaked\": %lu,\n", ws.objects_leaked);
                
                /* Phase 2.5: Slowpath sampling (optional) */
#ifdef ENABLE_SLOWPATH_SAMPLING
                ThreadStats ts = slab_stats_thread();
                if (ts.alloc_samples > 0) {
                    uint64_t avg_wall = ts.alloc_wall_ns_sum / ts.alloc_samples;
                    uint64_t avg_cpu = ts.alloc_cpu_ns_sum / ts.alloc_samples;
                    uint64_t avg_wait = ts.alloc_wait_ns_sum / ts.alloc_samples;
                    fprintf(f, "  \"slowpath_sampling\": {\n");
                    fprintf(f, "    \"enabled\": true,\n");
                    fprintf(f, "    \"samples\": %lu,\n", ts.alloc_samples);
                    fprintf(f, "    \"avg_wall_ns\": %lu,\n", avg_wall);
                    fprintf(f, "    \"avg_cpu_ns\": %lu,\n", avg_cpu);
                    fprintf(f, "    \"avg_wait_ns\": %lu,\n", avg_wait);
                    fprintf(f, "    \"max_wall_ns\": %lu,\n", ts.alloc_wall_ns_max);
                    fprintf(f, "    \"max_cpu_ns\": %lu,\n", ts.alloc_cpu_ns_max);
                    fprintf(f, "    \"max_wait_ns\": %lu,\n", ts.alloc_wait_ns_max);
                    if (ts.repair_count > 0) {
                        uint64_t avg_repair_cpu = ts.repair_cpu_ns_sum / ts.repair_count;
                        uint64_t avg_repair_wait = ts.repair_wait_ns_sum / ts.repair_count;
                        fprintf(f, "    \"repair_count\": %lu,\n", ts.repair_count);
                        fprintf(f, "    \"avg_repair_cpu_ns\": %lu,\n", avg_repair_cpu);
                        fprintf(f, "    \"avg_repair_wait_ns\": %lu,\n", avg_repair_wait);
                        fprintf(f, "    \"repair_reason_full_bitmap\": %lu,\n", ts.repair_reason_full_bitmap);
                        fprintf(f, "    \"repair_reason_list_mismatch\": %lu\n", ts.repair_reason_list_mismatch);
                    } else {
                        fprintf(f, "    \"repair_count\": 0\n");
                    }
                    fprintf(f, "  },\n");
                } else {
                    fprintf(f, "  \"slowpath_sampling\": {\"enabled\": true, \"samples\": 0},\n");
                }
#else
                fprintf(f, "  \"slowpath_sampling\": {\"enabled\": false},\n");
#endif
                
                /* Per-class stats */
                fprintf(f, "  \"classes\": [\n");
                for (uint32_t cls = 0; cls < 8; cls++) {
                    SlabClassStats cs;
                    slab_stats_class(a, cls, &cs);
                    fprintf(f, "    {\n");
                    fprintf(f, "      \"class_index\": %u,\n", cs.class_index);
                    fprintf(f, "      \"object_size\": %u,\n", cs.object_size);
                    fprintf(f, "      \"slow_path_hits\": %lu,\n", cs.slow_path_hits);
                    fprintf(f, "      \"new_slab_count\": %lu,\n", cs.new_slab_count);
                    fprintf(f, "      \"list_move_partial_to_full\": %lu,\n", cs.list_move_partial_to_full);
                    fprintf(f, "      \"list_move_full_to_partial\": %lu,\n", cs.list_move_full_to_partial);
                    fprintf(f, "      \"current_partial_null\": %lu,\n", cs.current_partial_null);
                    fprintf(f, "      \"current_partial_full\": %lu,\n", cs.current_partial_full);
                    fprintf(f, "      \"empty_slab_recycled\": %lu,\n", cs.empty_slab_recycled);
                    fprintf(f, "      \"empty_slab_overflowed\": %lu,\n", cs.empty_slab_overflowed);
                    fprintf(f, "      \"slow_path_cache_miss\": %lu,\n", cs.slow_path_cache_miss);
                    fprintf(f, "      \"slow_path_epoch_closed\": %lu,\n", cs.slow_path_epoch_closed);
                    fprintf(f, "      \"madvise_calls\": %lu,\n", cs.madvise_calls);
                    fprintf(f, "      \"madvise_bytes\": %lu,\n", cs.madvise_bytes);
                    fprintf(f, "      \"madvise_failures\": %lu,\n", cs.madvise_failures);
                    fprintf(f, "      \"epoch_close_calls\": %lu,\n", cs.epoch_close_calls);
                    fprintf(f, "      \"epoch_close_scanned_slabs\": %lu,\n", cs.epoch_close_scanned_slabs);
                    fprintf(f, "      \"epoch_close_recycled_slabs\": %lu,\n", cs.epoch_close_recycled_slabs);
                    fprintf(f, "      \"epoch_close_total_ns\": %lu,\n", cs.epoch_close_total_ns);
                    fprintf(f, "      \"cache_size\": %u,\n", cs.cache_size);
                    fprintf(f, "      \"cache_capacity\": %u,\n", cs.cache_capacity);
                    fprintf(f, "      \"cache_overflow_len\": %u,\n", cs.cache_overflow_len);
                    fprintf(f, "      \"total_partial_slabs\": %u,\n", cs.total_partial_slabs);
                    fprintf(f, "      \"total_full_slabs\": %u,\n", cs.total_full_slabs);
                    fprintf(f, "      \"recycle_rate_pct\": %.2f,\n", cs.recycle_rate_pct);
                    fprintf(f, "      \"net_slabs\": %lu,\n", cs.net_slabs);
                    fprintf(f, "      \"estimated_rss_bytes\": %lu,\n", cs.estimated_rss_bytes);
                    
                    /* Phase 2.2: Contention metrics */
                    uint64_t total_lock_ops = cs.lock_fast_acquire + cs.lock_contended;
                    if (total_lock_ops > 0) {
                        double contention_rate = 100.0 * cs.lock_contended / total_lock_ops;
                        fprintf(f, "      \"lock_contention_pct\": %.2f,\n", contention_rate);
                        fprintf(f, "      \"lock_fast_acquire\": %lu,\n", cs.lock_fast_acquire);
                        fprintf(f, "      \"lock_contended\": %lu,\n", cs.lock_contended);
                    }
                    if (cs.bitmap_alloc_attempts > 0) {
                        double cas_retry_rate = (double)cs.bitmap_alloc_cas_retries / cs.bitmap_alloc_attempts;
                        fprintf(f, "      \"cas_retry_rate\": %.4f,\n", cas_retry_rate);
                        fprintf(f, "      \"bitmap_alloc_cas_retries\": %lu,\n", cs.bitmap_alloc_cas_retries);
                        fprintf(f, "      \"bitmap_alloc_attempts\": %lu,\n", cs.bitmap_alloc_attempts);
                    }
                    fprintf(f, "      \"scan_mode\": %u,\n", cs.scan_mode);
                    fprintf(f, "      \"scan_adapt_checks\": %u,\n", cs.scan_adapt_checks);
                    fprintf(f, "      \"scan_adapt_switches\": %u\n", cs.scan_adapt_switches);
                    fprintf(f, "    }%s\n", (cls < 7) ? "," : "");
                }
                fprintf(f, "  ],\n");
                
                /* Per-epoch stats */
                fprintf(f, "  \"epochs\": [\n");
                int first_epoch = 1;
                for (uint32_t cls = 0; cls < 8; cls++) {
                    for (uint32_t ep = 0; ep < 16; ep++) {
                        SlabEpochStats es;
                        slab_stats_epoch(a, cls, ep, &es);
                        
                        if (!first_epoch) fprintf(f, ",\n");
                        first_epoch = 0;
                        
                        fprintf(f, "    {\n");
                        fprintf(f, "      \"class_index\": %u,\n", es.class_index);
                        fprintf(f, "      \"object_size\": %u,\n", es.object_size);
                        fprintf(f, "      \"epoch_id\": %u,\n", es.epoch_id);
                        fprintf(f, "      \"epoch_era\": %lu,\n", es.epoch_era);
                        fprintf(f, "      \"state\": \"%s\",\n", es.state == 0 ? "ACTIVE" : "CLOSING");
                        fprintf(f, "      \"open_since_ns\": %lu,\n", es.open_since_ns);
                        fprintf(f, "      \"alloc_count\": %lu,\n", es.alloc_count);
                        fprintf(f, "      \"label\": \"%s\",\n", es.label);
                        fprintf(f, "      \"rss_before_close\": %lu,\n", es.rss_before_close);
                        fprintf(f, "      \"rss_after_close\": %lu,\n", es.rss_after_close);
                        /* Phase 2.4: RSS delta analysis */
                        if (es.rss_before_close > 0 && es.rss_after_close > 0) {
                            uint64_t rss_delta_bytes = (es.rss_before_close > es.rss_after_close) ? 
                                (es.rss_before_close - es.rss_after_close) : 0;
                            fprintf(f, "      \"rss_delta_mb\": %.2f,\n", rss_delta_bytes / (1024.0 * 1024.0));
                        }
                        fprintf(f, "      \"partial_slab_count\": %u,\n", es.partial_slab_count);
                        fprintf(f, "      \"full_slab_count\": %u,\n", es.full_slab_count);
                        fprintf(f, "      \"estimated_rss_bytes\": %lu,\n", es.estimated_rss_bytes);
                        fprintf(f, "      \"reclaimable_slab_count\": %u,\n", es.reclaimable_slab_count);
                        fprintf(f, "      \"reclaimable_bytes\": %lu\n", es.reclaimable_bytes);
                        fprintf(f, "    }");
                    }
                }
                fprintf(f, "\n  ]\n");
                fprintf(f, "}\n");
                fclose(f);
            }
            last_stats_export = now;
        }
        
        /* Print progress every 10 seconds */
        if ((now - last_progress) >= 10000000000ULL) {
            double elapsed_s = (now - start_time) / 1e9;
            fprintf(stderr, "[%.0fs] Requests: %lu, Allocs: %lu, Frees: %lu, Leaked: %lu\n",
                    elapsed_s, ws.requests_completed, ws.objects_allocated,
                    ws.objects_freed, ws.objects_leaked);
            last_progress = now;
        }
    }

    ws.stop = true;
    pthread_join(thread, NULL);

    uint64_t elapsed_ns = get_time_ns() - start_time;
    double elapsed_s = elapsed_ns / 1e9;

    /* Print results */
    printf("\n");
    printf("Benchmark Results\n");
    printf("=================\n");
    printf("Elapsed time:        %.2f seconds\n", elapsed_s);
    printf("Requests completed:  %lu\n", ws.requests_completed);
    printf("Objects allocated:   %lu\n", ws.objects_allocated);
    printf("Objects freed:       %lu\n", ws.objects_freed);
    printf("Objects leaked:      %lu\n", ws.objects_leaked);
    printf("Request rate:        %.2f req/s\n", ws.requests_completed / elapsed_s);
    printf("Allocation rate:     %.2f obj/s\n", ws.objects_allocated / elapsed_s);
    printf("\n");

    /* Cleanup */
    if (ws.free_buffer) {
        free_buffer_destroy(ws.free_buffer);
    }
    backend_destroy(backend);

    return 0;
}
