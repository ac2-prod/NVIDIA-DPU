/*
 * benchmark_decompress.c — Decompression throughput benchmark for NetZIP
 *
 * L2-fitting design: 256KB sub-chunks, same-core decompress → reverse_preproc
 * so the decompressed data stays in L2 when reverse_preproc reads it.
 * Mirrors bf3_deploy's direct-mode compression benchmark.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <omp.h>
#include <unistd.h>

#include "zstd.h"
#include "lz4.h"
#include "lz4frame.h"
#include "netzip.h"
#include "snappy-c.h"
#include "zlib.h"

#define SUB_BYTES (256 * 1024)

static inline long long sec_to_ns(double sec) {
    return (long long)(sec * 1e9);
}

static const char* algo_name(int a) {
    switch (a) { case 0: return "zstd"; case 1: return "lz4"; case 3: return "snappy"; default: return "unknown"; }
}
static const char* pre_name(int p) {
    switch (p) { case 0: return "original"; case 1: return "byte_group"; case 2: return "bit_group";
                 case 3: return "diff"; case 4: return "diff_byte"; case 5: return "diff_bit"; default: return "unknown"; }
}

typedef enum { PREPROC_ORIGINAL=0, PREPROC_BYTE_GROUP, PREPROC_BIT_GROUP,
               PREPROC_DIFF, PREPROC_DIFF_BYTE, PREPROC_DIFF_BIT } PreprocMode;
typedef enum { ALGO_ZSTD=0, ALGO_LZ4=1, ALGO_SNAPPY=3 } AlgoMode;

/* ------------------------------------------------------------------ */
/*  Optimization-ladder toggles (decompress)                          */
/*                                                                    */
/*  Defaults = current code = Step6. --step {1,2,3,4,6} sets a        */
/*  cumulative preset; individual flags may override afterwards.       */
/*    g_use_lz4f      : LZ4 frame API (Step1-3) vs raw block (Step4-6) */
/*    g_use_scalar    : scalar ungroup (Step1) vs NEON (Step2+)        */
/*    g_reverse_omp   : OMP reverse over a big chunk (Step1-2) vs      */
/*                      serial reverse per L2 sub-chunk (Step3+)       */
/*    g_chunk_bytes   : 4MB chunk (Step1-2) vs 256KB sub-chunk (Step3+)*/
/*    g_dram_write    : write reverse output to strided DRAM (Step6)   */
/*                      vs reuse an L2 buffer / discard (Step1-4)       */
/* ------------------------------------------------------------------ */
static int    g_use_lz4f    = 0;
static int    g_use_scalar  = 0;
static int    g_reverse_omp = 0;
static size_t g_chunk_bytes = SUB_BYTES;
static int    g_dram_write  = 1;

/* ------------------------------------------------------------------ */
/*  LZ4 frame (LZ4F) decompress helper — used by Step1-3              */
/* ------------------------------------------------------------------ */
static size_t lz4f_decompress_frame(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_cap) {
    LZ4F_dctx* dctx = NULL;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)))
        return 0;
    size_t dst_size = dst_cap;
    size_t src_size_io = src_size;
    size_t r = LZ4F_decompress(dctx, dst, &dst_size, src, &src_size_io, NULL);
    LZ4F_freeDecompressionContext(dctx);
    return LZ4F_isError(r) ? 0 : dst_size;
}

/* ------------------------------------------------------------------ */
/*  L3 Cache Flush                                                     */
/* ------------------------------------------------------------------ */

#define L3_FLUSH_SIZE (64 * 1024 * 1024)
static volatile uint8_t* g_flush_buf = NULL;

static void flush_l3_cache(void) {
    if (!g_flush_buf) {
        g_flush_buf = (volatile uint8_t*)malloc(L3_FLUSH_SIZE);
        if (!g_flush_buf) return;
    }
    for (size_t i = 0; i < L3_FLUSH_SIZE; i += 64)
        g_flush_buf[i] = (uint8_t)i;
#ifdef __aarch64__
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

/* ------------------------------------------------------------------ */
/*  Codec wrappers                                                     */
/* ------------------------------------------------------------------ */

static size_t compress_sub(AlgoMode algo, const uint8_t* src, size_t src_size,
                           uint8_t* dst, size_t dst_cap) {
    if (algo == ALGO_ZSTD)
        return ZSTD_compress(dst, dst_cap, src, src_size, 1);
    else if (algo == ALGO_LZ4) {
        if (g_use_lz4f) {
            LZ4F_preferences_t prefs;
            memset(&prefs, 0, sizeof(prefs));
            size_t cs = LZ4F_compressFrame(dst, dst_cap, src, src_size, &prefs);
            return LZ4F_isError(cs) ? 0 : cs;
        }
        int cs = LZ4_compress_default((const char*)src, (char*)dst, src_size, dst_cap);
        return (cs > 0) ? (size_t)cs : 0;
    } else {
        size_t cs = dst_cap;
        if (snappy_compress((const char*)src, src_size, (char*)dst, &cs) != SNAPPY_OK) return 0;
        return cs;
    }
}

static size_t decompress_sub(AlgoMode algo, const uint8_t* src, size_t src_size,
                             uint8_t* dst, size_t orig_size) {
    if (algo == ALGO_ZSTD) {
        size_t d = ZSTD_decompress(dst, orig_size, src, src_size);
        return ZSTD_isError(d) ? 0 : d;
    } else if (algo == ALGO_LZ4) {
        if (g_use_lz4f)
            return lz4f_decompress_frame(src, src_size, dst, orig_size);
        int d = LZ4_decompress_safe((const char*)src, (char*)dst, src_size, orig_size);
        return (d > 0) ? (size_t)d : 0;
    } else {
        size_t ds = orig_size;
        snappy_uncompress((const char*)src, src_size, (char*)dst, &ds);
        return ds;
    }
}

/* ------------------------------------------------------------------ */
/*  Preproc helpers                                                    */
/* ------------------------------------------------------------------ */

static void apply_preproc_sub(const uint8_t* raw, size_t size, PreprocMode pre,
                              float shift, uint8_t* out, uint16_t* diff_tmp) {
    size_t n = size / 2;
    switch (pre) {
    case PREPROC_ORIGINAL:   memcpy(out, raw, size); break;
    case PREPROC_BYTE_GROUP: netzip_byte_group((const uint16_t*)raw, n, out); break;
    case PREPROC_BIT_GROUP:  netzip_bit_group((const uint16_t*)raw, n, out); break;
    case PREPROC_DIFF:       netzip_diff_encode((const uint16_t*)raw, n, shift, (uint16_t*)out); break;
    case PREPROC_DIFF_BYTE:  netzip_diff_byte_group((const uint16_t*)raw, n, shift, out); break;
    case PREPROC_DIFF_BIT:
        netzip_diff_encode((const uint16_t*)raw, n, shift, diff_tmp);
        netzip_bit_group(diff_tmp, n, out);
        break;
    }
}


/* ------------------------------------------------------------------ */
/*  Serial reverse preproc (called inside OMP parallel, no inner OMP)  */
/* ------------------------------------------------------------------ */

static void reverse_preproc_serial(const uint8_t* decomp, size_t size,
                                   PreprocMode pre, float shift,
                                   uint16_t* output, uint16_t* tmp) {
    size_t n = size / 2;
    switch (pre) {
    case PREPROC_ORIGINAL:
        memcpy(output, decomp, size); break;
    case PREPROC_BYTE_GROUP:
        netzip_byte_ungroup_serial(decomp, n, output); break;
    case PREPROC_BIT_GROUP:
        netzip_bit_ungroup_serial(decomp, n, output); break;
    case PREPROC_DIFF:
        netzip_diff_decode_serial((const uint16_t*)decomp, n, shift, output); break;
    case PREPROC_DIFF_BYTE:
        netzip_diff_byte_ungroup_serial(decomp, n, shift, output); break;
    case PREPROC_DIFF_BIT:
        netzip_bit_ungroup_serial(decomp, n, tmp);
        netzip_diff_decode_serial(tmp, n, shift, output);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Reverse over a whole chunk with OMP parallelism (Step1-2 structure) */
/*    use_scalar=1 -> non-NEON element loops parallelised with OMP (Step1)*/
/*    use_scalar=0 -> existing NEON + OMP netzip kernels (Step2)         */
/*  Both partition the full chunk; tmp must be >= size bytes (diff_bit). */
/* ------------------------------------------------------------------ */
static void reverse_preproc_omp(const uint8_t* decomp, size_t size,
                                PreprocMode pre, float shift,
                                uint16_t* output, uint16_t* tmp,
                                int n_threads, int use_scalar) {
    size_t n = size / 2;                 /* uint16 element count */
    omp_set_num_threads(n_threads);

    if (!use_scalar) {
        /* Step2: NEON kernels with internal #pragma omp parallel for */
        switch (pre) {
        case PREPROC_ORIGINAL:   memcpy(output, decomp, size); break;
        case PREPROC_BYTE_GROUP: netzip_byte_ungroup(decomp, n, output); break;
        case PREPROC_BIT_GROUP:  netzip_bit_ungroup(decomp, n, output); break;
        case PREPROC_DIFF:       netzip_diff_decode((const uint16_t*)decomp, n, shift, output); break;
        case PREPROC_DIFF_BYTE:  netzip_diff_byte_ungroup(decomp, n, shift, output); break;
        case PREPROC_DIFF_BIT:
            netzip_bit_ungroup(decomp, n, tmp);
            netzip_diff_decode(tmp, n, shift, output);
            break;
        }
        return;
    }

    /* Step1: scalar element loops, parallelised over elements */
    const uint8_t* low  = decomp;
    const uint8_t* high = decomp + n;
    size_t plane_size = (n + 7) / 8;

    if (pre == PREPROC_ORIGINAL) { memcpy(output, decomp, size); return; }

    #pragma omp parallel for num_threads(n_threads) schedule(static)
    for (size_t i = 0; i < n; i++) {
        switch (pre) {
        case PREPROC_BYTE_GROUP:
            output[i] = (uint16_t)low[i] | ((uint16_t)high[i] << 8);
            break;
        case PREPROC_DIFF:
            output[i] = fp32_to_bf16(bf16_to_fp32(((const uint16_t*)decomp)[i]) + shift);
            break;
        case PREPROC_DIFF_BYTE: {
            uint16_t v = (uint16_t)low[i] | ((uint16_t)high[i] << 8);
            output[i] = fp32_to_bf16(bf16_to_fp32(v) + shift);
            break;
        }
        case PREPROC_BIT_GROUP:
        case PREPROC_DIFF_BIT: {
            size_t byte_idx = i / 8; uint8_t bit_idx = i % 8;
            uint16_t v = 0;
            for (int pl = 0; pl < 16; pl++)
                if ((decomp[pl * plane_size + byte_idx] >> bit_idx) & 1) v |= (1u << pl);
            output[i] = (pre == PREPROC_DIFF_BIT)
                        ? fp32_to_bf16(bf16_to_fp32(v) + shift) : v;
            break;
        }
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Per-sub-chunk compressed data                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t* data;
    size_t   size;
} SubCompressed;

/* ------------------------------------------------------------------ */
/*  Error measurement                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t exact_match;
    uint64_t mismatch;
    float    max_abs_error;
    double   sum_abs_error;
} ErrorStat;

static ErrorStat measure_error(const uint16_t* orig, const uint16_t* dec, size_t n) {
    ErrorStat s = {0};
    for (size_t i = 0; i < n; i++) {
        if (orig[i] == dec[i]) { s.exact_match++; continue; }
        s.mismatch++;
        float err = fabsf(bf16_to_fp32(orig[i]) - bf16_to_fp32(dec[i]));
        if (err > s.max_abs_error) s.max_abs_error = err;
        s.sum_abs_error += err;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/*  Structure-A decompress path (Step1-2): big chunks, serial decode,  */
/*  OMP-parallel reverse. Self-contained: own layout/buffers so it can  */
/*  use 4MB chunks even at 16 threads (where the per-thread 256KB layout */
/*  would give <1 chunk/thread). Output stays in a reused L2 buffer      */
/*  (no DRAM write — matches Step1-2 presets).                           */
/* ------------------------------------------------------------------ */
static int run_structureA_decompress(uint8_t* raw_data, size_t file_size,
        float shift, int n_threads, int repeat, int do_l3_flush,
        const char* target_algo, const char* target_pre, FILE* out_fp) {

    size_t cbytes   = g_chunk_bytes;
    size_t n_chunks = file_size / cbytes;
    if (n_chunks == 0) {
        fprintf(stderr, "[StructA] file too small for %zuMB chunk\n", cbytes/(1024*1024));
        return 1;
    }
    size_t proc_bytes = n_chunks * cbytes;

    size_t max_comp = ZSTD_compressBound(cbytes);
    { size_t s = snappy_max_compressed_length(cbytes); if (s > max_comp) max_comp = s; }
    { size_t l = (size_t)LZ4F_compressFrameBound(cbytes, NULL); if (l > max_comp) max_comp = l; }

    uint8_t*  prebuf  = (uint8_t*)malloc(cbytes + 128);
    uint16_t* dtmp    = (uint16_t*)malloc(cbytes + 128);
    uint8_t*  decompB = (uint8_t*)malloc(cbytes + 128);
    uint16_t* outB    = (uint16_t*)malloc(cbytes + 128);
    uint16_t* tmpB    = (uint16_t*)malloc(cbytes + 128);
    SubCompressed* subs = (SubCompressed*)calloc(n_chunks, sizeof(SubCompressed));
    for (size_t i = 0; i < n_chunks; i++) subs[i].data = (uint8_t*)malloc(max_comp);

    /* NUMA first-touch on the active region */
    #pragma omp parallel for num_threads(n_threads) schedule(static)
    for (size_t i = 0; i < proc_bytes; i += 4096) raw_data[i] = raw_data[i];

    typedef struct { double e2e; long long ca; long long cb; } RR;
    RR* results = (RR*)malloc(sizeof(RR) * repeat);

    AlgoMode    algos[]    = {ALGO_LZ4, ALGO_SNAPPY, ALGO_ZSTD};
    PreprocMode preprocs[] = {PREPROC_ORIGINAL, PREPROC_BYTE_GROUP, PREPROC_BIT_GROUP,
                              PREPROC_DIFF, PREPROC_DIFF_BYTE, PREPROC_DIFF_BIT};

    fprintf(out_fp,
        "threads,codec,preproc_mode,bytes_original,bytes_compressed,ratio,"
        "total_decomp_ns,codec_decomp_ns,reverse_preproc_ns,"
        "throughput_gbps,codec_pct,reverse_pct,"
        "exact_match,mismatch,mismatch_pct,max_abs_error,mean_abs_error\n");

    for (int a = 0; a < 3; a++) {
        for (int p = 0; p < 6; p++) {
            AlgoMode    cur_algo = algos[a];
            PreprocMode cur_pre  = preprocs[p];
            if (target_algo && strcmp(target_algo, algo_name(cur_algo)) != 0) continue;
            if (target_pre  && strcmp(target_pre,  pre_name(cur_pre))  != 0) continue;

            /* Pre-compress all chunks (NOT timed) */
            size_t total_compressed = 0; int ok = 1;
            for (size_t c = 0; c < n_chunks; c++) {
                apply_preproc_sub(raw_data + c*cbytes, cbytes, cur_pre, shift, prebuf, dtmp);
                size_t cs = compress_sub(cur_algo, prebuf, cbytes, subs[c].data, max_comp);
                if (cs == 0) { ok = 0; break; }
                subs[c].size = cs; total_compressed += cs;
            }
            if (!ok) { fprintf(stderr,"[StructA] compress failed %s/%s\n",
                               algo_name(cur_algo), pre_name(cur_pre)); continue; }

            /* Verify round-trip + error (NOT timed) */
            ErrorStat err_total = {0};
            for (size_t c = 0; c < n_chunks; c++) {
                decompress_sub(cur_algo, subs[c].data, subs[c].size, decompB, cbytes);
                reverse_preproc_omp(decompB, cbytes, cur_pre, shift, outB, tmpB,
                                    n_threads, g_use_scalar);
                ErrorStat e = measure_error((const uint16_t*)(raw_data + c*cbytes),
                                            outB, cbytes/2);
                err_total.exact_match += e.exact_match;
                err_total.mismatch    += e.mismatch;
                err_total.sum_abs_error += e.sum_abs_error;
                if (e.max_abs_error > err_total.max_abs_error) err_total.max_abs_error = e.max_abs_error;
            }
            float mean_err = (err_total.mismatch > 0)
                ? (float)(err_total.sum_abs_error / err_total.mismatch) : 0.0f;

            /* Warmup */
            for (size_t c = 0; c < n_chunks; c++) {
                decompress_sub(cur_algo, subs[c].data, subs[c].size, decompB, cbytes);
                reverse_preproc_omp(decompB, cbytes, cur_pre, shift, outB, tmpB,
                                    n_threads, g_use_scalar);
            }

            /* Timed: serial decode per chunk + OMP reverse */
            for (int r = 0; r < repeat; r++) {
                if (do_l3_flush) flush_l3_cache();
                long long codec_ns = 0, rev_ns = 0;
                double t_start = omp_get_wtime();
                for (size_t c = 0; c < n_chunks; c++) {
                    double tc0 = omp_get_wtime();
                    decompress_sub(cur_algo, subs[c].data, subs[c].size, decompB, cbytes);
                    double tc1 = omp_get_wtime();
                    reverse_preproc_omp(decompB, cbytes, cur_pre, shift, outB, tmpB,
                                        n_threads, g_use_scalar);
                    double tc2 = omp_get_wtime();
                    codec_ns += sec_to_ns(tc1 - tc0);
                    rev_ns   += sec_to_ns(tc2 - tc1);
                }
                double t_end = omp_get_wtime();
                results[r].e2e = t_end - t_start;
                results[r].ca = codec_ns;
                results[r].cb = rev_ns;
            }

            for (int i = 0; i < repeat-1; i++)
                for (int j = 0; j < repeat-i-1; j++)
                    if (results[j].e2e > results[j+1].e2e) { RR t=results[j]; results[j]=results[j+1]; results[j+1]=t; }
            RR med = results[repeat/2];

            long long total_ns = sec_to_ns(med.e2e);
            double ratio   = (double)proc_bytes / (double)total_compressed;
            double gbps    = ((double)proc_bytes / 1e9) / med.e2e;
            double cod_pct = (total_ns > 0) ? 100.0 * med.ca / total_ns : 0;
            double rev_pct = (total_ns > 0) ? 100.0 * med.cb / total_ns : 0;
            uint64_t tot_elem   = err_total.exact_match + err_total.mismatch;
            double mismatch_pct = (tot_elem > 0) ? 100.0 * err_total.mismatch / tot_elem : 0;

            fprintf(stderr, "[StructA OK] %s/%s gbps=%.3f (codec %.1f%% rev %.1f%%)\n",
                    algo_name(cur_algo), pre_name(cur_pre), gbps, cod_pct, rev_pct);
            fprintf(out_fp, "%d,%s,%s,%zu,%zu,%.4f,%lld,%lld,%lld,%.4f,%.2f,%.2f,"
                    "%lu,%lu,%.6f,%.6e,%.6e\n",
                    n_threads, algo_name(cur_algo), pre_name(cur_pre),
                    proc_bytes, total_compressed, ratio,
                    total_ns, med.ca, med.cb, gbps, cod_pct, rev_pct,
                    (unsigned long)err_total.exact_match, (unsigned long)err_total.mismatch,
                    mismatch_pct, err_total.max_abs_error, mean_err);
            fflush(out_fp);
        }
    }

    for (size_t i = 0; i < n_chunks; i++) free(subs[i].data);
    free(subs); free(results);
    free(prebuf); free(dtmp); free(decompB); free(outB); free(tmpB);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    omp_set_max_active_levels(1);

    char*  input_file   = NULL;
    int    n_threads    = 16;
    char*  output_csv   = NULL;
    int    repeat       = 10;
    float  manual_shift = NAN;
    char*  target_algo  = NULL;
    char*  target_pre   = NULL;
    size_t limit_size   = 0;
    int    do_l3_flush  = 0;
    int    mode_compress = 0;
    int    step_preset  = 0;   /* 0 = use current defaults (Step6) */

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--input")   && i+1<argc) input_file  = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i+1<argc) n_threads   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")     && i+1<argc) output_csv  = argv[++i];
        else if (!strcmp(argv[i], "--repeat")  && i+1<argc) repeat      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shift")   && i+1<argc) manual_shift= atof(argv[++i]);
        else if (!strcmp(argv[i], "--algo")    && i+1<argc) target_algo = argv[++i];
        else if (!strcmp(argv[i], "--pre")     && i+1<argc) target_pre  = argv[++i];
        else if (!strcmp(argv[i], "--size")    && i+1<argc) limit_size  = (size_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--l3-flush")) do_l3_flush = 1;
        else if (!strcmp(argv[i], "--compress")) mode_compress = 1;
        /* ── ladder preset ── */
        else if (!strcmp(argv[i], "--step") && i+1<argc) step_preset = atoi(argv[++i]);
        /* ── individual toggles (override the preset) ── */
        else if (!strcmp(argv[i], "--codec-lz4f"))    g_use_lz4f = 1;
        else if (!strcmp(argv[i], "--codec-lz4raw"))  g_use_lz4f = 0;
        else if (!strcmp(argv[i], "--ungroup-scalar"))g_use_scalar = 1;
        else if (!strcmp(argv[i], "--ungroup-neon"))  g_use_scalar = 0;
        else if (!strcmp(argv[i], "--reverse-omp"))   g_reverse_omp = 1;
        else if (!strcmp(argv[i], "--reverse-serial"))g_reverse_omp = 0;
        else if (!strcmp(argv[i], "--chunk-4mb"))     g_chunk_bytes = 4*1024*1024;
        else if (!strcmp(argv[i], "--subchunk-256kb"))g_chunk_bytes = 256*1024;
        else if (!strcmp(argv[i], "--chunk-kb") && i+1<argc) g_chunk_bytes = (size_t)atoi(argv[++i])*1024;
        else if (!strcmp(argv[i], "--no-dram-write")) g_dram_write = 0;
        else if (!strcmp(argv[i], "--dram-write"))    g_dram_write = 1;
    }

    if (!input_file) {
        fprintf(stderr, "Usage: %s --input <file> [--threads N] [--out <csv>] "
                "[--repeat N] [--l3-flush] [--algo X] [--pre X] [--compress]\n"
                "  --step {1,2,3,4,6}   decompress optimization-ladder preset\n"
                "  toggles: --codec-lz4f/--codec-lz4raw --ungroup-scalar/--ungroup-neon\n"
                "           --reverse-omp/--reverse-serial --chunk-4mb/--subchunk-256kb\n"
                "           --no-dram-write/--dram-write\n", argv[0]);
        return 1;
    }

    /* Apply --step preset. If --step is given it fully defines the config
     * (overriding any individual toggles). Use EITHER --step OR explicit
     * toggles, not both. --step 0 / omitted keeps current defaults (= Step6). */
    switch (step_preset) {
    case 1: g_use_lz4f=1; g_use_scalar=1; g_reverse_omp=1; g_chunk_bytes=4*1024*1024; g_dram_write=0; break;
    case 2: g_use_lz4f=1; g_use_scalar=0; g_reverse_omp=1; g_chunk_bytes=4*1024*1024; g_dram_write=0; break;
    case 3: g_use_lz4f=1; g_use_scalar=0; g_reverse_omp=0; g_chunk_bytes=256*1024;    g_dram_write=0; break;
    case 4: g_use_lz4f=0; g_use_scalar=0; g_reverse_omp=0; g_chunk_bytes=256*1024;    g_dram_write=0; break;
    case 6: g_use_lz4f=0; g_use_scalar=0; g_reverse_omp=0; g_chunk_bytes=256*1024;    g_dram_write=1; break;
    default: break;  /* 0: keep defaults (= current code = Step6) */
    }
    fprintf(stderr, "[Ladder] step=%d lz4f=%d scalar=%d reverse_omp=%d chunk=%zuKB dram_write=%d\n",
            step_preset, g_use_lz4f, g_use_scalar, g_reverse_omp, g_chunk_bytes/1024, g_dram_write);

    FILE* out_fp = stdout;
    if (output_csv) {
        out_fp = fopen(output_csv, "w");
        if (!out_fp) { perror("fopen output"); return 1; }
    }

    /* --- Load data --- */
    FILE* f = fopen(input_file, "rb");
    if (!f) { perror("fopen input"); return 1; }
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* raw_data = (uint8_t*)malloc(file_size);
    fread(raw_data, 1, file_size, f);
    fclose(f);

    if (limit_size > 0 && limit_size < file_size)
        file_size = (limit_size / 2) * 2;

    /* --- Global min --- */
    float global_min = 0.0f;
    if (!isnan(manual_shift)) {
        global_min = manual_shift;
    } else {
        size_t n = file_size / 2;
        const uint16_t* p = (const uint16_t*)raw_data;
        float fm = bf16_to_fp32(p[0]);
        for (size_t k = 1; k < n; k++) { float v = bf16_to_fp32(p[k]); if (v < fm) fm = v; }
        global_min = fm;
    }

    /* --- Structure-A dispatch (Step1-2: big chunk + OMP reverse) --- */
    if (!mode_compress && g_reverse_omp) {
        int rc = run_structureA_decompress(raw_data, file_size, global_min,
                                           n_threads, repeat, do_l3_flush,
                                           target_algo, target_pre, out_fp);
        free(raw_data);
        if (g_flush_buf) free((void*)g_flush_buf);
        if (output_csv) fclose(out_fp);
        return rc;
    }

    /* --- Sub-chunk layout (Structure B: 256KB sub-chunks per thread) --- */
    size_t sub_bytes  = g_chunk_bytes;
    size_t per_thread = (file_size / (size_t)n_threads / sub_bytes) * sub_bytes;
    size_t proc_bytes = per_thread * (size_t)n_threads;
    int    num_sub_per_thread = (int)(per_thread / sub_bytes);
    int    total_subs = num_sub_per_thread * n_threads;

    if (num_sub_per_thread == 0) {
        fprintf(stderr, "Error: file too small for %d threads with sub=%zuKB\n",
                n_threads, sub_bytes / 1024);
        return 1;
    }

    /* --- NUMA first-touch --- */
    #pragma omp parallel for num_threads(n_threads) schedule(static)
    for (size_t i = 0; i < proc_bytes; i += 4096) raw_data[i] = raw_data[i];

    #pragma omp parallel num_threads(n_threads)
    {
        #pragma omp master
        fprintf(stderr, "[Info] OMP threads: %d, sub_chunks: %d (%zuKB each), "
                "per_thread: %d, proc_bytes: %zuMB\n",
                omp_get_num_threads(), total_subs, sub_bytes/1024,
                num_sub_per_thread, proc_bytes/1024/1024);
    }

    /* --- Buffers for pre-compression --- */
    size_t max_comp = ZSTD_compressBound(sub_bytes);
    { size_t s = snappy_max_compressed_length(sub_bytes); if (s > max_comp) max_comp = s; }
    { size_t l = (size_t)LZ4F_compressFrameBound(sub_bytes, NULL); if (l > max_comp) max_comp = l; }

    uint8_t*  preproc_buf = (uint8_t*)malloc(sub_bytes + 128);
    uint16_t* diff_tmp    = (uint16_t*)malloc(sub_bytes + 128);

    /* Per-sub-chunk compressed storage */
    SubCompressed* subs = (SubCompressed*)calloc(total_subs, sizeof(SubCompressed));
    for (int i = 0; i < total_subs; i++)
        subs[i].data = (uint8_t*)malloc(max_comp);

    /* Per-thread working buffers:
     * - decomp_buf: 256KB (L2-resident intermediate, reused per sub-chunk)
     * - tmp_buf:    256KB (L2-resident intermediate for bit_ungroup+diff)
     * - output_buf: per_thread bytes (DRAM destination, different offset per sub-chunk)
     *   Simulates writing final output to GPU/NIC buffer — forces DRAM writes. */
    uint8_t**  decomp_bufs = (uint8_t**)malloc(n_threads * sizeof(uint8_t*));
    uint8_t**  output_bufs = (uint8_t**)malloc(n_threads * sizeof(uint8_t*));
    uint16_t** tmp_bufs    = (uint16_t**)malloc(n_threads * sizeof(uint16_t*));
    for (int t = 0; t < n_threads; t++) {
        decomp_bufs[t] = (uint8_t*)malloc(sub_bytes + 128);
        output_bufs[t] = (uint8_t*)malloc(per_thread + 128);
        tmp_bufs[t]    = (uint16_t*)malloc(sub_bytes + 128);
    }

    /* NUMA first-touch for per-thread buffers */
    #pragma omp parallel num_threads(n_threads)
    {
        int tid = omp_get_thread_num();
        memset(decomp_bufs[tid], 0, sub_bytes + 128);
        memset(output_bufs[tid], 0, per_thread + 128);
        memset(tmp_bufs[tid],    0, sub_bytes + 128);
    }

    /* --- Scenarios --- */
    AlgoMode    algos[]    = {ALGO_LZ4, ALGO_SNAPPY, ALGO_ZSTD};
    PreprocMode preprocs[] = {PREPROC_ORIGINAL, PREPROC_BYTE_GROUP, PREPROC_BIT_GROUP,
                              PREPROC_DIFF, PREPROC_DIFF_BYTE, PREPROC_DIFF_BIT};

    typedef struct { double e2e_sec; long long ns_a; long long ns_b; } RunResult;
    RunResult* results = (RunResult*)malloc(sizeof(RunResult) * repeat);

    /* ====================================================================
     * COMPRESSION MODE
     * ==================================================================== */
    if (mode_compress) {
        fprintf(out_fp,
            "threads,codec,preproc_mode,bytes_original,bytes_compressed,ratio,"
            "total_comp_ns,preproc_ns,codec_ns,"
            "throughput_gbps,preproc_pct,codec_pct\n");

        /* Per-thread buffers for compression:
         * - preproc_th: 256KB (L2 intermediate, reused per sub-chunk)
         * - diff_tmp_th: 256KB (L2 intermediate for diff_bit)
         * - comp_out: num_sub_per_thread * max_comp (DRAM, strided) */
        uint8_t**  preproc_th  = (uint8_t**)malloc(n_threads * sizeof(uint8_t*));
        uint16_t** diff_tmp_th = (uint16_t**)malloc(n_threads * sizeof(uint16_t*));
        uint8_t**  comp_out    = (uint8_t**)malloc(n_threads * sizeof(uint8_t*));
        for (int t = 0; t < n_threads; t++) {
            preproc_th[t]  = (uint8_t*)malloc(sub_bytes + 128);
            diff_tmp_th[t] = (uint16_t*)malloc(sub_bytes + 128);
            comp_out[t]    = (uint8_t*)malloc((size_t)num_sub_per_thread * max_comp);
        }

        #pragma omp parallel num_threads(n_threads)
        {
            int tid = omp_get_thread_num();
            memset(preproc_th[tid],  0, sub_bytes + 128);
            memset(diff_tmp_th[tid], 0, sub_bytes + 128);
            memset(comp_out[tid],    0, (size_t)num_sub_per_thread * max_comp);
        }

        for (int a = 0; a < 3; a++) {
            for (int p = 0; p < 6; p++) {
                AlgoMode    cur_algo = algos[a];
                PreprocMode cur_pre  = preprocs[p];
                if (target_algo && strcmp(target_algo, algo_name(cur_algo)) != 0) continue;
                if (target_pre  && strcmp(target_pre,  pre_name(cur_pre))  != 0) continue;

                /* Warmup */
                #pragma omp parallel num_threads(n_threads)
                {
                    int tid = omp_get_thread_num();
                    for (int s = 0; s < num_sub_per_thread; s++) {
                        int si = tid * num_sub_per_thread + s;
                        const uint8_t* src = raw_data + (size_t)si * sub_bytes;
                        apply_preproc_sub(src, sub_bytes, cur_pre, global_min,
                                          preproc_th[tid], diff_tmp_th[tid]);
                        compress_sub(cur_algo, preproc_th[tid], sub_bytes,
                                     comp_out[tid] + (size_t)s * max_comp, max_comp);
                    }
                }

                /* Timed measurement */
                size_t total_compressed = 0;
                for (int r = 0; r < repeat; r++) {
                    if (do_l3_flush) flush_l3_cache();

                    long long sum_pre_ns = 0, sum_codec_ns = 0;
                    size_t r_comp = 0;

                    double t_start = omp_get_wtime();

                    #pragma omp parallel num_threads(n_threads) \
                        reduction(+:sum_pre_ns,sum_codec_ns,r_comp)
                    {
                        int tid = omp_get_thread_num();
                        uint8_t*  my_pre  = preproc_th[tid];
                        uint16_t* my_dtmp = diff_tmp_th[tid];
                        uint8_t*  my_cout = comp_out[tid];
                        long long lp = 0, lc = 0;

                        for (int s = 0; s < num_sub_per_thread; s++) {
                            int si = tid * num_sub_per_thread + s;
                            const uint8_t* src = raw_data + (size_t)si * sub_bytes;
                            uint8_t* dst = my_cout + (size_t)s * max_comp;

                            double t0 = omp_get_wtime();
                            apply_preproc_sub(src, sub_bytes, cur_pre, global_min,
                                              my_pre, my_dtmp);
                            double t1 = omp_get_wtime();
                            size_t cs = compress_sub(cur_algo, my_pre, sub_bytes,
                                                     dst, max_comp);
                            double t2 = omp_get_wtime();

                            lp += sec_to_ns(t1 - t0);
                            lc += sec_to_ns(t2 - t1);
                            r_comp += cs;
                        }
                        sum_pre_ns   += lp;
                        sum_codec_ns += lc;
                    }

                    double t_end = omp_get_wtime();

                    results[r].e2e_sec = t_end - t_start;
                    results[r].ns_a    = sum_pre_ns   / n_threads;
                    results[r].ns_b    = sum_codec_ns / n_threads;
                    if (r == 0) total_compressed = r_comp;
                }

                /* Median */
                for (int i = 0; i < repeat - 1; i++)
                    for (int j = 0; j < repeat - i - 1; j++)
                        if (results[j].e2e_sec > results[j+1].e2e_sec) {
                            RunResult t = results[j]; results[j] = results[j+1]; results[j+1] = t;
                        }
                RunResult med = results[repeat / 2];

                long long total_ns = sec_to_ns(med.e2e_sec);
                double ratio   = (double)proc_bytes / (double)total_compressed;
                double gbps    = ((double)proc_bytes / 1e9) / med.e2e_sec;
                double pre_pct = (total_ns > 0) ? 100.0 * med.ns_a / total_ns : 0;
                double cod_pct = (total_ns > 0) ? 100.0 * med.ns_b / total_ns : 0;

                fprintf(out_fp, "%d,%s,%s,%zu,%zu,%.4f,%lld,%lld,%lld,%.4f,%.2f,%.2f\n",
                        n_threads, algo_name(cur_algo), pre_name(cur_pre),
                        proc_bytes, total_compressed, ratio,
                        total_ns, med.ns_a, med.ns_b,
                        gbps, pre_pct, cod_pct);
                fflush(out_fp);
            }
        }

        for (int t = 0; t < n_threads; t++) {
            free(preproc_th[t]); free(diff_tmp_th[t]); free(comp_out[t]);
        }
        free(preproc_th); free(diff_tmp_th); free(comp_out);

        goto cleanup;
    }

    /* ====================================================================
     * DECOMPRESSION MODE (default)
     * ==================================================================== */
    fprintf(out_fp,
        "threads,codec,preproc_mode,bytes_original,bytes_compressed,ratio,"
        "total_decomp_ns,codec_decomp_ns,reverse_preproc_ns,"
        "throughput_gbps,codec_pct,reverse_pct,"
        "exact_match,mismatch,mismatch_pct,max_abs_error,mean_abs_error\n");

    for (int a = 0; a < 3; a++) {
        for (int p = 0; p < 6; p++) {
            AlgoMode    cur_algo = algos[a];
            PreprocMode cur_pre  = preprocs[p];
            if (target_algo && strcmp(target_algo, algo_name(cur_algo)) != 0) continue;
            if (target_pre  && strcmp(target_pre,  pre_name(cur_pre))  != 0) continue;

            /* ================================================================
             * Step 1: Pre-compress all sub-chunks (NOT timed)
             * ================================================================ */
            size_t total_compressed = 0;
            int compress_ok = 1;
            for (int s = 0; s < total_subs; s++) {
                const uint8_t* chunk_raw = raw_data + (size_t)s * sub_bytes;
                apply_preproc_sub(chunk_raw, sub_bytes, cur_pre, global_min,
                                  preproc_buf, diff_tmp);
                size_t cs = compress_sub(cur_algo, preproc_buf, sub_bytes,
                                         subs[s].data, max_comp);
                if (cs == 0) {
                    fprintf(stderr, "[ERROR] Compress failed: %s/%s sub %d\n",
                            algo_name(cur_algo), pre_name(cur_pre), s);
                    compress_ok = 0; break;
                }
                subs[s].size = cs;
                total_compressed += cs;
            }
            if (!compress_ok) continue;

            /* ================================================================
             * Step 2: Verification + error measurement (NOT timed)
             * ================================================================ */
            int has_diff = (cur_pre == PREPROC_DIFF || cur_pre == PREPROC_DIFF_BYTE
                            || cur_pre == PREPROC_DIFF_BIT);
            ErrorStat err_total = {0};
            int verify_ok = 1;
            for (int s = 0; s < total_subs && verify_ok; s++) {
                const uint8_t* chunk_raw = raw_data + (size_t)s * sub_bytes;

                /* Re-generate preprocessed reference */
                apply_preproc_sub(chunk_raw, sub_bytes, cur_pre, global_min,
                                  preproc_buf, diff_tmp);

                /* Decompress and verify codec losslessness */
                size_t ds = decompress_sub(cur_algo, subs[s].data, subs[s].size,
                                           decomp_bufs[0], sub_bytes);
                if (ds != sub_bytes || memcmp(preproc_buf, decomp_bufs[0], sub_bytes) != 0) {
                    fprintf(stderr, "[FAIL] %s/%s sub %d: codec mismatch\n",
                            algo_name(cur_algo), pre_name(cur_pre), s);
                    verify_ok = 0; break;
                }

                /* Full round-trip error */
                uint16_t* verify_out = (uint16_t*)output_bufs[0];
                reverse_preproc_serial(decomp_bufs[0], sub_bytes, cur_pre, global_min,
                                       verify_out, tmp_bufs[0]);
                ErrorStat e = measure_error((const uint16_t*)chunk_raw,
                                            verify_out, sub_bytes / 2);
                err_total.exact_match   += e.exact_match;
                err_total.mismatch      += e.mismatch;
                err_total.sum_abs_error += e.sum_abs_error;
                if (e.max_abs_error > err_total.max_abs_error)
                    err_total.max_abs_error = e.max_abs_error;

                if (!has_diff && e.mismatch > 0) {
                    fprintf(stderr, "[FAIL] %s/%s sub %d: %lu mismatches (non-diff)\n",
                            algo_name(cur_algo), pre_name(cur_pre), s,
                            (unsigned long)e.mismatch);
                    verify_ok = 0;
                }
            }
            if (!verify_ok) continue;

            float mean_err = (err_total.mismatch > 0)
                ? (float)(err_total.sum_abs_error / err_total.mismatch) : 0.0f;
            fprintf(stderr, "[OK] %s / %s (exact=%lu, mismatch=%lu, max=%.3e, mean=%.3e)\n",
                    algo_name(cur_algo), pre_name(cur_pre),
                    (unsigned long)err_total.exact_match,
                    (unsigned long)err_total.mismatch,
                    err_total.max_abs_error, mean_err);

            /* ================================================================
             * Step 3: Warmup
             * ================================================================ */
            #pragma omp parallel num_threads(n_threads)
            {
                int tid = omp_get_thread_num();
                for (int s = 0; s < num_sub_per_thread; s++) {
                    int si = tid * num_sub_per_thread + s;
                    uint16_t* out_dst = (uint16_t*)(output_bufs[tid] + (size_t)s * sub_bytes);
                    decompress_sub(cur_algo, subs[si].data, subs[si].size,
                                   decomp_bufs[tid], sub_bytes);
                    reverse_preproc_serial(decomp_bufs[tid], sub_bytes, cur_pre,
                                           global_min, out_dst, tmp_bufs[tid]);
                }
            }

            /* ================================================================
             * Step 4: Timed measurement
             *
             * Each thread: for each 256KB sub-chunk:
             *   decompress → decomp_buf (L2, reused)
             *   reverse_preproc → output_buf + s*sub_bytes (DRAM, strided)
             * ================================================================ */
            for (int r = 0; r < repeat; r++) {
                if (do_l3_flush) flush_l3_cache();

                long long sum_codec_ns = 0, sum_rev_ns = 0;

                double t_start = omp_get_wtime();

                #pragma omp parallel num_threads(n_threads) \
                    reduction(+:sum_codec_ns,sum_rev_ns)
                {
                    int tid = omp_get_thread_num();
                    uint8_t*  my_decomp = decomp_bufs[tid];
                    uint8_t*  my_out_base = output_bufs[tid];
                    uint16_t* my_tmp    = tmp_bufs[tid];
                    long long local_codec = 0, local_rev = 0;

                    for (int s = 0; s < num_sub_per_thread; s++) {
                        int si = tid * num_sub_per_thread + s;
                        /* DRAM-write toggle (Step6 vs Step3-4):
                         *   g_dram_write=1 -> strided per-sub-chunk DRAM destination
                         *   g_dram_write=0 -> reuse offset 0 (output stays L2-resident) */
                        uint16_t* out_dst = g_dram_write
                            ? (uint16_t*)(my_out_base + (size_t)s * sub_bytes)
                            : (uint16_t*)my_out_base;

                        double tc0 = omp_get_wtime();
                        decompress_sub(cur_algo, subs[si].data, subs[si].size,
                                       my_decomp, sub_bytes);
                        double tc1 = omp_get_wtime();
                        reverse_preproc_serial(my_decomp, sub_bytes, cur_pre,
                                               global_min, out_dst, my_tmp);
                        double tc2 = omp_get_wtime();

                        local_codec += sec_to_ns(tc1 - tc0);
                        local_rev   += sec_to_ns(tc2 - tc1);
                    }

                    sum_codec_ns += local_codec;
                    sum_rev_ns   += local_rev;
                }

                double t_end = omp_get_wtime();

                results[r].e2e_sec  = t_end - t_start;
                results[r].ns_a = sum_codec_ns / n_threads;
                results[r].ns_b   = sum_rev_ns   / n_threads;
            }

            /* ================================================================
             * Median
             * ================================================================ */
            for (int i = 0; i < repeat - 1; i++)
                for (int j = 0; j < repeat - i - 1; j++)
                    if (results[j].e2e_sec > results[j+1].e2e_sec) {
                        RunResult t = results[j]; results[j] = results[j+1]; results[j+1] = t;
                    }
            RunResult med = results[repeat / 2];

            long long total_ns = sec_to_ns(med.e2e_sec);
            double ratio     = (double)proc_bytes / (double)total_compressed;
            double gbps      = ((double)proc_bytes / 1e9) / med.e2e_sec;
            double codec_pct = (total_ns > 0) ? 100.0 * med.ns_a / total_ns : 0;
            double rev_pct   = (total_ns > 0) ? 100.0 * med.ns_b  / total_ns : 0;

            uint64_t tot_elem    = err_total.exact_match + err_total.mismatch;
            double mismatch_pct  = (tot_elem > 0) ? 100.0 * err_total.mismatch / tot_elem : 0;

            fprintf(out_fp, "%d,%s,%s,%zu,%zu,%.4f,%lld,%lld,%lld,%.4f,%.2f,%.2f,"
                    "%lu,%lu,%.6f,%.6e,%.6e\n",
                    n_threads, algo_name(cur_algo), pre_name(cur_pre),
                    proc_bytes, total_compressed, ratio,
                    total_ns, med.ns_a, med.ns_b,
                    gbps, codec_pct, rev_pct,
                    (unsigned long)err_total.exact_match,
                    (unsigned long)err_total.mismatch, mismatch_pct,
                    err_total.max_abs_error, mean_err);
            fflush(out_fp);
        }
    }

cleanup:
    /* --- Cleanup --- */
    free(results);
    for (int i = 0; i < total_subs; i++) free(subs[i].data);
    free(subs);
    for (int t = 0; t < n_threads; t++) {
        free(decomp_bufs[t]); free(output_bufs[t]); free((void*)tmp_bufs[t]);
    }
    free(decomp_bufs); free(output_bufs); free(tmp_bufs);
    free(preproc_buf); free(diff_tmp);
    free(raw_data);
    if (g_flush_buf) free((void*)g_flush_buf);
    if (output_csv) fclose(out_fp);

    return 0;
}
