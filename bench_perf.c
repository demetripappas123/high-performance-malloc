#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <malloc.h>
#define HAVE_USABLE_SIZE 1
#endif

#include "allocator.h"

#ifndef STRESS_OPS
#define STRESS_OPS 50000
#endif

#ifndef LONG_OPS
#define LONG_OPS 200000
#endif

#ifndef LEAK_BLOCKS
#define LEAK_BLOCKS 100
#endif

typedef struct {
    void *(*alloc)(size_t);
    void (*dealloc)(void *);
    void *(*resize)(void *, size_t);
    const char *name;
    int is_hp;
} allocator_t;

typedef struct {
    double avg_ns;
    double median_ns;
    double p95_ns;
    double p99_ns;
    size_t samples;
} latency_stats_t;

typedef struct {
    const char *scenario;
    allocator_t alloc;

    latency_stats_t malloc_lat;
    latency_stats_t free_lat;
    latency_stats_t realloc_lat;

    double throughput_ops_per_sec;
    double wall_seconds;

    size_t total_allocs;
    size_t total_frees;
    size_t total_reallocs;
    size_t peak_live_allocs;
    size_t total_bytes_requested;
    size_t total_bytes_allocated;
    size_t largest_request;
    size_t peak_heap_bytes;
    size_t heap_growth_bytes;
    size_t sbrk_calls;

    double metadata_overhead_ratio;
    double internal_frag_ratio;
    double external_frag_ratio;
    double reuse_rate;

    size_t leaked_blocks;
    size_t leaked_bytes;

    int has_heap_metrics;
} bench_result_t;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void summarize_latencies(uint64_t *samples, size_t n, latency_stats_t *out) {
    memset(out, 0, sizeof *out);
    if (n == 0)
        return;

    uint64_t *sorted = malloc(n * sizeof *sorted);
    if (!sorted)
        return;

    memcpy(sorted, samples, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, cmp_u64);

    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += sorted[i];

    out->samples = n;
    out->avg_ns = (double)sum / (double)n;
    out->median_ns = (double)sorted[n / 2];
    out->p95_ns = (double)sorted[(size_t)((n - 1) * 0.95)];
    out->p99_ns = (double)sorted[(size_t)((n - 1) * 0.99)];
    free(sorted);
}

static void record_latency(uint64_t **buf, size_t *count, size_t *cap, uint64_t ns) {
    if (*count >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 1024;
        uint64_t *grown = realloc(*buf, new_cap * sizeof *grown);
        if (!grown)
            return;
        *buf = grown;
        *cap = new_cap;
    }
    (*buf)[(*count)++] = ns;
}

static size_t usable_size(allocator_t a, void *ptr) {
    if (!ptr)
        return 0;
#ifdef HAVE_USABLE_SIZE
    if (!a.is_hp)
        return malloc_usable_size(ptr);
#endif
    (void)a;
    return 0;
}

static size_t random_size_small_large(int alt_parity, int op) {
    if (alt_parity)
        return (size_t)((op & 1) ? (8192 + (rand() % 57344)) : (8 + (rand() % 504)));
    int r = rand() % 100;
    if (r < 55) {
        size_t sizes[] = {8, 16, 24, 32, 40, 64, 96, 128, 256, 400, 504, 512};
        return sizes[rand() % (sizeof(sizes) / sizeof(sizes[0]))];
    }
    if (r < 85) {
        size_t sizes[] = {513, 520, 600, 768, 1024, 2048, 4096};
        return sizes[rand() % (sizeof(sizes) / sizeof(sizes[0]))];
    }
    size_t sizes[] = {8192, 16384, 32768, 65536};
    return sizes[rand() % (sizeof(sizes) / sizeof(sizes[0]))];
}

static void *do_realloc(allocator_t a, void *ptr, size_t old_size, size_t new_size) {
    if (a.is_hp) {
        if (!ptr)
            return hpmalloc(new_size);
        if (new_size == 0) {
            hpfree(ptr);
            return NULL;
        }
        void *n = hpmalloc(new_size);
        if (!n)
            return NULL;
        if (old_size > 0) {
            size_t copy = old_size < new_size ? old_size : new_size;
            memcpy(n, ptr, copy);
        }
        hpfree(ptr);
        return n;
    }
    return realloc(ptr, new_size);
}

static void fill_hp_stats(bench_result_t *r) {
    hp_stats_t hs;
    hp_get_stats(&hs);

    r->has_heap_metrics = 1;
    r->sbrk_calls = hs.sbrk_calls;
    r->peak_heap_bytes = hs.peak_bytes_from_os;
    r->heap_growth_bytes = hs.bytes_from_os;

    size_t user_bytes = hs.live_user_bytes + hs.free_user_bytes;
    if (user_bytes > 0)
        r->metadata_overhead_ratio = (double)hs.metadata_bytes / (double)user_bytes;

    if (hs.free_user_bytes > 0)
        r->external_frag_ratio = (double)hs.largest_free_block / (double)hs.free_user_bytes;

    if (r->total_allocs > 0 && hs.malloc_calls > 0)
        r->reuse_rate = (double)hs.pool_allocs / (double)hs.malloc_calls;

    if (r->total_bytes_requested > 0)
        r->internal_frag_ratio = (double)r->total_bytes_allocated / (double)r->total_bytes_requested;
}

static void run_stress(const char *scenario, allocator_t a, size_t ops, int mode,
                       bench_result_t *r) {
    memset(r, 0, sizeof *r);
    r->scenario = scenario;
    r->alloc = a;

    enum { MAX_SLOTS = 4096 };
    void *slots[MAX_SLOTS];
    size_t req_sizes[MAX_SLOTS];
    memset(slots, 0, sizeof slots);
    memset(req_sizes, 0, sizeof req_sizes);

    uint64_t *malloc_samples = NULL;
    uint64_t *free_samples = NULL;
    uint64_t *realloc_samples = NULL;
    size_t malloc_n = 0, free_n = 0, realloc_n = 0;
    size_t malloc_cap = 0, free_cap = 0, realloc_cap = 0;

    size_t live = 0;
    double peak_external_frag = 0.0;
    uint64_t t0 = monotonic_ns();

    for (size_t op = 0; op < ops; op++) {
        int try_realloc = (mode == 0 && (op % 17) == 0 && live > 0) ||
                          (mode == 1 && (op % 23) == 0 && live > 0);

        if (try_realloc) {
            int idx = rand() % MAX_SLOTS;
            for (int tries = 0; tries < 32 && !slots[idx]; tries++)
                idx = rand() % MAX_SLOTS;
            if (!slots[idx])
                continue;

            size_t new_size = random_size_small_large(mode == 1, (int)op);
            size_t old_size = req_sizes[idx];
            uint64_t t1 = monotonic_ns();
            void *np = do_realloc(a, slots[idx], old_size, new_size);
            uint64_t dt = monotonic_ns() - t1;

            if (!np && new_size != 0)
                continue;

            record_latency(&realloc_samples, &realloc_n, &realloc_cap, dt);
            r->total_reallocs++;

            if (new_size == 0) {
                slots[idx] = NULL;
                req_sizes[idx] = 0;
                live--;
                r->total_frees++;
            } else {
                size_t old_req = req_sizes[idx];
                size_t got = a.is_hp ? new_size : usable_size(a, np);
                if (got == 0)
                    got = new_size;

                slots[idx] = np;
                req_sizes[idx] = new_size;
                r->total_bytes_requested += new_size;
                r->total_bytes_allocated += got;
                if (new_size > r->largest_request)
                    r->largest_request = new_size;
                (void)old_req;
            }
            continue;
        }

        int idx = rand() % MAX_SLOTS;
        if (slots[idx]) {
            uint64_t t1 = monotonic_ns();
            a.dealloc(slots[idx]);
            record_latency(&free_samples, &free_n, &free_cap, monotonic_ns() - t1);

            slots[idx] = NULL;
            req_sizes[idx] = 0;
            live--;
            r->total_frees++;
        } else {
            size_t size = random_size_small_large(mode == 1, (int)op);
            uint64_t t1 = monotonic_ns();
            void *p = a.alloc(size);
            uint64_t dt = monotonic_ns() - t1;

            if (!p)
                continue;

            record_latency(&malloc_samples, &malloc_n, &malloc_cap, dt);
            slots[idx] = p;
            req_sizes[idx] = size;
            live++;
            r->total_allocs++;

            size_t got = a.is_hp ? size : usable_size(a, p);
            if (got == 0)
                got = size;

            r->total_bytes_requested += size;
            r->total_bytes_allocated += got;
            if (size > r->largest_request)
                r->largest_request = size;
        }

        if (live > r->peak_live_allocs)
            r->peak_live_allocs = live;

        if (a.is_hp && (op % 512 == 0)) {
            hp_stats_t snap;
            hp_get_stats(&snap);
            size_t user = snap.live_user_bytes + snap.free_user_bytes;
            if (user > 0) {
                double meta = (double)snap.metadata_bytes / (double)user;
                if (meta > r->metadata_overhead_ratio)
                    r->metadata_overhead_ratio = meta;
            }
            if (snap.free_user_bytes > 0) {
                double ext = (double)snap.largest_free_block / (double)snap.free_user_bytes;
                if (ext > peak_external_frag)
                    peak_external_frag = ext;
            }
        }
    }

    if (a.is_hp) {
        hp_stats_t hs;
        hp_get_stats(&hs);
        r->has_heap_metrics = 1;
        r->sbrk_calls = hs.sbrk_calls;
        r->peak_heap_bytes = hs.peak_bytes_from_os;
        r->heap_growth_bytes = hs.bytes_from_os;
        if (hs.malloc_calls > 0)
            r->reuse_rate = (double)hs.pool_allocs / (double)hs.malloc_calls;
        r->external_frag_ratio = peak_external_frag;
    } else if (r->total_bytes_requested > 0) {
        r->internal_frag_ratio =
            (double)r->total_bytes_allocated / (double)r->total_bytes_requested;
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i]) {
            a.dealloc(slots[i]);
            r->total_frees++;
            slots[i] = NULL;
            live--;
        }
    }

    uint64_t t1 = monotonic_ns();
    r->wall_seconds = (double)(t1 - t0) / 1e9;
    r->throughput_ops_per_sec =
        (double)(r->total_allocs + r->total_frees + r->total_reallocs) / r->wall_seconds;

    summarize_latencies(malloc_samples, malloc_n, &r->malloc_lat);
    summarize_latencies(free_samples, free_n, &r->free_lat);
    summarize_latencies(realloc_samples, realloc_n, &r->realloc_lat);

    if (r->total_bytes_requested > 0)
        r->internal_frag_ratio =
            (double)r->total_bytes_allocated / (double)r->total_bytes_requested;

    free(malloc_samples);
    free(free_samples);
    free(realloc_samples);
}

static void run_long_cycles(allocator_t a, bench_result_t *r) {
    memset(r, 0, sizeof *r);
    r->scenario = "long-running cycles";
    r->alloc = a;

    enum { BATCH = 512, CYCLES = LONG_OPS / BATCH };
    void *batch[BATCH];
    size_t sizes[BATCH];

    uint64_t *malloc_samples = NULL;
    uint64_t *free_samples = NULL;
    size_t malloc_n = 0, free_n = 0;
    size_t malloc_cap = 0, free_cap = 0;

    uint64_t t0 = monotonic_ns();

    for (size_t c = 0; c < CYCLES; c++) {
        size_t live = 0;
        for (size_t i = 0; i < BATCH; i++) {
            sizes[i] = (size_t)(16 + (rand() % 2048));
            uint64_t t1 = monotonic_ns();
            batch[i] = a.alloc(sizes[i]);
            record_latency(&malloc_samples, &malloc_n, &malloc_cap, monotonic_ns() - t1);

            if (!batch[i])
                continue;

            live++;
            r->total_allocs++;
            r->total_bytes_requested += sizes[i];

            size_t got = a.is_hp ? sizes[i] : usable_size(a, batch[i]);
            if (got == 0)
                got = sizes[i];
            r->total_bytes_allocated += got;
            if (sizes[i] > r->largest_request)
                r->largest_request = sizes[i];
        }

        if (live > r->peak_live_allocs)
            r->peak_live_allocs = live;

        for (size_t i = 0; i < BATCH; i++) {
            if (!batch[i])
                continue;
            uint64_t t1 = monotonic_ns();
            a.dealloc(batch[i]);
            record_latency(&free_samples, &free_n, &free_cap, monotonic_ns() - t1);
            batch[i] = NULL;
            r->total_frees++;
        }
    }

    r->wall_seconds = (double)(monotonic_ns() - t0) / 1e9;
    r->throughput_ops_per_sec =
        (double)(r->total_allocs + r->total_frees) / r->wall_seconds;

    summarize_latencies(malloc_samples, malloc_n, &r->malloc_lat);
    summarize_latencies(free_samples, free_n, &r->free_lat);

    if (a.is_hp)
        fill_hp_stats(r);
    else if (r->total_bytes_requested > 0)
        r->internal_frag_ratio =
            (double)r->total_bytes_allocated / (double)r->total_bytes_requested;

    free(malloc_samples);
    free(free_samples);
}

static void run_leak_check(allocator_t a, bench_result_t *r) {
    memset(r, 0, sizeof *r);
    r->scenario = "leak check";
    r->alloc = a;

    void *blocks[LEAK_BLOCKS];
    size_t sizes[LEAK_BLOCKS];
    memset(blocks, 0, sizeof blocks);
    memset(sizes, 0, sizeof sizes);

    for (int i = 0; i < LEAK_BLOCKS; i++) {
        sizes[i] = (size_t)(64 + (rand() % 4096));
        blocks[i] = a.alloc(sizes[i]);
    }

    for (int i = 0; i < LEAK_BLOCKS; i += 2) {
        if (blocks[i]) {
            a.dealloc(blocks[i]);
            blocks[i] = NULL;
            sizes[i] = 0;
        }
    }

    for (int i = 0; i < LEAK_BLOCKS; i++) {
        if (blocks[i]) {
            r->leaked_blocks++;
            r->leaked_bytes += sizes[i];
        }
    }

    if (a.is_hp) {
        hp_stats_t hs;
        hp_get_stats(&hs);
        r->leaked_blocks = hs.live_blocks;
        r->leaked_bytes = hs.live_user_bytes;
    }

    for (int i = 0; i < LEAK_BLOCKS; i++) {
        if (blocks[i])
            a.dealloc(blocks[i]);
    }
}

static void print_latency_row(const char *label, const latency_stats_t *s) {
    if (s->samples == 0) {
        printf("  %-18s  (no samples)\n", label);
        return;
    }
    printf("  %-18s  avg %8.0f ns  median %8.0f ns  p95 %8.0f ns  p99 %8.0f ns  (n=%zu)\n",
           label, s->avg_ns, s->median_ns, s->p95_ns, s->p99_ns, s->samples);
}

static void print_result(const bench_result_t *r) {
    printf("\n=== %s [%s] ===\n", r->scenario, r->alloc.name);
    print_latency_row("malloc", &r->malloc_lat);
    print_latency_row("free", &r->free_lat);
    print_latency_row("realloc", &r->realloc_lat);

    printf("  throughput:        %.0f ops/sec (wall %.3f s)\n",
           r->throughput_ops_per_sec, r->wall_seconds);
    printf("  total allocs:      %zu\n", r->total_allocs);
    printf("  total frees:       %zu\n", r->total_frees);
    printf("  total reallocs:    %zu\n", r->total_reallocs);
    printf("  peak live allocs:  %zu\n", r->peak_live_allocs);
    printf("  avg request size:  %.1f bytes\n",
           r->total_allocs ? (double)r->total_bytes_requested / (double)r->total_allocs : 0.0);
    printf("  largest request:   %zu bytes\n", r->largest_request);
    printf("  total bytes req:   %zu\n", r->total_bytes_requested);

    if (r->has_heap_metrics) {
        printf("  peak heap:         %zu bytes\n", r->peak_heap_bytes);
        printf("  heap growth:       %zu bytes\n", r->heap_growth_bytes);
        printf("  sbrk calls:        %zu\n", r->sbrk_calls);
        printf("  metadata overhead: %.4f (meta/user bytes in heap)\n", r->metadata_overhead_ratio);
        printf("  internal frag:     %.3f (allocated/requested)\n", r->internal_frag_ratio);
        printf("  external frag:     %.3f (largest free / total free)\n", r->external_frag_ratio);
        printf("  reuse rate:        %.3f (pool allocs / total allocs)\n", r->reuse_rate);
    } else {
        printf("  peak heap:         n/a (system allocator)\n");
        printf("  sbrk calls:        n/a (system allocator)\n");
        printf("  metadata overhead: n/a (system allocator)\n");
        if (r->internal_frag_ratio > 0.0)
            printf("  internal frag:     %.3f (usable/requested via malloc_usable_size)\n",
                   r->internal_frag_ratio);
        else
            printf("  internal frag:     n/a\n");
        printf("  external frag:     n/a (system allocator)\n");
        printf("  reuse rate:        n/a (system allocator)\n");
    }

    if (r->scenario && strcmp(r->scenario, "leak check") == 0) {
        printf("  leaked blocks:     %zu\n", r->leaked_blocks);
        printf("  leaked bytes:      %zu\n", r->leaked_bytes);
    }
}

static void compare_metric(const char *label, double sys, double hp, const char *unit, int lower_is_better) {
    if (sys <= 0.0 || hp <= 0.0) {
        printf("  %-22s  system n/a vs hp n/a\n", label);
        return;
    }
    double ratio = sys / hp;
    const char *verdict = lower_is_better ? (hp < sys ? "hp faster" : "hp slower")
                                          : (hp > sys ? "hp higher" : "hp lower");
    printf("  %-22s  system %8.1f %s  hp %8.1f %s  (hp %.2fx %s)\n",
           label, sys, unit, hp, unit, ratio, verdict);
}

static void print_comparison(const bench_result_t *sys, const bench_result_t *hp) {
    printf("\n=== Comparison: hpmalloc vs system malloc (%s) ===\n", sys->scenario);
    compare_metric("malloc avg latency", sys->malloc_lat.avg_ns, hp->malloc_lat.avg_ns, "ns", 1);
    compare_metric("free avg latency", sys->free_lat.avg_ns, hp->free_lat.avg_ns, "ns", 1);
    compare_metric("realloc avg latency", sys->realloc_lat.avg_ns, hp->realloc_lat.avg_ns, "ns", 1);
    compare_metric("throughput", sys->throughput_ops_per_sec, hp->throughput_ops_per_sec, "ops/s", 0);
    compare_metric("internal fragmentation", sys->internal_frag_ratio, hp->internal_frag_ratio, "ratio", 1);
}

int main(void) {
    srand(42);

    allocator_t sys = {
        .alloc = malloc,
        .dealloc = free,
        .resize = realloc,
        .name = "system malloc",
        .is_hp = 0,
    };

    allocator_t hp = {
        .alloc = hpmalloc,
        .dealloc = hpfree,
        .resize = NULL,
        .name = "hpmalloc",
        .is_hp = 1,
    };

    bench_result_t results[8];
    size_t n = 0;

    printf("Allocator benchmark harness\n");
    printf("STRESS_OPS = %d  LONG_OPS = %d  LEAK_BLOCKS = %d\n\n",
           STRESS_OPS, LONG_OPS, LEAK_BLOCKS);

    run_stress("random sizes / random free", sys, STRESS_OPS, 0, &results[n++]);
    hp_reset_stats();
    run_stress("random sizes / random free", hp, STRESS_OPS, 0, &results[n++]);
    print_comparison(&results[0], &results[1]);

    run_stress("alternating small/large", sys, STRESS_OPS, 1, &results[n++]);
    hp_reset_stats();
    run_stress("alternating small/large", hp, STRESS_OPS, 1, &results[n++]);
    print_comparison(&results[2], &results[3]);

    run_long_cycles(sys, &results[n++]);
    hp_reset_stats();
    run_long_cycles(hp, &results[n++]);
    print_comparison(&results[4], &results[5]);

    run_leak_check(sys, &results[n++]);
    hp_reset_stats();
    run_leak_check(hp, &results[n++]);

    for (size_t i = 0; i < n; i++)
        print_result(&results[i]);

    return 0;
}
