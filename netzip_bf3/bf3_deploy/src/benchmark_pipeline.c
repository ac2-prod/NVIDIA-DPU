// benchmark_pipeline.c  (v2: L2-fitting sub-chunks + split sweep)
//
// Producer/Consumer pipeline benchmark with:
//   - 256KB sub-chunks (L2-fitting, same as benchmark_l3)
//   - Serial preproc functions (no internal OMP)
//   - pthread mutex/cond queue between preproc and codec thread groups
//   - Automatic all-pattern iteration (6 preproc × 3 codec = 18 patterns)
//   - Split ratio sweep: 6:10 / 8:8 / 10:6 / 12:4 / 14:2
//   - Direct (non-pipeline) mode for comparison
//   - Cold-cache mode, auto-shift, NUMA first-touch
//
// Usage: ./benchmark_pipeline --input <file> [options]
//   --sub-kb N         sub-chunk size KB          [default: 256 = L2 size]
//   --threads N        total threads              [default: 16]
//   --preproc-th N     preproc threads (manual)   [default: auto sweep]
//   --codec-th N       codec threads (manual)     [default: auto sweep]
//   --queue-depth N    ring buffer depth           [default: threads*2]
//   --pre <mode>       specific preproc mode       [default: all 6]
//   --algo <name>      specific codec              [default: snappy,lz4,zstd]
//   --shift N          bf16 shift value            [default: auto from data]
//   --repeat N         measurement repeats         [default: 5]
//   --level N          compression level           [default: 1]
//   --out <file>       CSV output file             [default: stdout]
//   --cold-cache       flush L3 before each run
//   --direct-only      run only direct mode (no pipeline)
//   --pipeline-only    run only pipeline mode (no direct)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <omp.h>

#include "zstd.h"
#include "lz4.h"
#include "lz4frame.h"
#include "netzip.h"
#include "snappy-c.h"
#include "zlib.h"

// ─── Cache Flush ─────────────────────────────────────────────────────────────
#define FLUSH_SIZE (64 * 1024 * 1024)  // 64MB > L3 32MB
static char flush_buf[FLUSH_SIZE];

static void flush_caches(int n_threads) {
    #pragma omp parallel num_threads(n_threads)
    {
        int tid = omp_get_thread_num();
        int nth = omp_get_num_threads();
        size_t chunk = FLUSH_SIZE / nth;
        size_t start = (size_t)tid * chunk;
        size_t end   = (tid == nth - 1) ? FLUSH_SIZE : start + chunk;
        volatile char sum = 0;
        for (size_t i = start; i < end; i += 64) {
            flush_buf[i] = (char)i;
            sum += flush_buf[i];
        }
        (void)sum;
    }
}

// L3フラッシュ: 毎回 malloc/free で確実に新しい物理ページを取得し
// L3(32MB) の 2倍 = 64MB を書き込んでキャッシュラインを追い出す
static void flush_l3_cache(void) {
    const size_t L3_FLUSH_SIZE = 64UL * 1024 * 1024;
    volatile char *buf = (volatile char *)malloc(L3_FLUSH_SIZE);
    if (!buf) return;
    for (size_t i = 0; i < L3_FLUSH_SIZE; i += 64) {
        buf[i] = (char)i;
    }
#ifdef __aarch64__
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
    free((void *)buf);
}

// ─── Enums ───────────────────────────────────────────────────────────────────

typedef enum {
    PREPROC_ORIGINAL = 0, PREPROC_BYTE_GROUP, PREPROC_BIT_GROUP,
    PREPROC_DIFF, PREPROC_DIFF_BYTE, PREPROC_DIFF_BIT
} PreprocMode;

typedef enum { ALGO_ZSTD = 0, ALGO_LZ4, ALGO_ZLIB, ALGO_SNAPPY } AlgoMode;

static const char* algo_name(AlgoMode a) {
    switch (a) {
        case ALGO_ZSTD: return "zstd"; case ALGO_LZ4: return "lz4";
        case ALGO_ZLIB: return "zlib"; default: return "snappy";
    }
}

static const char* pre_name(PreprocMode p) {
    switch (p) {
        case PREPROC_ORIGINAL:   return "original";
        case PREPROC_BYTE_GROUP: return "byte_group";
        case PREPROC_BIT_GROUP:  return "bit_group";
        case PREPROC_DIFF:       return "diff";
        case PREPROC_DIFF_BYTE:  return "diff_byte";
        default:                 return "diff_bit";
    }
}

// ─── Pipeline Slot ───────────────────────────────────────────────────────────

typedef struct {
    uint8_t* preproc_buf;
    uint8_t* tmp_buf;
    uint8_t* codec_buf;
    size_t   data_size;
    size_t   compressed_sz;
    int      chunk_idx;
} PSlot;

// ─── Pipeline Queue ──────────────────────────────────────────────────────────

typedef struct {
    PSlot*  slots;
    int     cap;

    // Free pool (stack)
    int*    free_stack;
    int     free_top;

    // Ready queue (ring)
    int*    ready_ring;
    int     ready_head;
    int     ready_tail;
    int     ready_count;

    int     producers_remaining;

    pthread_mutex_t mu;
    pthread_cond_t  free_avail;
    pthread_cond_t  ready_avail;
} PQueue;

static void pq_init(PQueue* q, int cap, size_t chunk_size,
                    size_t max_comp, int n_producers) {
    q->cap              = cap;
    q->free_top         = cap - 1;
    q->ready_head       = 0;
    q->ready_tail       = 0;
    q->ready_count      = 0;
    q->producers_remaining = n_producers;

    q->slots      = (PSlot*)malloc(cap * sizeof(PSlot));
    q->free_stack = (int*)malloc(cap * sizeof(int));
    q->ready_ring = (int*)malloc(cap * sizeof(int));

    pthread_mutex_init(&q->mu,          NULL);
    pthread_cond_init (&q->free_avail,  NULL);
    pthread_cond_init (&q->ready_avail, NULL);

    for (int i = 0; i < cap; i++) {
        q->slots[i].preproc_buf  = (uint8_t*)malloc(chunk_size + 128);
        q->slots[i].tmp_buf      = (uint8_t*)malloc(chunk_size + 128);
        q->slots[i].codec_buf    = (uint8_t*)malloc(max_comp   + 128);
        q->slots[i].data_size    = 0;
        q->slots[i].compressed_sz= 0;
        q->slots[i].chunk_idx    = -1;
        q->free_stack[i]         = i;
    }
}

static void pq_reset(PQueue* q, int n_producers) {
    q->free_top            = q->cap - 1;
    q->ready_head          = 0;
    q->ready_tail          = 0;
    q->ready_count         = 0;
    q->producers_remaining = n_producers;
    for (int i = 0; i < q->cap; i++)
        q->free_stack[i] = i;
}

static void pq_destroy(PQueue* q) {
    for (int i = 0; i < q->cap; i++) {
        free(q->slots[i].preproc_buf);
        free(q->slots[i].tmp_buf);
        free(q->slots[i].codec_buf);
    }
    free(q->slots);
    free(q->free_stack);
    free(q->ready_ring);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->free_avail);
    pthread_cond_destroy(&q->ready_avail);
}

static int pq_acquire(PQueue* q) {
    pthread_mutex_lock(&q->mu);
    while (q->free_top < 0)
        pthread_cond_wait(&q->free_avail, &q->mu);
    int idx = q->free_stack[q->free_top--];
    pthread_mutex_unlock(&q->mu);
    return idx;
}

static void pq_push_ready(PQueue* q, int idx) {
    pthread_mutex_lock(&q->mu);
    q->ready_ring[q->ready_tail] = idx;
    q->ready_tail = (q->ready_tail + 1) % q->cap;
    q->ready_count++;
    pthread_cond_signal(&q->ready_avail);
    pthread_mutex_unlock(&q->mu);
}

static void pq_producer_done(PQueue* q) {
    pthread_mutex_lock(&q->mu);
    q->producers_remaining--;
    if (q->producers_remaining == 0)
        pthread_cond_broadcast(&q->ready_avail);
    pthread_mutex_unlock(&q->mu);
}

static int pq_pop_ready(PQueue* q) {
    pthread_mutex_lock(&q->mu);
    while (q->ready_count == 0 && q->producers_remaining > 0)
        pthread_cond_wait(&q->ready_avail, &q->mu);
    if (q->ready_count == 0) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    int idx = q->ready_ring[q->ready_head];
    q->ready_head = (q->ready_head + 1) % q->cap;
    q->ready_count--;
    pthread_mutex_unlock(&q->mu);
    return idx;
}

static void pq_release(PQueue* q, int idx) {
    pthread_mutex_lock(&q->mu);
    q->free_stack[++q->free_top] = idx;
    pthread_cond_signal(&q->free_avail);
    pthread_mutex_unlock(&q->mu);
}

// ─── Preproc Helper (serial functions, same as benchmark_l3) ─────────────────

static void do_preproc(PreprocMode mode, const uint8_t* input, size_t size,
                       uint8_t* preproc_buf, uint8_t* tmp_buf, float shift) {
    size_t n = size / 2;
    switch (mode) {
    case PREPROC_ORIGINAL:
        memcpy(preproc_buf, input, size);
        break;
    case PREPROC_BYTE_GROUP:
        netzip_byte_group_serial((const uint16_t*)input, n, preproc_buf);
        break;
    case PREPROC_BIT_GROUP:
        netzip_bit_group_serial((const uint16_t*)input, n, preproc_buf);
        break;
    case PREPROC_DIFF:
        netzip_diff_encode_serial((const uint16_t*)input, n, shift,
                                  (uint16_t*)preproc_buf);
        break;
    case PREPROC_DIFF_BYTE:
        netzip_diff_byte_group_serial((const uint16_t*)input, n, shift,
                                      preproc_buf);
        break;
    case PREPROC_DIFF_BIT:
        netzip_diff_encode_serial((const uint16_t*)input, n, shift,
                                  (uint16_t*)tmp_buf);
        netzip_bit_group_serial((const uint16_t*)tmp_buf, n, preproc_buf);
        break;
    }
}

// ─── Codec Helper ────────────────────────────────────────────────────────────

static size_t do_codec(AlgoMode algo, const uint8_t* src, size_t src_size,
                       uint8_t* dst, size_t dst_cap, int level) {
    if (algo == ALGO_SNAPPY) {
        size_t csz = dst_cap;
        snappy_compress((const char*)src, src_size, (char*)dst, &csz);
        return csz;
    } else if (algo == ALGO_LZ4) {
        LZ4F_preferences_t prefs;
        memset(&prefs, 0, sizeof(prefs));
        return LZ4F_compressFrame(dst, dst_cap, src, src_size, &prefs);
    } else if (algo == ALGO_ZSTD) {
        return ZSTD_compress(dst, dst_cap, src, src_size, level);
    } else {
        uLongf dsz = (uLongf)dst_cap;
        compress(dst, &dsz, src, src_size);
        return (size_t)dsz;
    }
}

// ─── Thread Contexts ─────────────────────────────────────────────────────────

typedef struct {
    const uint8_t*  raw_data;
    size_t          chunk_size;
    int             total_chunks;
    PreprocMode     preproc;
    AlgoMode        algo;
    float           shift;
    int             level;
    PQueue*         queue;
    size_t          max_comp;
    // Static assignment: this thread handles chunks [start_chunk, end_chunk)
    int             start_chunk;
    int             end_chunk;
    // Output
    size_t          total_compressed;
} ThreadCtx;

// ─── Producer Thread (static assignment) ─────────────────────────────────────

static void* producer_thread(void* arg) {
    ThreadCtx* ctx = (ThreadCtx*)arg;

    // Suppress OMP nesting inside serial preproc functions
    omp_set_num_threads(1);

    for (int cidx = ctx->start_chunk; cidx < ctx->end_chunk; cidx++) {
        const uint8_t* chunk_in = ctx->raw_data + (size_t)cidx * ctx->chunk_size;

        int slot_idx   = pq_acquire(ctx->queue);
        PSlot* slot    = &ctx->queue->slots[slot_idx];
        slot->chunk_idx = cidx;
        slot->data_size = ctx->chunk_size;

        do_preproc(ctx->preproc, chunk_in, ctx->chunk_size,
                   slot->preproc_buf, slot->tmp_buf, ctx->shift);

        pq_push_ready(ctx->queue, slot_idx);
    }

    pq_producer_done(ctx->queue);
    return NULL;
}

// ─── Consumer Thread ─────────────────────────────────────────────────────────

static void* consumer_thread(void* arg) {
    ThreadCtx* ctx = (ThreadCtx*)arg;
    size_t local_compressed = 0;

    for (;;) {
        int slot_idx = pq_pop_ready(ctx->queue);
        if (slot_idx < 0) break;

        PSlot* slot = &ctx->queue->slots[slot_idx];
        size_t csz = do_codec(ctx->algo,
                               slot->preproc_buf, slot->data_size,
                               slot->codec_buf,   ctx->max_comp,
                               ctx->level);
        slot->compressed_sz = csz;
        local_compressed   += csz;

        pq_release(ctx->queue, slot_idx);
    }

    ctx->total_compressed = local_compressed;
    return NULL;
}

// ─── Run Pipeline Once ───────────────────────────────────────────────────────

typedef struct {
    double e2e_sec;
    size_t total_compressed;
} PipeResult;

static PipeResult run_pipeline(const uint8_t* data, size_t proc_bytes,
                                size_t chunk_size, int n_preproc, int n_codec,
                                PQueue* q, PreprocMode pre, AlgoMode algo,
                                float shift, int level, size_t max_comp) {
    int total_chunks = (int)(proc_bytes / chunk_size);

    pq_reset(q, n_preproc);

    ThreadCtx* prod_ctx = (ThreadCtx*)calloc(n_preproc, sizeof(ThreadCtx));
    ThreadCtx* cons_ctx = (ThreadCtx*)calloc(n_codec,   sizeof(ThreadCtx));

    // Static chunk assignment for producers
    int chunks_per_prod = total_chunks / n_preproc;
    int remainder       = total_chunks % n_preproc;
    int offset = 0;
    for (int i = 0; i < n_preproc; i++) {
        int my_chunks = chunks_per_prod + (i < remainder ? 1 : 0);
        prod_ctx[i].raw_data     = data;
        prod_ctx[i].chunk_size   = chunk_size;
        prod_ctx[i].total_chunks = total_chunks;
        prod_ctx[i].preproc      = pre;
        prod_ctx[i].algo         = algo;
        prod_ctx[i].shift        = shift;
        prod_ctx[i].level        = level;
        prod_ctx[i].queue        = q;
        prod_ctx[i].max_comp     = max_comp;
        prod_ctx[i].start_chunk  = offset;
        prod_ctx[i].end_chunk    = offset + my_chunks;
        offset += my_chunks;
    }
    for (int i = 0; i < n_codec; i++) {
        cons_ctx[i].raw_data     = data;
        cons_ctx[i].chunk_size   = chunk_size;
        cons_ctx[i].total_chunks = total_chunks;
        cons_ctx[i].preproc      = pre;
        cons_ctx[i].algo         = algo;
        cons_ctx[i].shift        = shift;
        cons_ctx[i].level        = level;
        cons_ctx[i].queue        = q;
        cons_ctx[i].max_comp     = max_comp;
    }

    pthread_t* prod_tid = (pthread_t*)malloc(n_preproc * sizeof(pthread_t));
    pthread_t* cons_tid = (pthread_t*)malloc(n_codec   * sizeof(pthread_t));

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int i = 0; i < n_codec;   i++) pthread_create(&cons_tid[i], NULL, consumer_thread, &cons_ctx[i]);
    for (int i = 0; i < n_preproc; i++) pthread_create(&prod_tid[i], NULL, producer_thread, &prod_ctx[i]);

    for (int i = 0; i < n_preproc; i++) pthread_join(prod_tid[i], NULL);
    for (int i = 0; i < n_codec;   i++) pthread_join(cons_tid[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    double e2e = (t_end.tv_sec  - t_start.tv_sec) +
                 (t_end.tv_nsec - t_start.tv_nsec) * 1e-9;

    size_t total_compressed = 0;
    for (int i = 0; i < n_codec; i++)
        total_compressed += cons_ctx[i].total_compressed;

    free(prod_tid); free(cons_tid);
    free(prod_ctx); free(cons_ctx);

    return (PipeResult){ .e2e_sec = e2e, .total_compressed = total_compressed };
}

// ─── Run Direct (non-pipeline, same as benchmark_l3) ─────────────────────────

static PipeResult run_direct(const uint8_t* data, size_t proc_bytes,
                              size_t sub_bytes, int n_threads,
                              PreprocMode pre, AlgoMode algo,
                              float shift, int level, size_t max_comp) {
    size_t per_thread = (proc_bytes / (size_t)n_threads / sub_bytes) * sub_bytes;
    int    num_sub    = (int)(per_thread / sub_bytes);
    size_t actual_proc = per_thread * (size_t)n_threads;

    // Per-thread buffers
    uint8_t* preproc_buf = (uint8_t*)malloc((size_t)n_threads * sub_bytes + 128);
    uint8_t* tmp_buf     = (uint8_t*)malloc((size_t)n_threads * sub_bytes + 128);
    uint8_t** codec_bufs = (uint8_t**)malloc(n_threads * sizeof(uint8_t*));
    for (int t = 0; t < n_threads; t++)
        codec_bufs[t] = (uint8_t*)malloc(max_comp + 128);

    size_t compressed_total = 0;

    double t_start = omp_get_wtime();

    #pragma omp parallel num_threads(n_threads) reduction(+:compressed_total)
    {
        int tid = omp_get_thread_num();
        const uint8_t* my_block = data + (size_t)tid * per_thread;
        uint8_t* my_preproc = preproc_buf + (size_t)tid * sub_bytes;
        uint8_t* my_tmp     = tmp_buf     + (size_t)tid * sub_bytes;
        uint8_t* my_codec   = codec_bufs[tid];
        size_t sub_elem = sub_bytes / 2;

        for (int s = 0; s < num_sub; s++) {
            const uint8_t*  src_u8  = my_block + (size_t)s * sub_bytes;
            const uint16_t* src_u16 = (const uint16_t*)src_u8;

            // Phase 1: preproc
            switch (pre) {
            case PREPROC_ORIGINAL:
                memcpy(my_preproc, src_u8, sub_bytes); break;
            case PREPROC_BYTE_GROUP:
                netzip_byte_group_serial(src_u16, sub_elem, my_preproc); break;
            case PREPROC_BIT_GROUP:
                netzip_bit_group_serial(src_u16, sub_elem, my_preproc); break;
            case PREPROC_DIFF:
                netzip_diff_encode_serial(src_u16, sub_elem, shift,
                                          (uint16_t*)my_preproc); break;
            case PREPROC_DIFF_BYTE:
                netzip_diff_byte_group_serial(src_u16, sub_elem, shift,
                                              my_preproc); break;
            case PREPROC_DIFF_BIT:
                netzip_diff_encode_serial(src_u16, sub_elem, shift,
                                          (uint16_t*)my_tmp);
                netzip_bit_group_serial((const uint16_t*)my_tmp, sub_elem,
                                        my_preproc); break;
            }

            // Phase 2: codec
            size_t csz = do_codec(algo, my_preproc, sub_bytes,
                                  my_codec, max_comp, level);
            compressed_total += csz;
        }
    }

    double t_end = omp_get_wtime();

    for (int t = 0; t < n_threads; t++) free(codec_bufs[t]);
    free(codec_bufs);
    free(preproc_buf);
    free(tmp_buf);

    return (PipeResult){
        .e2e_sec = t_end - t_start,
        .total_compressed = compressed_total
    };
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const char* input_file  = NULL;
    int         sub_kb      = 256;
    int         n_threads   = 16;
    int         queue_depth = 0;   // 0 = auto (threads*2)
    int         repeat      = 5;
    int         level       = 1;
    float       shift       = NAN;
    int         shift_specified = 0;
    const char* target_pre  = NULL;
    const char* target_algo = NULL;
    const char* out_file    = NULL;
    int         cold_cache  = 0;
    int         l3_flush    = 0;
    int         direct_only = 0;
    int         pipeline_only = 0;
    int         manual_split = 0;
    int         manual_preproc_th = 0;
    int         manual_codec_th   = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--input")       && i+1<argc) input_file = argv[++i];
        else if (!strcmp(argv[i], "--sub-kb")      && i+1<argc) sub_kb     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads")     && i+1<argc) n_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preproc-th")  && i+1<argc) { manual_preproc_th = atoi(argv[++i]); manual_split = 1; }
        else if (!strcmp(argv[i], "--codec-th")    && i+1<argc) { manual_codec_th   = atoi(argv[++i]); manual_split = 1; }
        else if (!strcmp(argv[i], "--queue-depth") && i+1<argc) queue_depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat")      && i+1<argc) repeat     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shift")       && i+1<argc) { shift = (float)atof(argv[++i]); shift_specified = 1; }
        else if (!strcmp(argv[i], "--level")       && i+1<argc) level      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")         && i+1<argc) out_file   = argv[++i];
        else if (!strcmp(argv[i], "--algo")        && i+1<argc) target_algo = argv[++i];
        else if (!strcmp(argv[i], "--pre")         && i+1<argc) target_pre  = argv[++i];
        else if (!strcmp(argv[i], "--cold-cache"))    cold_cache    = 1;
        else if (!strcmp(argv[i], "--l3-flush"))      l3_flush      = 1;
        else if (!strcmp(argv[i], "--direct-only"))   direct_only   = 1;
        else if (!strcmp(argv[i], "--pipeline-only")) pipeline_only = 1;
    }

    if (!input_file) {
        fprintf(stderr,
            "Usage: %s --input <file> [options]\n"
            "  --sub-kb N         sub-chunk size KB          [default: 256 = L2]\n"
            "  --threads N        total threads              [default: 16]\n"
            "  --preproc-th N     preproc threads (manual)   [skips sweep]\n"
            "  --codec-th N       codec threads (manual)     [skips sweep]\n"
            "  --queue-depth N    ring buffer depth           [default: threads*2]\n"
            "  --pre <mode>       specific preproc mode       [default: all 6]\n"
            "  --algo <name>      snappy/lz4/zstd             [default: all 3]\n"
            "  --shift N          bf16 shift                  [default: auto]\n"
            "  --repeat N         repeats                     [default: 5]\n"
            "  --level N          compression level           [default: 1]\n"
            "  --out <file>       CSV output                  [default: stdout]\n"
            "  --cold-cache       flush L2 before each run\n"
            "  --l3-flush         flush L3 cache before each run\n"
            "  --direct-only      run only direct mode\n"
            "  --pipeline-only    run only pipeline mode\n",
            argv[0]);
        return 1;
    }

    // Sub-chunk size (16-byte aligned for NEON)
    size_t sub_bytes = ((size_t)sub_kb * 1024 / 16) * 16;
    if (sub_bytes == 0) sub_bytes = 256 * 1024;

    if (queue_depth == 0) queue_depth = n_threads * 2;

    omp_set_num_threads(n_threads);
    omp_set_max_active_levels(1);

    // ── Load data ────────────────────────────────────────────────────────────
    FILE* f = fopen(input_file, "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    size_t file_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* raw = (uint8_t*)malloc(file_size);
    if (!raw) { perror("malloc raw"); return 1; }
    fread(raw, 1, file_size, f);
    fclose(f);

    // NUMA first-touch
    #pragma omp parallel for num_threads(n_threads) schedule(static)
    for (size_t i = 0; i < file_size; i += 4096) raw[i] = raw[i];

    // Auto-compute shift
    if (!shift_specified) {
        shift = netzip_find_min((uint16_t*)raw, file_size / 2);
        fprintf(stderr, "  auto shift = %.6f (from netzip_find_min)\n", shift);
    }

    // Compute proc_bytes aligned to sub_bytes * n_threads
    size_t per_thread = (file_size / (size_t)n_threads / sub_bytes) * sub_bytes;
    size_t proc_bytes = per_thread * (size_t)n_threads;
    int    total_chunks = (int)(proc_bytes / sub_bytes);
    int    num_sub_per_thread = (int)(per_thread / sub_bytes);

    if (num_sub_per_thread == 0) {
        fprintf(stderr, "Error: file too small for %d threads with sub_kb=%d\n",
                n_threads, sub_kb);
        return 1;
    }

    // Compute max compressed buffer size
    size_t max_comp = ZSTD_compressBound(sub_bytes);
    size_t snappy_max = snappy_max_compressed_length(sub_bytes);
    size_t lz4_max    = (size_t)LZ4F_compressFrameBound(sub_bytes, NULL);
    if (snappy_max > max_comp) max_comp = snappy_max;
    if (lz4_max    > max_comp) max_comp = lz4_max;

    // ── Output ───────────────────────────────────────────────────────────────
    FILE* out_fp = stdout;
    if (out_file) {
        out_fp = fopen(out_file, "w");
        if (!out_fp) { perror("fopen out"); return 1; }
    }

    fprintf(out_fp,
        "mode,algo,pre,split,sub_kb,threads,total_chunks,"
        "total_bytes,compressed_bytes,ratio,gbps_median,gbps_best\n");
    fflush(out_fp);

    fprintf(stderr,
        "Pipeline benchmark: sub=%zuKB, threads=%d, chunks=%d%s\n"
        "  proc_bytes=%zuMB, sub_per_thread=%d\n\n",
        sub_bytes/1024, n_threads, total_chunks,
        cold_cache ? " [COLD-CACHE]" : "",
        proc_bytes/1024/1024, num_sub_per_thread);

    // ── Pattern definitions ──────────────────────────────────────────────────
    AlgoMode    algos[]    = {ALGO_SNAPPY, ALGO_LZ4, ALGO_ZSTD};
    PreprocMode preprocs[] = {PREPROC_ORIGINAL, PREPROC_BYTE_GROUP, PREPROC_BIT_GROUP,
                              PREPROC_DIFF, PREPROC_DIFF_BYTE, PREPROC_DIFF_BIT};
    int n_algos = 3, n_preprocs = 6;

    // Split ratios to sweep (preproc:codec)
    int splits[][2] = { {6,10}, {8,8}, {10,6}, {12,4}, {14,2} };
    int n_splits = 5;

    // If manual split, use only that
    int manual_splits[][2] = { {0, 0} };
    if (manual_split) {
        manual_splits[0][0] = manual_preproc_th;
        manual_splits[0][1] = manual_codec_th;
        n_splits = 1;
    }

    double* gbps_arr = (double*)malloc(repeat * sizeof(double));

    // ── Main loop: algo × preproc × mode × split ────────────────────────────
    for (int ai = 0; ai < n_algos; ai++) {
        if (target_algo && strcmp(target_algo, algo_name(algos[ai])) != 0) continue;
        AlgoMode cur_algo = algos[ai];

        for (int pi = 0; pi < n_preprocs; pi++) {
            if (target_pre && strcmp(target_pre, pre_name(preprocs[pi])) != 0) continue;
            PreprocMode cur_pre = preprocs[pi];

            // ── Direct mode ──────────────────────────────────────────────
            if (!pipeline_only) {
                fprintf(stderr, "--- direct: %s + %s%s ---\n",
                        algo_name(cur_algo), pre_name(cur_pre),
                        cold_cache ? " (cold)" : "");

                // Warmup
                if (!cold_cache) {
                    run_direct(raw, proc_bytes, sub_bytes, n_threads,
                               cur_pre, cur_algo, shift, level, max_comp);
                }

                double best_gbps = 0;
                size_t comp_bytes = 0;
                for (int r = 0; r < repeat; r++) {
                    if (cold_cache) flush_caches(n_threads);
                    if (l3_flush)   flush_l3_cache();
                    PipeResult res = run_direct(raw, proc_bytes, sub_bytes, n_threads,
                                                cur_pre, cur_algo, shift, level, max_comp);
                    double gbps = (double)proc_bytes / 1e9 / res.e2e_sec;
                    gbps_arr[r] = gbps;
                    if (gbps > best_gbps) { best_gbps = gbps; comp_bytes = res.total_compressed; }
                    fprintf(stderr, "  repeat %d/%d: %.3f GB/s\n", r+1, repeat, gbps);
                }

                // Median
                for (int i = 0; i < repeat-1; i++)
                    for (int j = 0; j < repeat-i-1; j++)
                        if (gbps_arr[j] > gbps_arr[j+1]) {
                            double t = gbps_arr[j]; gbps_arr[j] = gbps_arr[j+1]; gbps_arr[j+1] = t;
                        }
                double median = gbps_arr[repeat/2];
                double ratio = (comp_bytes > 0) ? (double)proc_bytes / (double)comp_bytes : 0.0;

                fprintf(out_fp, "direct,%s,%s,%d:%d,%zu,%d,%d,%zu,%zu,%.4f,%.4f,%.4f\n",
                        algo_name(cur_algo), pre_name(cur_pre),
                        n_threads, 0, sub_bytes/1024, n_threads, total_chunks,
                        proc_bytes, comp_bytes, ratio, median, best_gbps);
                fflush(out_fp);
            }

            // ── Pipeline mode (sweep splits) ─────────────────────────────
            if (!direct_only) {
                for (int si = 0; si < n_splits; si++) {
                    int np = manual_split ? manual_splits[0][0] : splits[si][0];
                    int nc = manual_split ? manual_splits[0][1] : splits[si][1];

                    // Validate
                    if (np < 1 || nc < 1) continue;
                    if (total_chunks < np) continue;  // need at least 1 chunk per producer

                    int qd = queue_depth;
                    if (qd < np) qd = np;

                    fprintf(stderr, "--- pipeline %d:%d: %s + %s%s ---\n",
                            np, nc, algo_name(cur_algo), pre_name(cur_pre),
                            cold_cache ? " (cold)" : "");

                    // Allocate queue once per split ratio
                    PQueue q;
                    pq_init(&q, qd, sub_bytes, max_comp, np);

                    // Warmup
                    if (!cold_cache) {
                        run_pipeline(raw, proc_bytes, sub_bytes, np, nc,
                                     &q, cur_pre, cur_algo, shift, level, max_comp);
                    }

                    double best_gbps = 0;
                    size_t comp_bytes = 0;
                    for (int r = 0; r < repeat; r++) {
                        if (cold_cache) flush_caches(n_threads);
                        if (l3_flush)   flush_l3_cache();
                        PipeResult res = run_pipeline(raw, proc_bytes, sub_bytes, np, nc,
                                                      &q, cur_pre, cur_algo, shift, level, max_comp);
                        double gbps = (double)proc_bytes / 1e9 / res.e2e_sec;
                        gbps_arr[r] = gbps;
                        if (gbps > best_gbps) { best_gbps = gbps; comp_bytes = res.total_compressed; }
                        fprintf(stderr, "  repeat %d/%d: %.3f GB/s\n", r+1, repeat, gbps);
                    }

                    // Median
                    for (int i = 0; i < repeat-1; i++)
                        for (int j = 0; j < repeat-i-1; j++)
                            if (gbps_arr[j] > gbps_arr[j+1]) {
                                double t = gbps_arr[j]; gbps_arr[j] = gbps_arr[j+1]; gbps_arr[j+1] = t;
                            }
                    double median = gbps_arr[repeat/2];
                    double ratio = (comp_bytes > 0) ? (double)proc_bytes / (double)comp_bytes : 0.0;

                    fprintf(out_fp, "pipeline,%s,%s,%d:%d,%zu,%d,%d,%zu,%zu,%.4f,%.4f,%.4f\n",
                            algo_name(cur_algo), pre_name(cur_pre),
                            np, nc, sub_bytes/1024, np+nc, total_chunks,
                            proc_bytes, comp_bytes, ratio, median, best_gbps);
                    fflush(out_fp);

                    pq_destroy(&q);
                }
            }
        }
    }

    if (out_file && out_fp != stdout) fclose(out_fp);

    free(gbps_arr);
    free(raw);

    return 0;
}
