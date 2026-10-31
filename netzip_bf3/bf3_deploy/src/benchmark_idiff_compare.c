// benchmark_idiff_compare.c — Issue #4/#5: 4 前処理の lossless 比較
//   byte_group (純並べ替え, lossless) / bit_group (純並べ替え, lossless) /
//   diff_byte (float, lossy; 誤差は ±1ULP に収まらない) / idiff_byte (整数 diff, lossless)
// ─────────────────────────────────────────────────────────────────────────────
// 同条件 (16 スレッド, 256KB サブチャンク連続割当, run_direct 準拠) で:
//   - LZ4 圧縮率
//   - preproc / compress / decompress / restore の各スループット (DRAM write 込み)
//   - round-trip の bit-exact 検証 (memcmp, 不一致バイト数)
// を測り results_lossless_compare.csv に追記する。複数データファイルを引数で渡すと
// それぞれについて 4 preproc の行を出す (圧縮率が lm_head 特有でないかの確認用)。
//
// 計時方法: 各フェーズを独立した OMP 並列領域で囲み clock_gettime(MONOTONIC) で計測。
//   各スレッドは連続した sub_elem サブチャンク群を担当 (run_direct と同じブロック割当)。
//   出力は毎回フルサイズ配列へ書く → DRAM write が実際に発生し単体測定と同条件。
//   warmup 後に median を採る。スループット基準は論理元サイズ (decimal GB/s)。
//
// 使い方:
//   ./benchmark_idiff_compare [--out CSV] [--iters N] [--warmup N] [--sub-kb K]
//                             file1 [file2 ...]
//   ファイル無指定なら既定の lm_head を使う。
// ─────────────────────────────────────────────────────────────────────────────
#define _GNU_SOURCE
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "netzip.h"
#include "lz4.h"

#define DEFAULT_DATA "../../bf3_data/forward/0212_lm_head.pkl.bin"
#define MAX_ITERS 64

// 4 前処理: byte_group/bit_group は純粋な並べ替え (lossless), diff_byte は float
// (lossy; 誤差は ±1ULP に収まらない), idiff_byte は整数 diff (lossless)。byte/bit は shift 不要。
enum { PRE_BYTE_GROUP = 0, PRE_BIT_GROUP, PRE_DIFF_BYTE, PRE_IDIFF_BYTE, PRE_N };
static const char *pre_name[] = { "byte_group", "bit_group", "diff_byte", "idiff_byte" };

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y);
}
static double median(double *v, int n) {
    qsort(v, n, sizeof(double), cmp_d);
    return (n & 1) ? v[n/2] : 0.5 * (v[n/2-1] + v[n/2]);
}

static long file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void get_commit(char *out, size_t n) {
    snprintf(out, n, "nogit");
    FILE *p = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (p) {
        char b[64];
        if (fgets(b, sizeof b, p)) { b[strcspn(b, "\n")] = 0; if (b[0]) snprintf(out, n, "%s", b); }
        pclose(p);
    }
}

// グローバル作業バッファ (フルサイズ; DRAM write を実発生させる)
static size_t  g_sub_bytes, g_sub_elem, g_bound, g_nsub, g_proc_bytes;
static int     g_nth;
static uint16_t *g_src;        // 元 u16
static uint8_t  *g_preproc;    // preproc 出力 (proc_bytes)
static uint8_t  *g_comp;       // 圧縮出力 (nsub * bound, サブチャンク毎 stride)
static int      *g_csize;      // 各サブチャンク圧縮サイズ
static uint8_t  *g_decomp;     // 伸長 = preproc 再構成 (proc_bytes)
static uint16_t *g_restored;   // 復元 u16 (proc_bytes)
static float    g_shift;       // diff_byte 用 (idiff では未使用)

// スレッド t が担当するサブチャンク範囲 [s0,s1)
static inline void block_range(int t, size_t *s0, size_t *s1) {
    size_t per = (g_nsub + (size_t)g_nth - 1) / (size_t)g_nth;
    *s0 = (size_t)t * per;
    *s1 = *s0 + per;
    if (*s0 > g_nsub) *s0 = g_nsub;
    if (*s1 > g_nsub) *s1 = g_nsub;
}

static void phase_preproc(int mode) {
    #pragma omp parallel num_threads(g_nth)
    {
        size_t s0, s1; block_range(omp_get_thread_num(), &s0, &s1);
        for (size_t s = s0; s < s1; s++) {
            const uint16_t *in  = g_src + s * g_sub_elem;
            uint8_t        *out = g_preproc + s * g_sub_bytes;
            switch (mode) {
            case PRE_BYTE_GROUP: netzip_byte_group_serial(in, g_sub_elem, out); break;
            case PRE_BIT_GROUP:  netzip_bit_group_serial(in, g_sub_elem, out); break;
            case PRE_DIFF_BYTE:  netzip_diff_byte_group_serial(in, g_sub_elem, g_shift, out); break;
            case PRE_IDIFF_BYTE: netzip_idiff_byte_group_serial(in, g_sub_elem, out); break;
            }
        }
    }
}
static void phase_compress(void) {
    #pragma omp parallel num_threads(g_nth)
    {
        size_t s0, s1; block_range(omp_get_thread_num(), &s0, &s1);
        for (size_t s = s0; s < s1; s++) {
            g_csize[s] = LZ4_compress_default(
                (const char*)(g_preproc + s * g_sub_bytes),
                (char*)(g_comp + s * g_bound),
                (int)g_sub_bytes, (int)g_bound);
        }
    }
}
static void phase_decompress(void) {
    #pragma omp parallel num_threads(g_nth)
    {
        size_t s0, s1; block_range(omp_get_thread_num(), &s0, &s1);
        for (size_t s = s0; s < s1; s++) {
            LZ4_decompress_safe((const char*)(g_comp + s * g_bound),
                                (char*)(g_decomp + s * g_sub_bytes),
                                g_csize[s], (int)g_sub_bytes);
        }
    }
}
static void phase_restore(int mode) {
    #pragma omp parallel num_threads(g_nth)
    {
        size_t s0, s1; block_range(omp_get_thread_num(), &s0, &s1);
        for (size_t s = s0; s < s1; s++) {
            const uint8_t *in  = g_decomp + s * g_sub_bytes;
            uint16_t      *out = g_restored + s * g_sub_elem;
            switch (mode) {
            case PRE_BYTE_GROUP: netzip_byte_ungroup_serial(in, g_sub_elem, out); break;
            case PRE_BIT_GROUP:  netzip_bit_ungroup_serial(in, g_sub_elem, out); break;
            case PRE_DIFF_BYTE:  netzip_diff_byte_ungroup_serial(in, g_sub_elem, g_shift, out); break;
            case PRE_IDIFF_BYTE: netzip_idiff_byte_ungroup_serial(in, g_sub_elem, out); break;
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *out_csv = "results_lossless_compare.csv";
    int iters = 7, warmup = 2;
    g_sub_bytes = 256 * 1024;
    const char *files[64]; int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--out")    && i+1 < argc) out_csv = argv[++i];
        else if (!strcmp(argv[i], "--iters")  && i+1 < argc) iters   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i+1 < argc) warmup  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sub-kb") && i+1 < argc) g_sub_bytes = (size_t)atoi(argv[++i]) * 1024;
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
        else if (nfiles < 64) files[nfiles++] = argv[i];
    }
    if (nfiles == 0) files[nfiles++] = DEFAULT_DATA;
    if (iters > MAX_ITERS) iters = MAX_ITERS;
    g_sub_elem = g_sub_bytes / 2;
    g_bound = (size_t)LZ4_compressBound((int)g_sub_bytes);
    g_nth = omp_get_max_threads();

    char commit[64], ts[32];
    get_commit(commit, sizeof commit);
    time_t tt = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&tt));

    int hdr = (file_size(out_csv) < 0);
    FILE *csv = fopen(out_csv, "a");
    if (!csv) { perror(out_csv); return 1; }
    if (hdr) fprintf(csv,
        "data,preproc,bit_exact,mismatch_bytes,total_bytes,comp_bytes,comp_ratio,"
        "t_preproc_ms,t_compress_ms,t_decompress_ms,t_restore_ms,"
        "gbps_preproc,gbps_compress,gbps_decompress,gbps_restore,"
        "omp_threads,sub_bytes,iters,commit_hash,timestamp\n");

    printf("=== idiff_byte (bit-exact) vs diff_byte (float, lossy) ===\n");
    printf("threads=%d sub=%zuKB iters=%d warmup=%d\n\n", g_nth, g_sub_bytes/1024, iters, warmup);

    for (int fi = 0; fi < nfiles; fi++) {
        long fsz = file_size(files[fi]);
        if (fsz < (long)g_sub_bytes) { fprintf(stderr, "skip %s (size %ld)\n", files[fi], fsz); continue; }
        g_nsub = (size_t)fsz / g_sub_bytes;            // 端数サブチャンクは切り捨て (全条件共通)
        g_proc_bytes = g_nsub * g_sub_bytes;

        g_src      = (uint16_t*)malloc(g_proc_bytes);
        g_preproc  = (uint8_t*) malloc(g_proc_bytes);
        g_comp     = (uint8_t*) malloc(g_nsub * g_bound);
        g_csize    = (int*)     malloc(g_nsub * sizeof(int));
        g_decomp   = (uint8_t*) malloc(g_proc_bytes);
        g_restored = (uint16_t*)malloc(g_proc_bytes);
        if (!g_src||!g_preproc||!g_comp||!g_csize||!g_decomp||!g_restored) {
            fprintf(stderr, "alloc failed\n"); return 1;
        }
        FILE *f = fopen(files[fi], "rb");
        if (!f || fread(g_src, 1, g_proc_bytes, f) != g_proc_bytes) {
            fprintf(stderr, "read failed %s\n", files[fi]); return 1;
        }
        fclose(f);
        g_shift = netzip_find_min(g_src, g_proc_bytes / 2);   // diff_byte 用 (float ドメイン)

        const char *base = strrchr(files[fi], '/');
        base = base ? base + 1 : files[fi];
        printf("--- %s (%zu sub × %zuKB = %.1f MB) shift=%.4f ---\n",
               base, g_nsub, g_sub_bytes/1024, g_proc_bytes/1e6, g_shift);

        for (int mode = 0; mode < PRE_N; mode++) {
            double tp[MAX_ITERS], tc[MAX_ITERS], td[MAX_ITERS], tr[MAX_ITERS];
            size_t comp_bytes = 0;
            for (int it = 0; it < warmup + iters; it++) {
                double a = now_s(); phase_preproc(mode);   double t_pre = now_s() - a;
                a = now_s();        phase_compress();       double t_cmp = now_s() - a;
                a = now_s();        phase_decompress();     double t_dec = now_s() - a;
                a = now_s();        phase_restore(mode);    double t_res = now_s() - a;
                if (it >= warmup) {
                    int j = it - warmup;
                    tp[j]=t_pre*1e3; tc[j]=t_cmp*1e3; td[j]=t_dec*1e3; tr[j]=t_res*1e3;
                }
            }
            for (size_t s = 0; s < g_nsub; s++) comp_bytes += (size_t)g_csize[s];

            // bit-exact 検証 (最終反復の復元結果と元データを memcmp)
            long mism = 0;
            const uint8_t *o = (const uint8_t*)g_src, *r = (const uint8_t*)g_restored;
            for (size_t b = 0; b < g_proc_bytes; b++) if (o[b] != r[b]) mism++;
            int bit_exact = (mism == 0);

            double mp = median(tp, iters), mc = median(tc, iters),
                   md = median(td, iters), mr = median(tr, iters);
            double base_g = (double)g_proc_bytes / 1e9;
            double gp = base_g/(mp/1e3), gc = base_g/(mc/1e3),
                   gd = base_g/(md/1e3), gr = base_g/(mr/1e3);
            double ratio = (double)g_proc_bytes / (double)comp_bytes;

            printf("  %-10s ratio=%.4f  bit_exact=%-3s mism=%ld/%zu  "
                   "pre/cmp/dec/res = %.2f/%.2f/%.2f/%.2f GB/s\n",
                   pre_name[mode], ratio, bit_exact?"YES":"NO", mism, g_proc_bytes,
                   gp, gc, gd, gr);

            fprintf(csv,
                "%s,%s,%s,%ld,%zu,%zu,%.4f,"
                "%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,"
                "%d,%zu,%d,%s,%s\n",
                base, pre_name[mode], bit_exact?"yes":"no", mism,
                g_proc_bytes, comp_bytes, ratio,
                mp, mc, md, mr, gp, gc, gd, gr,
                g_nth, g_sub_bytes, iters, commit, ts);
        }
        printf("\n");
        free(g_src); free(g_preproc); free(g_comp); free(g_csize);
        free(g_decomp); free(g_restored);
    }
    fclose(csv);
    printf("CSV appended -> %s\n", out_csv);
    return 0;
}
