// benchmark_e2e_rdma.c (v6) — フェーズ分解計時付き E2E ベンチマーク (ibverbs RDMA / MPI)
// ─────────────────────────────────────────────────────────────────────────────
// 目的: 「単体30 GB/s の圧縮が E2E では何 GB/s 出ているか」を直接測る (論文 Fig 2,7,8)。
//   ※ 30 GB/s は開発初期の lz4+diff_byte (非可逆) の単体値。論文の可逆構成 (byte grouping+LZ4)
//     の送信側圧縮経路は元バイト換算で約 9 GB/s で、E2E はどちらのページ構成でも無圧縮を下回る。
//
//   rank 0 (送信): [preproc+LZ4 (fused, 16T)] → [pack memcpy] → [RDMA Write post] → [CQE]
//   rank 1 (受信): [recv CQE 待ち] → [LZ4伸長+restore (fused, 16T)] → verify
//
// フェーズ分解 (1回の圧縮あり通信):
//   1 preproc      前処理 (--preproc)      (fused 領域 wall をスレッド毎実測時間比で按分)
//   2 compression  LZ4 本体                (同上)
//   3 rdma_post    ibv_post_send 発行までの時間
//   4 completion   post 後 CQE 取得まで = 実転送時間 (overlap=1 では「隠れなかった残り」)
//   5 decompression LZ4 伸長               (fused 領域を按分)
//   6 restore      前処理の逆変換          (同上)
//   7 pack         圧縮サブチャンク → wire バッファへの memcpy (copy/staging)
//   8 total        wall (LLC追い出し時間は除外)。sum(1..7) との差 = unaccounted
//
// 計測プロトコル: warmup + iters (既定 3+10)、各反復前に両ノードで LLC flush (--flush-iter 0 で無効)。
// 圧縮/伸長の中身は単体30 GB/s を出した benchmark_pipeline run_direct と同一手法:
//   256KB サブチャンクを各スレッドが連続ブロック担当、preproc→LZ4 を同一コアで fused 実行。
//
// transport:
//   ibverbs (既定): RC QP + RDMA Write with Immediate。バッファは ibv_reg_mr 事前登録
//                   (登録時間は計時除外、CSV の mr_reg_ms に1回だけ記録)。
//   mpi (フォールバック): MPI_Isend/Irecv。UCX_TLS=rc を明示して実行すること。
//
// 主なオプション:
//   --data PATH        入力 (raw bf16)。省略時は合成データ
//   --msg-size N       転送総バイト数 (既定: ファイルサイズ or 65667072)
//   --chunk-size N     チャンクバイト数 (既定: msg-size = 一括1チャンク)
//   --compress 0|1     圧縮パス (0 = baseline 直送)              [1]
//   --overlap 0|1      1 = 圧縮(c+1) と 転送(c) をオーバーラップ  [0]
//   --cold none|send|recv|both  測定3: 圧縮後/受信後バッファの LLC 追い出し [none]
//   --lockstep 0|1     1 = チャンク毎に受信側の伸長完了 ack を待ってから次チャンクへ。
//                      ノード間オーバーラップが無くなり全フェーズが直列化されるため、
//                      eviction 時間の除外が厳密になる (cold × 複数チャンク時は自動有効)。
//                      ※ cold なし・複数チャンクの旧計測では rank0 圧縮 ∥ rank1 evict が
//                        重なり、evict 合計の除外が過剰で t_total が負になるバグがあった。
//   --transport ibverbs|mpi                                       [ibverbs]
//   --thp 0|1          1 = 送受信・圧縮・伸長の全主要バッファ (src/dst/wire/scratch/tls)
//                      を 2MB 境界 posix_memalign + madvise(MADV_HUGEPAGE) で確保 [0]。
//                      実際に 2MB 化されたかを smaps の AnonHugePages で検出し
//                      CSV の page_mode 列に記録 (thp_madvise / thp_no_2m / base_4k / mixed)。
//                      背景: 4KB ページでは 65.7MB ≈ 16k ページ (MTT エントリ) になり
//                      raw 帯域が 24.7→11 GB/s に落ちる
//                      (THP で崖が消えることを rdma_sanity --hugepages で確認済み)。
//                      崖はアドレス変換の粒度と関係し RNIC 側のアドレス変換負荷と整合的だが、
//                      MTT ミスのカウンタは直接観測していない (キャッシュ溢れの直接証拠ではない)。
//   --zerocopy 0|1     1 = pack 排除 (ゼロコピー)。LZ4 出力を直接 wire スロットの
//                      LZ4_compressBound 固定オフセットへ書く (t_pack=0)。
//                      転送はサブチャンク毎の実バイトのみを個別 RDMA Write し、
//                      最後にサイズ表ヘッダを Write-with-Imm (RC 順序保証で
//                      imm CQE = 全データ着弾)。→ 転送バイトは膨張ゼロ、
//                      膨張するのはリモートスロットの footprint のみ
//                      (CSV の wire_footprint_bytes に記録)。ibverbs 専用。
//   --phase-shift 0|1  1 = 位相ずらし。compress を preproc 全サブ → LZ4 全サブの
//                      非 fused 2 領域に分割し、チャンク c-1 の post を
//                      「c の preproc 完了後」に行う。DMA は LZ4 とだけ重なり、
//                      preproc 中は DMA 非活動 (帯域競合の切り分け用)。
//                      overlap=1 を強制。この場合 t_preproc/t_compress は按分でなく
//                      真の領域 wall。
//   --pre-threads N    非 fused 時の preproc 領域のスレッド数 (LZ4 領域は常に全数)。
//                      N < OMP_NUM_THREADS 指定で非 fused compress を強制 [0=全数]
//   --preproc NAME     diff_byte (float,lossy,既定。誤差は ±1ULP に収まらない) | idiff_byte (整数 diff,lossless)
//                      | byte_group (純並べ替え,lossless) | bit_group (純並べ替え,lossless)。
//                      lossless 3 種は verify が memcmp 完全一致 (EXACT) を要求する。
//   --iters N / --warmup N                                        [10 / 3]
//   --out CSV                                                     [results_e2e_breakdown.csv]
//
// CSV 行: 反復毎の raw 行 (stat=raw) + 列毎の median/min/max 行。
// ─────────────────────────────────────────────────────────────────────────────
#define _GNU_SOURCE
#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>

#include "netzip.h"
#include "lz4.h"
#ifndef NO_IBVERBS
#include "rdma_transport.h"
#endif

#define DEFAULT_MSG   65667072UL     // 0212_lm_head.pkl.bin 全体
#define EVICT_BYTES   (64UL * 1024 * 1024)
#define MAX_CHUNKS    256
#define MAX_SUBS      512
#define MAX_THREADS   64
#define MAX_ITERS     64

typedef struct {
    const char *data_path;
    const char *out_csv;
    size_t msg, chunk, sub;
    int    compress, overlap, iters, warmup, flush_iter;
    int    cold_send, cold_recv, lockstep, thp;
    int    zerocopy, phase_shift, pre_threads;
    // 前処理: diff_byte=float lossy (誤差は ±1ULP に収まらない); 他 3 つは lossless (memcmp 完全一致)。
    // diff_byte のみ shift(find_min) を使う。
    int    preproc;                 // PRE_DIFF_BYTE / PRE_IDIFF_BYTE / PRE_BYTE_GROUP / PRE_BIT_GROUP
    double rate_gbps;               // 実効リンク帯域の上限 (leaky-bucket pacing); 0=無制限
    const char *sharedlink_out;     // 非NULLなら results_sharedlink 形式の行を追記
    int    use_ib;                  // 1=ibverbs 0=mpi
    float  shift; int shift_auto;
} Cfg;
enum { PRE_DIFF_BYTE = 0, PRE_IDIFF_BYTE, PRE_BYTE_GROUP, PRE_BIT_GROUP };
static const char *e_pre_name[] = { "diff_byte", "idiff_byte", "byte_group", "bit_group" };
#define PRE_IS_LOSSLESS(p)  ((p) != PRE_DIFF_BYTE)   // diff_byte だけ lossy

typedef struct {
    double pre, comp, pack, post, completion, wait_recv, decomp, restore, evict;
} Phases;
#define N_PH 9

static Cfg g_cfg;
static int g_rank, g_nth;
static int g_nchunks, g_nsub_chunk;     // g_nsub_chunk = 先頭(最大)チャンクのサブチャンク数
static size_t g_stride;                 // wire バッファのチャンクスロット間隔
static size_t g_bound;                  // LZ4_compressBound(sub)
static uint16_t *g_src, *g_dst, *g_ref;
static uint8_t  *g_wire, *g_scratch, *g_tls, *g_evbuf;
static uint8_t  *g_prebuf;              // 非fused時の preproc 出力 (n_sub×sub の固定スロット)
static int g_unfused = 0;               // 1 = preproc/LZ4 を別領域で実行
static size_t g_footprint;              // 反復毎: リモートスロットで実際に触れた範囲 (zerocopy 膨張の実測)
static size_t g_wb_chunk[MAX_CHUNKS];   // 各チャンクの wire バイト数 (pacing 用)
static double g_rate_t0;                 // pacing: 反復内の転送開始時刻
static size_t g_rate_sent;               // pacing: 完了済み wire 累積バイト
static float g_shift;
static double g_reg_ms = 0.0;
static int g_decomp_err = 0;

// 前処理 forward / inverse のディスパッチ (4 種)。OMP parallel 内から呼ぶ。
static inline void e_preproc(const uint16_t *in, size_t nelem, uint8_t *out) {
    switch (g_cfg.preproc) {
    case PRE_BYTE_GROUP: netzip_byte_group_serial(in, nelem, out); break;
    case PRE_BIT_GROUP:  netzip_bit_group_serial(in, nelem, out); break;
    case PRE_IDIFF_BYTE: netzip_idiff_byte_group_serial(in, nelem, out); break;
    default:             netzip_diff_byte_group_serial(in, nelem, g_shift, out); break;
    }
}
static inline void e_restore(const uint8_t *in, size_t nelem, uint16_t *out) {
    switch (g_cfg.preproc) {
    case PRE_BYTE_GROUP: netzip_byte_ungroup_serial(in, nelem, out); break;
    case PRE_BIT_GROUP:  netzip_bit_ungroup_serial(in, nelem, out); break;
    case PRE_IDIFF_BYTE: netzip_idiff_byte_ungroup_serial(in, nelem, out); break;
    default:             netzip_diff_byte_ungroup_serial(in, nelem, g_shift, out); break;
    }
}

#ifndef NO_IBVERBS
static RdmaCtx g_rdma;
static struct ibv_mr *g_mr_data, *g_mr_wire;
static RdmaRemoteBuf g_peer_data, g_peer_wire;
#endif
static MPI_Request g_sreq[MAX_CHUNKS], g_rreq[MAX_CHUNKS];

static double g_th_a[MAX_THREADS], g_th_b[MAX_THREADS];

// ─── util ────────────────────────────────────────────────────────────────────
static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y);
}
static double med_of(const double *v, int n) {
    double t[MAX_ITERS]; memcpy(t, v, n * sizeof(double));
    qsort(t, n, sizeof(double), cmp_d);
    return (n & 1) ? t[n/2] : 0.5 * (t[n/2 - 1] + t[n/2]);
}
static double min_of(const double *v, int n) {
    double m = v[0]; for (int i = 1; i < n; i++) if (v[i] < m) m = v[i]; return m;
}
static double max_of(const double *v, int n) {
    double m = v[0]; for (int i = 1; i < n; i++) if (v[i] > m) m = v[i]; return m;
}
static size_t chunk_off(int c) { return (size_t)c * g_cfg.chunk; }
static size_t chunk_sz(int c) {
    size_t o = chunk_off(c);
    return (g_cfg.msg - o < g_cfg.chunk) ? g_cfg.msg - o : g_cfg.chunk;
}
static int n_sub_of(size_t csz) { return (int)((csz + g_cfg.sub - 1) / g_cfg.sub); }

// ─── THP (2MB transparent hugepages) バッファ確保 ───────────────────────────
// rdma_sanity の THP フォールバックと同方式: 2MB 境界 + madvise(MADV_HUGEPAGE)。
// madvise は最初の page fault (この後の memset/load/初回アクセス) より前に行うこと。
#define HUGE_2M (2UL * 1024 * 1024)
static int g_thp_madvise_ok = 1;          // 全バッファで madvise が成功したか
static char g_page_col[24] = "base_4k";   // 両 rank 突き合わせ後の CSV 値

static void *alloc_buf(size_t len) {
    void *p = NULL;
    if (g_cfg.thp) {
        size_t sz = (len + HUGE_2M - 1) & ~(HUGE_2M - 1);
        if (posix_memalign(&p, HUGE_2M, sz)) return NULL;
        if (madvise(p, sz, MADV_HUGEPAGE)) {
            g_thp_madvise_ok = 0;
            fprintf(stderr, "[thp] madvise(MADV_HUGEPAGE, %zu) 失敗: %s\n",
                    sz, strerror(errno));
        }
        return p;
    }
    if (posix_memalign(&p, 4096, len)) return NULL;
    return p;
}

// buf を含むマッピングの AnonHugePages (kB) を /proc/self/smaps から取得 (-1 = 不明)
static long smaps_anon_huge_kb(const void *buf) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;
    char line[256];
    unsigned long a = (unsigned long)(uintptr_t)buf;
    int in = 0; long kb = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long l, h;
        if (sscanf(line, "%lx-%lx", &l, &h) == 2) in = (a >= l && a < h);
        else if (in && !strncmp(line, "AnonHugePages:", 14)) {
            sscanf(line + 14, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

// 主要バッファが touch 済みになった後に呼ぶ: page_mode を実測で決め、両 rank で突き合わせ
static void detect_page_mode(void) {
    const char *mode = "base_4k";
    if (g_cfg.thp) {
        void *data_buf = (g_rank == 0) ? (void*)g_src : (void*)g_dst;
        long kd = smaps_anon_huge_kb(data_buf);
        long kw = smaps_anon_huge_kb(g_wire);
        mode = (g_thp_madvise_ok && kd > 0 && kw > 0) ? "thp_madvise" : "thp_no_2m";
        printf("[rank %d] thp: AnonHugePages data=%ld kB wire=%ld kB -> page_mode=%s\n",
               g_rank, kd, kw, mode);
        fflush(stdout);
    }
    char mine[24] = {0}, theirs[24] = {0};
    snprintf(mine, sizeof mine, "%s", mode);
    MPI_Sendrecv(mine,   sizeof mine,   MPI_CHAR, 1 - g_rank, 57,
                 theirs, sizeof theirs, MPI_CHAR, 1 - g_rank, 57,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    snprintf(g_page_col, sizeof g_page_col, "%s",
             strcmp(mine, theirs) == 0 ? mine : "mixed");
}

// LLC(32MB) 追い出し: 64MB を read+write でスイープ。経過秒を返す (計時からは除外する)。
static double evict_llc(void) {
    double t0 = now_s();
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < EVICT_BYTES; i += 64) g_evbuf[i]++;
#ifdef __aarch64__
    asm volatile("dsb sy" ::: "memory");
#endif
    return now_s() - t0;
}

static void gen_synth(uint16_t *buf, size_t n) {
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < n; i++) {
        float x = 0.05f * sinf((float)i * 1.0e-4f);
        uint16_t b = fp32_to_bf16(x);
        uint32_t r = (uint32_t)(i * 2654435761u) >> 24;
        buf[i] = (uint16_t)((b & 0xFFF8) | (r & 0x0007));
    }
}
static int load_file(const char *path, void *buf, size_t want) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    if (got < want) {
        fprintf(stderr, "ERROR: %s too small (%zu < %zu)\n", path, got, want);
        return -1;
    }
    return 0;
}

// ─── 圧縮 1 チャンク (fused preproc→LZ4, run_direct 準拠: 連続ブロック割当) ──
// wire layout:
//   通常     : [int32 sizes[n_sub]][packed compressed]   (scratch → pack memcpy)
//   zerocopy : [int32 sizes[n_sub]][sub0 @ 0×bound][sub1 @ 1×bound]...
//              LZ4 が直接 wire スロットへ書く (pack なし)。転送は post 側で
//              実バイトのみを scatter するため wire バイト数は通常と同じ。
// 戻り値 = wire バイト数 (hdr + Σ実圧縮サイズ)。
static size_t compress_chunk(int c, uint8_t *out, Phases *ph) {
    const size_t csz   = chunk_sz(c);
    const int    n_sub = n_sub_of(csz);
    int     *sizes  = (int*)out;
    uint8_t *packed = out + (size_t)n_sub * sizeof(int);
    uint8_t *cdst   = g_cfg.zerocopy ? packed : g_scratch;   // どちらも stride = g_bound
    const uint16_t *cs = g_src + chunk_off(c) / 2;
    static size_t s_off[MAX_SUBS + 1];

    double r0 = now_s();
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        uint8_t *my_pp = g_tls + (size_t)tid * g_cfg.sub;
        double tp = 0, tc = 0, t;
        int per = (n_sub + g_nth - 1) / g_nth;
        int s0 = tid * per, s1 = s0 + per;
        if (s0 > n_sub) s0 = n_sub;
        if (s1 > n_sub) s1 = n_sub;
        for (int s = s0; s < s1; s++) {
            size_t sb_off = (size_t)s * g_cfg.sub;
            size_t sb     = (csz - sb_off < g_cfg.sub) ? csz - sb_off : g_cfg.sub;
            t = now_s();
            e_preproc(cs + sb_off / 2, sb / 2, my_pp);
            tp += now_s() - t;
            t = now_s();
            sizes[s] = LZ4_compress_default((const char*)my_pp,
                            (char*)(cdst + (size_t)s * g_bound),
                            (int)sb, (int)g_bound);
            tc += now_s() - t;
        }
        g_th_a[tid] = tp; g_th_b[tid] = tc;
    }
    double region = now_s() - r0;
    // fused 領域 wall をスレッド毎の preproc/LZ4 実測時間比で按分
    double sa = 0, sb_ = 0;
    for (int t = 0; t < g_nth; t++) { sa += g_th_a[t]; sb_ += g_th_b[t]; }
    if (sa + sb_ > 0) {
        ph->pre  += region * sa  / (sa + sb_);
        ph->comp += region * sb_ / (sa + sb_);
    }

    if (g_cfg.zerocopy) {               // pack 不要 (t_pack = 0)
        size_t total = 0;
        for (int s = 0; s < n_sub; s++) total += (size_t)sizes[s];
        return (size_t)n_sub * sizeof(int) + total;
    }
    double p0 = now_s();
    s_off[0] = 0;
    for (int s = 0; s < n_sub; s++) s_off[s + 1] = s_off[s] + (size_t)sizes[s];
    #pragma omp parallel for schedule(static)
    for (int s = 0; s < n_sub; s++)
        memcpy(packed + s_off[s], g_scratch + (size_t)s * g_bound, (size_t)sizes[s]);
    ph->pack += now_s() - p0;

    return (size_t)n_sub * sizeof(int) + s_off[n_sub];
}

// ─── 非 fused 圧縮 (phase-shift / --pre-threads 用): preproc と LZ4 を別領域で ──
// preproc: 全サブを g_prebuf の固定スロットへ (pre-threads で絞れる)。
// 計時は領域 wall そのもの (按分ではない)。
static void preproc_chunk_unfused(int c, Phases *ph) {
    const size_t csz   = chunk_sz(c);
    const int    n_sub = n_sub_of(csz);
    const uint16_t *cs = g_src + chunk_off(c) / 2;
    int nthr = (g_cfg.pre_threads > 0 && g_cfg.pre_threads < g_nth)
               ? g_cfg.pre_threads : g_nth;
    double r0 = now_s();
    #pragma omp parallel num_threads(nthr)
    {
        int tid = omp_get_thread_num();
        int per = (n_sub + nthr - 1) / nthr;
        int s0 = tid * per, s1 = s0 + per;
        if (s0 > n_sub) s0 = n_sub;
        if (s1 > n_sub) s1 = n_sub;
        for (int s = s0; s < s1; s++) {
            size_t sb_off = (size_t)s * g_cfg.sub;
            size_t sb     = (csz - sb_off < g_cfg.sub) ? csz - sb_off : g_cfg.sub;
            uint8_t *dst = g_prebuf + (size_t)s * g_cfg.sub;
            e_preproc(cs + sb_off / 2, sb / 2, dst);
        }
    }
    ph->pre += now_s() - r0;
}

// LZ4: g_prebuf → wire スロット (zerocopy) / scratch+pack。常に全スレッド。
static size_t lz4_chunk_unfused(int c, uint8_t *out, Phases *ph) {
    const size_t csz   = chunk_sz(c);
    const int    n_sub = n_sub_of(csz);
    int     *sizes  = (int*)out;
    uint8_t *packed = out + (size_t)n_sub * sizeof(int);
    uint8_t *cdst   = g_cfg.zerocopy ? packed : g_scratch;
    static size_t s_off[MAX_SUBS + 1];

    double r0 = now_s();
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int per = (n_sub + g_nth - 1) / g_nth;
        int s0 = tid * per, s1 = s0 + per;
        if (s0 > n_sub) s0 = n_sub;
        if (s1 > n_sub) s1 = n_sub;
        for (int s = s0; s < s1; s++) {
            size_t sb_off = (size_t)s * g_cfg.sub;
            size_t sb     = (csz - sb_off < g_cfg.sub) ? csz - sb_off : g_cfg.sub;
            sizes[s] = LZ4_compress_default(
                            (const char*)(g_prebuf + (size_t)s * g_cfg.sub),
                            (char*)(cdst + (size_t)s * g_bound),
                            (int)sb, (int)g_bound);
        }
    }
    ph->comp += now_s() - r0;

    if (g_cfg.zerocopy) {
        size_t total = 0;
        for (int s = 0; s < n_sub; s++) total += (size_t)sizes[s];
        return (size_t)n_sub * sizeof(int) + total;
    }
    double p0 = now_s();
    s_off[0] = 0;
    for (int s = 0; s < n_sub; s++) s_off[s + 1] = s_off[s] + (size_t)sizes[s];
    #pragma omp parallel for schedule(static)
    for (int s = 0; s < n_sub; s++)
        memcpy(packed + s_off[s], g_scratch + (size_t)s * g_bound, (size_t)sizes[s]);
    ph->pack += now_s() - p0;
    return (size_t)n_sub * sizeof(int) + s_off[n_sub];
}

// fused / 非 fused を設定に応じて切替 (通常パス用)
static size_t compress_chunk_any(int c, uint8_t *out, Phases *ph) {
    if (g_unfused) {
        preproc_chunk_unfused(c, ph);
        return lz4_chunk_unfused(c, out, ph);
    }
    return compress_chunk(c, out, ph);
}

// ─── 伸長 1 チャンク (fused LZ4→restore) ─────────────────────────────────────
static void decompress_chunk(int c, const uint8_t *in, Phases *ph) {
    const size_t csz   = chunk_sz(c);
    const int    n_sub = n_sub_of(csz);
    const int     *sizes  = (const int*)in;
    const uint8_t *packed = in + (size_t)n_sub * sizeof(int);
    uint16_t *cd = g_dst + chunk_off(c) / 2;
    // zerocopy: サブチャンクは bound 固定オフセットに散在。通常: 詰めて連続。
    static size_t s_off[MAX_SUBS + 1];
    if (g_cfg.zerocopy) {
        for (int s = 0; s < n_sub; s++) s_off[s] = (size_t)s * g_bound;
    } else {
        s_off[0] = 0;
        for (int s = 0; s < n_sub; s++) s_off[s + 1] = s_off[s] + (size_t)sizes[s];
    }

    double r0 = now_s();
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        uint8_t *my_dp = g_tls + (size_t)tid * g_cfg.sub;
        double td = 0, tr = 0, t;
        int per = (n_sub + g_nth - 1) / g_nth;
        int s0 = tid * per, s1 = s0 + per;
        if (s0 > n_sub) s0 = n_sub;
        if (s1 > n_sub) s1 = n_sub;
        for (int s = s0; s < s1; s++) {
            size_t sb_off = (size_t)s * g_cfg.sub;
            size_t sb     = (csz - sb_off < g_cfg.sub) ? csz - sb_off : g_cfg.sub;
            t = now_s();
            int rc = LZ4_decompress_safe((const char*)(packed + s_off[s]),
                                         (char*)my_dp, sizes[s], (int)sb);
            td += now_s() - t;
            if (rc != (int)sb) g_decomp_err = 1;
            t = now_s();
            e_restore(my_dp, sb / 2, cd + sb_off / 2);
            tr += now_s() - t;
        }
        g_th_a[tid] = td; g_th_b[tid] = tr;
    }
    double region = now_s() - r0;
    double sa = 0, sb_ = 0;
    for (int t = 0; t < g_nth; t++) { sa += g_th_a[t]; sb_ += g_th_b[t]; }
    if (sa + sb_ > 0) {
        ph->decomp  += region * sa  / (sa + sb_);
        ph->restore += region * sb_ / (sa + sb_);
    }
}

// ─── transport 抽象 ─────────────────────────────────────────────────────────
static void post_send_chunk(int c, const void *buf, size_t len) {
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        if (g_cfg.compress && g_cfg.zerocopy) {
            // ゼロコピー scatter: サブ毎に実バイトのみ write (unsignaled)、
            // 最後にサイズ表ヘッダを write-with-imm (signaled)。RC の順序保証により
            // 受信側の imm CQE 観測時点で全サブのデータ着弾が保証される。
            const size_t csz   = chunk_sz(c);
            const int    n_sub = n_sub_of(csz);
            uint8_t *slot  = g_wire + (size_t)c * g_stride;
            uint64_t raddr = g_peer_wire.addr + (size_t)c * g_stride;
            const int *sizes = (const int*)slot;
            const size_t hdr = (size_t)n_sub * sizeof(int);
            size_t span = hdr;
            for (int s = 0; s < n_sub; s++) {
                if (sizes[s] <= 0) continue;
                if (rdma_post_write(&g_rdma, slot + hdr + (size_t)s * g_bound,
                                    g_mr_wire, raddr + hdr + (size_t)s * g_bound,
                                    g_peer_wire.rkey, (size_t)sizes[s], 0))
                    MPI_Abort(MPI_COMM_WORLD, 1);
                span = hdr + (size_t)s * g_bound + (size_t)sizes[s];
            }
            if (rdma_post_write_imm(&g_rdma, slot, g_mr_wire, raddr,
                                    g_peer_wire.rkey, hdr, (uint32_t)len))
                MPI_Abort(MPI_COMM_WORLD, 1);
            g_footprint += span;
            return;
        }
        uint64_t raddr = g_cfg.compress ? g_peer_wire.addr + (size_t)c * g_stride
                                        : g_peer_data.addr + chunk_off(c);
        uint32_t rkey  = g_cfg.compress ? g_peer_wire.rkey : g_peer_data.rkey;
        struct ibv_mr *mr = g_cfg.compress ? g_mr_wire : g_mr_data;
        if (rdma_post_write_imm(&g_rdma, buf, mr, raddr, rkey, len, (uint32_t)len))
            MPI_Abort(MPI_COMM_WORLD, 1);
        g_footprint += len;
#endif
    } else {
        MPI_Isend(buf, (int)len, MPI_BYTE, 1, 100 + c, MPI_COMM_WORLD, &g_sreq[c]);
        g_footprint += len;
    }
}
static void wait_send_chunk(int c) {
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        if (rdma_poll_send(&g_rdma, 1)) MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    } else {
        MPI_Wait(&g_sreq[c], MPI_STATUS_IGNORE);
    }
}
static void receiver_prepost(void) {
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        if (rdma_post_recv(&g_rdma, g_nchunks)) MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    } else {
        for (int c = 0; c < g_nchunks; c++) {
            void  *buf = g_cfg.compress ? (void*)(g_wire + (size_t)c * g_stride)
                                        : (void*)((uint8_t*)g_dst + chunk_off(c));
            size_t cap = g_cfg.compress ? g_stride : chunk_sz(c);
            MPI_Irecv(buf, (int)cap, MPI_BYTE, 0, 100 + c, MPI_COMM_WORLD, &g_rreq[c]);
        }
    }
}
static size_t wait_recv_chunk(int c) {
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        uint32_t imm = 0;
        if (rdma_poll_recv_imm(&g_rdma, &imm)) MPI_Abort(MPI_COMM_WORLD, 1);
        return (size_t)imm;
#else
        return 0;
#endif
    } else {
        MPI_Status st; int cnt = 0;
        MPI_Wait(&g_rreq[c], &st);
        MPI_Get_count(&st, MPI_BYTE, &cnt);
        return (size_t)cnt;
    }
}

// ─── lockstep ack (受信側: チャンク伸長完了 → 0バイト write-imm / 0バイトMPI) ──
static void wait_ack(void) {            // rank0: チャンク c-1 の伸長完了待ち
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        uint32_t imm = 0;
        if (rdma_poll_recv_imm(&g_rdma, &imm)) MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    } else {
        MPI_Recv(NULL, 0, MPI_BYTE, 1, 99, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
}
static void send_ack(void) {            // rank1: 伸長完了を通知
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        if (rdma_post_write_imm(&g_rdma, NULL, NULL,
                                g_peer_wire.addr, g_peer_wire.rkey, 0, 0))
            MPI_Abort(MPI_COMM_WORLD, 1);
        if (rdma_poll_send(&g_rdma, 1)) MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    } else {
        MPI_Send(NULL, 0, MPI_BYTE, 0, 99, MPI_COMM_WORLD);
    }
}

// ─── レート制限 (低速/共有リンクのエミュレート, Issue #10 補助実験) ──────────
// leaky-bucket: 完了済み wire 累積バイトが target_bw を超えないよう busy-wait で
// 遅延を挿入し、実効リンク帯域を rate_gbps に制限する。chunk 完了 (CQE) の直後に
// 呼び、待ち時間は completion フェーズ (= 転送時間) に計上する。compute がリンクより
// 遅ければ待ちは発生せず compute 律速 (E2E=計算上限)、速ければ待ちが入りリンク律速
// (E2E=ratio×rate) → E2E = min(計算上限, ratio×rate)。
static inline void rate_pace(void) {
    if (g_cfg.rate_gbps <= 0.0) return;
    double deadline = g_rate_t0 + (double)g_rate_sent / (g_cfg.rate_gbps * 1e9);
    while (now_s() < deadline) { /* busy wait */ }
}

// ─── 1 反復 ──────────────────────────────────────────────────────────────────
// 位相ずらし送信: preproc(c) → post(c-1) → LZ4(c) ∥ DMA(c-1) → CQE(c-1) 待ち。
// preproc 実行中は常に DMA 非活動 (直前に CQE 回収済み) — 帯域競合の切り分け用。
static void sender_iter_phase_shift(Phases *ph, size_t *wire) {
    size_t wb_prev = 0;
    double t;
    for (int c = 0; c < g_nchunks; c++) {
        uint8_t *slot = g_wire + (size_t)c * g_stride;
        preproc_chunk_unfused(c, ph);                       // DMA 非活動中
        if (c > 0) {
            t = now_s();
            post_send_chunk(c - 1, g_wire + (size_t)(c-1) * g_stride, wb_prev);
            ph->post += now_s() - t;
        }
        size_t wb = lz4_chunk_unfused(c, slot, ph);         // DMA(c-1) ∥ LZ4(c)
        if (c > 0) {
            t = now_s(); wait_send_chunk(c - 1); ph->completion += now_s() - t;
            *wire += wb_prev;
        }
        wb_prev = wb;
    }
    t = now_s();
    post_send_chunk(g_nchunks - 1, g_wire + (size_t)(g_nchunks-1) * g_stride, wb_prev);
    ph->post += now_s() - t;
    t = now_s(); wait_send_chunk(g_nchunks - 1); ph->completion += now_s() - t;
    *wire += wb_prev;
}

static void sender_iter(Phases *ph, size_t *wire) {
    if (g_cfg.phase_shift && g_cfg.compress) { sender_iter_phase_shift(ph, wire); return; }
    g_rate_t0 = now_s(); g_rate_sent = 0;     // レート制限の基準時刻/累積をリセット
    int pending = -1;   // overlap=1 のとき CQE 未回収のチャンク番号
    for (int c = 0; c < g_nchunks; c++) {
        // lockstep: 受信側がチャンク c-1 を処理し終えるまで次を始めない
        // (待ち時間は rank1 の decomp/restore/evict として既に計上されるため無計時)
        if (g_cfg.lockstep && c > 0) wait_ack();
        const void *sbuf; size_t wb;
        if (g_cfg.compress) {
            uint8_t *slot = g_wire + (size_t)c * g_stride;
            wb   = compress_chunk_any(c, slot, ph);
            sbuf = slot;
        } else {
            wb   = chunk_sz(c);
            sbuf = (const uint8_t*)g_src + chunk_off(c);
        }
        g_wb_chunk[c] = wb;
        if (g_cfg.cold_send) ph->evict += evict_llc();

        double t;
        if (g_cfg.overlap && pending >= 0) {
            t = now_s(); wait_send_chunk(pending);
            g_rate_sent += g_wb_chunk[pending]; rate_pace();   // 低速リンク遅延
            ph->completion += now_s() - t;
            pending = -1;
        }
        t = now_s(); post_send_chunk(c, sbuf, wb); ph->post += now_s() - t;
        if (g_cfg.overlap) {
            pending = c;
        } else {
            t = now_s(); wait_send_chunk(c);
            g_rate_sent += g_wb_chunk[c]; rate_pace();
            ph->completion += now_s() - t;
        }
        *wire += wb;
    }
    if (pending >= 0) {
        double t = now_s(); wait_send_chunk(pending);
        g_rate_sent += g_wb_chunk[pending]; rate_pace();
        ph->completion += now_s() - t;
    }
}
static void receiver_iter(Phases *ph, size_t *wire) {
    for (int c = 0; c < g_nchunks; c++) {
        double t = now_s();
        size_t wb = wait_recv_chunk(c);
        ph->wait_recv += now_s() - t;
        if (g_cfg.cold_recv) ph->evict += evict_llc();
        if (g_cfg.compress) decompress_chunk(c, g_wire + (size_t)c * g_stride, ph);
        if (g_cfg.lockstep && c < g_nchunks - 1) send_ack();
        *wire += wb;
    }
}

// total wall を返す (LLC 追い出し時間は除外済み)。ph は rank1 で両 rank 統合済みになる。
static double run_iter(Phases *ph, size_t *wire) {
    memset(ph, 0, sizeof *ph);
    *wire = 0;
    g_footprint = 0;
    if (g_rank == 1) receiver_prepost();
    else if (g_cfg.lockstep && g_cfg.use_ib && g_nchunks > 1) {
#ifndef NO_IBVERBS
        // lockstep ack (0バイト write-imm) 受け用の recv WR
        if (rdma_post_recv(&g_rdma, g_nchunks - 1)) MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (g_cfg.flush_iter) evict_llc();          // 反復前 flush (両 rank, 計時外)
    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = now_s();
    if (g_rank == 0) sender_iter(ph, wire);
    else             receiver_iter(ph, wire);
    MPI_Barrier(MPI_COMM_WORLD);
    double t1 = now_s();

    // rank0 のフェーズを rank1 に集約
    double v[N_PH] = { ph->pre, ph->comp, ph->pack, ph->post, ph->completion,
                       ph->wait_recv, ph->decomp, ph->restore, ph->evict };
    if (g_rank == 0) {
        unsigned long wf[2] = { (unsigned long)*wire, (unsigned long)g_footprint };
        MPI_Send(v,  N_PH, MPI_DOUBLE,        1, 55, MPI_COMM_WORLD);
        MPI_Send(wf, 2,    MPI_UNSIGNED_LONG, 1, 56, MPI_COMM_WORLD);
    } else {
        double o[N_PH]; unsigned long wf[2];
        MPI_Recv(o,  N_PH, MPI_DOUBLE,        0, 55, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(wf, 2,    MPI_UNSIGNED_LONG, 0, 56, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        ph->pre = o[0]; ph->comp = o[1]; ph->pack = o[2];
        ph->post = o[3]; ph->completion = o[4];
        ph->evict += o[8];
        *wire = (size_t)wf[0];
        g_footprint = (size_t)wf[1];
    }
    return (t1 - t0) - ph->evict;
}

// ─── CSV ─────────────────────────────────────────────────────────────────────
static const char *placement_str(void) {
    if (!g_cfg.compress)                 return "na";
    if (g_cfg.cold_send && g_cfg.cold_recv) return "cold_both";
    if (g_cfg.cold_send)                 return "cold_send";
    if (g_cfg.cold_recv)                 return "cold_recv";
    return "hot";
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
static double safe_gbps(double ms) {
    return ms > 1e-9 ? (double)g_cfg.msg / (ms * 1e-3) / 1e9 : 0.0;
}
static void csv_row(FILE *f, const char *stat, int iter, const double *p,
                    double total_ms, size_t wire, size_t foot, const char *verify,
                    const char *commit, const char *ts) {
    // p[0..8] = pre,comp,pack,post,completion,wait_recv,decomp,restore,evict (ms)
    double sum = p[0] + p[1] + p[2] + p[3] + p[4] + p[6] + p[7];   // wait_recv は重複のため除外
    int eff_pre = (g_cfg.pre_threads > 0 && g_cfg.pre_threads < g_nth)
                  ? g_cfg.pre_threads : g_nth;
    fprintf(f,
        "%s,%s,%zu,%zu,%zu,%zu,%zu,%s,%s,%d,%d,%s,%d,%d,%d,%d,%s,%d,"
        "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
        "%.4f,%.6f,%.3f,%s,%s,%s\n",
        g_cfg.compress ? "lz4" : "none",
        g_cfg.compress ? e_pre_name[g_cfg.preproc] : "none",
        g_cfg.msg, g_cfg.chunk, g_cfg.sub, wire, foot,
        placement_str(),
        g_cfg.use_ib ? "ibverbs" : "mpi",
        g_cfg.overlap, g_cfg.lockstep, g_page_col,
        g_cfg.zerocopy, g_cfg.phase_shift, eff_pre, g_nth, stat, iter,
        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8],
        total_ms, total_ms - sum,
        safe_gbps(p[0]), safe_gbps(p[1]), safe_gbps(p[3] + p[4]),
        safe_gbps(p[6]), safe_gbps(p[7]),
        (double)g_cfg.msg / (total_ms * 1e-3) / 1e9,
        wire ? (double)g_cfg.msg / (double)wire : 0.0,
        (double)g_shift, g_reg_ms, verify, commit, ts);
}

static const char *CSV_HEADER =
    "codec,preproc,msg_size_bytes,chunk_size_bytes,sub_size_bytes,wire_bytes,"
    "wire_footprint_bytes,"
    "buffer_placement,transport,overlap,lockstep,page_mode,"
    "zerocopy,phase_shift,pre_threads,omp_threads,stat,iter,"
    "t_preproc_ms,t_compress_ms,t_pack_ms,t_post_ms,t_completion_ms,"
    "t_wait_recv_ms,t_decompress_ms,t_restore_ms,t_evict_excl_ms,"
    "t_total_ms,t_unaccounted_ms,"
    "gbps_preproc,gbps_compress,gbps_xfer,gbps_decompress,gbps_restore,"
    "throughput_gbps,comp_ratio,shift_val,mr_reg_ms,verify,commit_hash,timestamp";

// 追記オープン。既存ファイルのヘッダが現スキーマと違えば .old.<unixtime> へ退避して作り直す。
static FILE *open_csv_checked(const char *path) {
    int need_hdr = 1;
    FILE *t = fopen(path, "r");
    if (t) {
        char first[1024] = "";
        if (fgets(first, sizeof first, t)) first[strcspn(first, "\n")] = 0;
        fclose(t);
        if (strcmp(first, CSV_HEADER) == 0) {
            need_hdr = 0;
        } else {
            char bak[512];
            snprintf(bak, sizeof bak, "%s.old.%ld", path, (long)time(NULL));
            rename(path, bak);
            fprintf(stderr, "note: %s のスキーマが変わったため %s へ退避しました\n", path, bak);
        }
    }
    FILE *f = fopen(path, "a");
    if (f && need_hdr) fprintf(f, "%s\n", CSV_HEADER);
    return f;
}

// ─── main ────────────────────────────────────────────────────────────────────
static size_t arg_size(const char *s) { return (size_t)strtoull(s, NULL, 10); }

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
    int nproc; MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 2) {
        if (g_rank == 0) fprintf(stderr, "ERROR: need exactly 2 ranks\n");
        MPI_Finalize(); return 1;
    }

    g_cfg = (Cfg){ .data_path = NULL, .out_csv = "results_e2e_breakdown.csv",
                   .msg = 0, .chunk = 0, .sub = 262144,
                   .compress = 1, .overlap = 0, .iters = 10, .warmup = 3,
                   .flush_iter = 1, .cold_send = 0, .cold_recv = 0, .lockstep = 0,
                   .thp = 0, .zerocopy = 0, .phase_shift = 0, .pre_threads = 0,
                   .preproc = PRE_DIFF_BYTE, .rate_gbps = 0.0, .sharedlink_out = NULL,
                   .use_ib = 1, .shift = 0.0f, .shift_auto = 1 };
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--data")       && i+1 < argc) g_cfg.data_path = argv[++i];
        else if (!strcmp(argv[i], "--out")        && i+1 < argc) g_cfg.out_csv   = argv[++i];
        else if (!strcmp(argv[i], "--msg-size")   && i+1 < argc) g_cfg.msg   = arg_size(argv[++i]);
        else if (!strcmp(argv[i], "--chunk-size") && i+1 < argc) g_cfg.chunk = arg_size(argv[++i]);
        else if (!strcmp(argv[i], "--sub-kb")     && i+1 < argc) g_cfg.sub   = arg_size(argv[++i]) * 1024;
        else if (!strcmp(argv[i], "--compress")   && i+1 < argc) g_cfg.compress = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--overlap")    && i+1 < argc) g_cfg.overlap  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lockstep")   && i+1 < argc) g_cfg.lockstep = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--thp")        && i+1 < argc) g_cfg.thp      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zerocopy")   && i+1 < argc) g_cfg.zerocopy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--phase-shift")&& i+1 < argc) g_cfg.phase_shift = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pre-threads")&& i+1 < argc) g_cfg.pre_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preproc")    && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "diff_byte"))  g_cfg.preproc = PRE_DIFF_BYTE;
            else if (!strcmp(m, "idiff_byte")) g_cfg.preproc = PRE_IDIFF_BYTE;
            else if (!strcmp(m, "byte_group")) g_cfg.preproc = PRE_BYTE_GROUP;
            else if (!strcmp(m, "bit_group"))  g_cfg.preproc = PRE_BIT_GROUP;
            else { if (g_rank==0) fprintf(stderr,
                     "unknown --preproc %s (diff_byte|idiff_byte|byte_group|bit_group)\n", m);
                   MPI_Finalize(); return 1; }
        }
        else if (!strcmp(argv[i], "--rate-limit-gbps") && i+1 < argc) g_cfg.rate_gbps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sharedlink-out")  && i+1 < argc) g_cfg.sharedlink_out = argv[++i];
        else if (!strcmp(argv[i], "--iters")      && i+1 < argc) g_cfg.iters    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup")     && i+1 < argc) g_cfg.warmup   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flush-iter") && i+1 < argc) g_cfg.flush_iter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cold")       && i+1 < argc) {
            const char *m = argv[++i];
            g_cfg.cold_send = !strcmp(m, "send") || !strcmp(m, "both");
            g_cfg.cold_recv = !strcmp(m, "recv") || !strcmp(m, "both");
        }
        else if (!strcmp(argv[i], "--transport")  && i+1 < argc)
            g_cfg.use_ib = strcmp(argv[++i], "mpi") != 0;
        else if (!strcmp(argv[i], "--shift")      && i+1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "auto")) { g_cfg.shift = (float)atof(m); g_cfg.shift_auto = 0; }
        }
        else { if (g_rank == 0) fprintf(stderr, "unknown arg: %s\n", argv[i]); MPI_Finalize(); return 1; }
    }
    if (g_cfg.msg == 0) g_cfg.msg = DEFAULT_MSG;
    if (g_cfg.chunk == 0 || g_cfg.chunk > g_cfg.msg) g_cfg.chunk = g_cfg.msg;
    if (g_cfg.msg & 1 || g_cfg.chunk & 1 || g_cfg.sub & 1) {
        if (g_rank == 0) fprintf(stderr, "ERROR: sizes must be even (bf16)\n");
        MPI_Finalize(); return 1;
    }
#ifdef NO_IBVERBS
    if (g_cfg.use_ib) {
        if (g_rank == 0) fprintf(stderr, "ERROR: built with NO_IBVERBS; use --transport mpi\n");
        MPI_Finalize(); return 1;
    }
#endif
    if (g_cfg.iters > MAX_ITERS) g_cfg.iters = MAX_ITERS;

    g_nth = omp_get_max_threads();
    if (g_nth > MAX_THREADS) g_nth = MAX_THREADS;
    omp_set_num_threads(g_nth);

    g_nchunks    = (int)((g_cfg.msg + g_cfg.chunk - 1) / g_cfg.chunk);
    g_nsub_chunk = n_sub_of(chunk_sz(0));
    g_bound      = (size_t)LZ4_compressBound((int)g_cfg.sub);
    g_stride     = ((size_t)g_nsub_chunk * sizeof(int)
                    + (size_t)g_nsub_chunk * g_bound + 4095) & ~(size_t)4095;
    if (g_nchunks > MAX_CHUNKS || g_nsub_chunk > MAX_SUBS) {
        if (g_rank == 0) fprintf(stderr, "ERROR: too many chunks (%d) or subchunks (%d)\n",
                                 g_nchunks, g_nsub_chunk);
        MPI_Finalize(); return 1;
    }
    // bit_group の出力は 16*ceil(nelem/8) バイト。サブチャンクの要素数が 8 の倍数
    // でないと sub_bytes を超えて末尾が転送されず復元が壊れる。各サブチャンクの
    // バイト数が 16 の倍数 (= nelem%8==0) であることを保証する: msg/chunk/sub が全て
    // 16 の倍数なら全サブチャンク (端数含む) が条件を満たす。
    if (g_cfg.compress && g_cfg.preproc == PRE_BIT_GROUP &&
        ((g_cfg.msg % 16) || (g_cfg.chunk % 16) || (g_cfg.sub % 16))) {
        if (g_rank == 0) fprintf(stderr,
            "ERROR: --preproc bit_group は msg/chunk/sub が全て 16 の倍数である必要があります "
            "(msg=%zu chunk=%zu sub=%zu)\n", g_cfg.msg, g_cfg.chunk, g_cfg.sub);
        MPI_Finalize(); return 1;
    }
    // cold × 複数チャンクは lockstep 必須: そうでないと rank0 圧縮 ∥ rank1 evict の
    // ノード間オーバーラップにより evict 合計の除外が過剰になり t_total が負になる。
    if ((g_cfg.cold_send || g_cfg.cold_recv) && g_nchunks > 1 && !g_cfg.lockstep) {
        g_cfg.lockstep = 1;
        if (g_rank == 0)
            fprintf(stderr, "note: cold + multi-chunk -> --lockstep 1 を自動有効化 "
                            "(eviction 除外を厳密にするため)\n");
    }
    if (g_cfg.lockstep && g_cfg.overlap) {
        g_cfg.overlap = 0;
        if (g_rank == 0)
            fprintf(stderr, "note: lockstep と overlap は両立しないため overlap=0 に変更\n");
    }
    // ─── zerocopy / phase-shift / pre-threads の整合 ───
    if (g_cfg.zerocopy && !g_cfg.use_ib) {
        if (g_rank == 0) fprintf(stderr, "ERROR: --zerocopy は ibverbs 専用 "
                                         "(scatter write を使うため)\n");
        MPI_Finalize(); return 1;
    }
    if (g_cfg.phase_shift) {
        if (!g_cfg.compress) {
            g_cfg.phase_shift = 0;
            if (g_rank == 0) fprintf(stderr, "note: compress=0 では phase-shift は無意味のため無効化\n");
        } else if (g_cfg.lockstep || g_cfg.cold_send || g_cfg.cold_recv) {
            if (g_rank == 0) fprintf(stderr, "ERROR: phase-shift は lockstep/cold と併用不可\n");
            MPI_Finalize(); return 1;
        } else if (!g_cfg.overlap) {
            g_cfg.overlap = 1;
            if (g_rank == 0) fprintf(stderr, "note: phase-shift は overlap=1 を強制\n");
        }
    }
    if (g_cfg.pre_threads < 0 || g_cfg.pre_threads > g_nth) g_cfg.pre_threads = 0;
    // 非 fused compress: phase-shift には必須。pre-threads 指定時も
    // 「preproc だけ N スレッド、LZ4 は全スレッド」を実現するため非 fused にする。
    g_unfused = g_cfg.phase_shift ||
                (g_cfg.pre_threads > 0 && g_cfg.pre_threads < g_nth);

    // ─── alloc (--thp 1 なら 2MB 境界 + madvise(MADV_HUGEPAGE), touch より前) ───
    size_t wire_total = (size_t)g_nchunks * g_stride;
    g_wire = (uint8_t*)alloc_buf(wire_total);
    if (!g_wire) MPI_Abort(MPI_COMM_WORLD, 1);
    memset(g_wire, 0, wire_total);
    g_evbuf = (uint8_t*)malloc(EVICT_BYTES);          // eviction ダミー: データパス外
    g_tls   = (uint8_t*)alloc_buf((size_t)g_nth * g_cfg.sub);
    if (!g_evbuf || !g_tls) MPI_Abort(MPI_COMM_WORLD, 1);
    memset(g_evbuf, 1, EVICT_BYTES);

    if (g_rank == 0) {
        g_src     = (uint16_t*)alloc_buf(g_cfg.msg);
        g_scratch = (uint8_t*) alloc_buf((size_t)g_nsub_chunk * g_bound);
        if (!g_src || !g_scratch) MPI_Abort(MPI_COMM_WORLD, 1);
        if (g_unfused) {
            g_prebuf = (uint8_t*)alloc_buf((size_t)g_nsub_chunk * g_cfg.sub);
            if (!g_prebuf) MPI_Abort(MPI_COMM_WORLD, 1);
        }
        if (g_cfg.data_path) {
            if (load_file(g_cfg.data_path, g_src, g_cfg.msg)) MPI_Abort(MPI_COMM_WORLD, 1);
        } else gen_synth(g_src, g_cfg.msg / 2);
    } else {
        g_dst = (uint16_t*)alloc_buf(g_cfg.msg);
        if (!g_dst) MPI_Abort(MPI_COMM_WORLD, 1);
        memset(g_dst, 0, g_cfg.msg);
        g_ref = (uint16_t*)malloc(g_cfg.msg);         // verify 用: データパス外
        if (!g_ref) MPI_Abort(MPI_COMM_WORLD, 1);
        if (g_cfg.data_path) {
            if (load_file(g_cfg.data_path, g_ref, g_cfg.msg)) MPI_Abort(MPI_COMM_WORLD, 1);
        } else gen_synth(g_ref, g_cfg.msg / 2);
    }

    // 主要バッファ touch 済み → 実際のページサイズを smaps で確認し両 rank で突き合わせ
    detect_page_mode();

    // shift: rank0 が使用プレフィクスの min から決定 (run_direct の auto-shift と同じ)
    if (g_cfg.shift_auto) {
        if (g_rank == 0) g_shift = netzip_find_min(g_src, g_cfg.msg / 2);
    } else g_shift = g_cfg.shift;
    MPI_Bcast(&g_shift, 1, MPI_FLOAT, 0, MPI_COMM_WORLD);

    // ─── transport setup (MR 登録時間は別途記録, 計時除外) ───
    if (g_cfg.use_ib) {
#ifndef NO_IBVERBS
        if (rdma_init(&g_rdma, MPI_COMM_WORLD, 1 - g_rank)) MPI_Abort(MPI_COMM_WORLD, 1);
        double t0 = now_s();
        void *data_buf = (g_rank == 0) ? (void*)g_src : (void*)g_dst;
        g_mr_data = rdma_reg(&g_rdma, data_buf, g_cfg.msg);
        g_mr_wire = rdma_reg(&g_rdma, g_wire, wire_total);
        double my_reg = (now_s() - t0) * 1e3;
        if (!g_mr_data || !g_mr_wire) MPI_Abort(MPI_COMM_WORLD, 1);
        MPI_Allreduce(&my_reg, &g_reg_ms, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        rdma_exchange_mr(MPI_COMM_WORLD, 1 - g_rank, data_buf, g_mr_data, &g_peer_data);
        rdma_exchange_mr(MPI_COMM_WORLD, 1 - g_rank, g_wire,   g_mr_wire, &g_peer_wire);
        if (g_rank == 0)
            printf("# rdma: dev=%s link=%s mtu=%d  mr_reg(両rank計)=%.3f ms\n",
                   g_rdma.dev_name,
                   g_rdma.link_layer == IBV_LINK_LAYER_ETHERNET ? "RoCE" : "IB",
                   128 << g_rdma.mtu, g_reg_ms);
#endif
    }

    // ─── warmup + 計測 ───
    static Phases ph_it[MAX_ITERS];
    static double total_it[MAX_ITERS];
    static size_t wire_it[MAX_ITERS], foot_it[MAX_ITERS];
    Phases ph; size_t w;
    for (int i = 0; i < g_cfg.warmup; i++) run_iter(&ph, &w);
    for (int i = 0; i < g_cfg.iters; i++) {
        total_it[i] = run_iter(&ph_it[i], &wire_it[i]);
        foot_it[i]  = g_footprint;
    }

    // ─── verify (rank1, 最終反復の g_dst) ───
    char verify[16] = "NA";
    if (g_rank == 1) {
        if (!g_cfg.compress || PRE_IS_LOSSLESS(g_cfg.preproc)) {
            // 無圧縮 / lossless 前処理 (idiff_byte/byte_group/bit_group) は memcmp 完全一致を要求
            long mism = 0;
            const uint8_t *a = (const uint8_t*)g_dst, *b = (const uint8_t*)g_ref;
            for (size_t i = 0; i < g_cfg.msg; i++) if (a[i] != b[i]) mism++;
            snprintf(verify, sizeof verify, "%s",
                     (!g_decomp_err && mism == 0) ? "EXACT" : "FAIL");
            if (g_decomp_err || mism)
                fprintf(stderr, "VERIFY FAIL (%s): mismatch=%ld/%zu bytes decomp_err=%d\n",
                        g_cfg.compress ? e_pre_name[g_cfg.preproc] : "raw",
                        mism, g_cfg.msg, g_decomp_err);
        } else {
            long mism = 0; double maxerr = 0.0;
            for (size_t i = 0; i < g_cfg.msg / 2; i++)
                if (g_dst[i] != g_ref[i]) {
                    mism++;
                    float e = fabsf(bf16_to_fp32(g_dst[i]) - bf16_to_fp32(g_ref[i]));
                    if (e > maxerr) maxerr = e;
                }
            // diff_byte は非可逆 (bit 一致しない。誤差は ±1ULP に収まらない)。ここは絶対誤差
            // maxerr<1.0 だけを見る緩い判定 (v5 と同じ基準) で、OK は lossless を意味しない
            snprintf(verify, sizeof verify, "%s",
                     g_decomp_err ? "FAIL" : (maxerr < 1.0 ? "OK" : "FAIL"));
            if (g_decomp_err || maxerr >= 1.0)
                fprintf(stderr, "VERIFY FAIL: mism=%ld maxerr=%.4g decomp_err=%d\n",
                        mism, maxerr, g_decomp_err);
        }
    }

    // ─── 出力 (rank1) ───
    if (g_rank == 1) {
        char commit[64], ts[32];
        get_commit(commit, sizeof commit);
        time_t tt = time(NULL);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&tt));

        FILE *f = open_csv_checked(g_cfg.out_csv);
        if (!f) { perror(g_cfg.out_csv); MPI_Abort(MPI_COMM_WORLD, 1); }

        // raw 行 (ms に変換) + 統計行
        double cols[N_PH + 1][MAX_ITERS];     // [0..8]=phases, [9]=total
        for (int i = 0; i < g_cfg.iters; i++) {
            const Phases *p = &ph_it[i];
            double v[N_PH + 1] = { p->pre, p->comp, p->pack, p->post, p->completion,
                                   p->wait_recv, p->decomp, p->restore, p->evict,
                                   total_it[i] };
            for (int k = 0; k <= N_PH; k++) { v[k] *= 1e3; cols[k][i] = v[k]; }
            csv_row(f, "raw", i, v, v[N_PH], wire_it[i], foot_it[i], verify, commit, ts);
        }
        double m[N_PH + 1], lo[N_PH + 1], hi[N_PH + 1];
        for (int k = 0; k <= N_PH; k++) {
            m[k]  = med_of(cols[k], g_cfg.iters);
            lo[k] = min_of(cols[k], g_cfg.iters);
            hi[k] = max_of(cols[k], g_cfg.iters);
        }
        csv_row(f, "median", -1, m,  m[N_PH],  wire_it[0], foot_it[0], verify, commit, ts);
        csv_row(f, "min",    -1, lo, lo[N_PH], wire_it[0], foot_it[0], verify, commit, ts);
        csv_row(f, "max",    -1, hi, hi[N_PH], wire_it[0], foot_it[0], verify, commit, ts);
        fclose(f);

        // stdout サマリ (median)
        double ratio = wire_it[0] ? (double)g_cfg.msg / (double)wire_it[0] : 0.0;
        printf("\n=== E2E breakdown (median of %d, %s, %s, chunk=%zu, %s, overlap=%d, lockstep=%d, page=%s, zc=%d, ps=%d, pre_thr=%d) ===\n",
               g_cfg.iters, g_cfg.compress ? "lz4+diff_byte" : "no-compress",
               g_cfg.use_ib ? "ibverbs" : "mpi", g_cfg.chunk, placement_str(),
               g_cfg.overlap, g_cfg.lockstep, g_page_col,
               g_cfg.zerocopy, g_cfg.phase_shift,
               (g_cfg.pre_threads > 0 && g_cfg.pre_threads < g_nth) ? g_cfg.pre_threads : g_nth);
        printf("data=%s msg=%zu wire=%zu footprint=%zu (x%.3f) ratio=%.3f shift=%.4f verify=%s\n",
               g_cfg.data_path ? g_cfg.data_path : "(synthetic)",
               g_cfg.msg, wire_it[0], foot_it[0],
               wire_it[0] ? (double)foot_it[0] / (double)wire_it[0] : 0.0,
               ratio, (double)g_shift, verify);
        const char *names[] = { "preproc", "compress", "pack(copy)", "rdma_post",
                                "completion", "wait_recv", "decompress", "restore",
                                "evict(excl)" };
        for (int k = 0; k < N_PH; k++)
            printf("  %-12s %9.3f ms   %7.2f GB/s\n", names[k], m[k], safe_gbps(m[k]));
        double sum = m[0] + m[1] + m[2] + m[3] + m[4] + m[6] + m[7];
        printf("  %-12s %9.3f ms   %7.2f GB/s   (min %.3f / max %.3f ms)\n",
               "TOTAL", m[N_PH], safe_gbps(m[N_PH]), lo[N_PH], hi[N_PH]);
        printf("  %-12s %9.3f ms   (total - sum(phases); wait_recv除く)\n",
               "unaccounted", m[N_PH] - sum);
        printf("CSV appended -> %s\n", g_cfg.out_csv);

        // ─── Issue #10 共有リンク実験: results_sharedlink.csv 形式の 1 行 ───
        if (g_cfg.sharedlink_out) {
            // 実効リンク帯域 = wire バイト / 転送時間 (post+completion) の median。
            // レート制限時は ≈ target_bw、compute 律速のときは target を下回る。
            double mbw[MAX_ITERS];
            for (int i = 0; i < g_cfg.iters; i++) {
                double tx = (cols[3][i] + cols[4][i]) * 1e-3;   // post+completion [s]
                mbw[i] = (tx > 0.0) ? (double)wire_it[i] / tx / 1e9 : 0.0;
            }
            double measured_bw = med_of(mbw, g_cfg.iters);
            double e2e_gbps = safe_gbps(m[N_PH]);
            int lossless = (!g_cfg.compress) || PRE_IS_LOSSLESS(g_cfg.preproc);

            int sl_hdr = 0;
            FILE *t = fopen(g_cfg.sharedlink_out, "r");
            if (!t) sl_hdr = 1; else fclose(t);
            FILE *sl = fopen(g_cfg.sharedlink_out, "a");
            if (sl) {
                if (sl_hdr) fprintf(sl,
                    "method,target_bw_gbps,measured_bw_gbps,codec,preproc,lossless,"
                    "e2e_gbps,comp_ratio,verify,commit_hash,timestamp\n");
                fprintf(sl, "%s,%.3f,%.4f,%s,%s,%s,%.4f,%.4f,%s,%s,%s\n",
                        g_cfg.rate_gbps > 0.0 ? "ratelimit" : "unlimited",
                        g_cfg.rate_gbps, measured_bw,
                        g_cfg.compress ? "lz4" : "none",
                        g_cfg.compress ? e_pre_name[g_cfg.preproc] : "none",
                        lossless ? "yes" : "no",
                        e2e_gbps, ratio, verify, commit, ts);
                fclose(sl);
                printf("sharedlink row -> %s (target=%.1f measured=%.2f e2e=%.2f GB/s)\n",
                       g_cfg.sharedlink_out, g_cfg.rate_gbps, measured_bw, e2e_gbps);
            }
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
#ifndef NO_IBVERBS
    if (g_cfg.use_ib) {
        if (g_mr_data) ibv_dereg_mr(g_mr_data);
        if (g_mr_wire) ibv_dereg_mr(g_mr_wire);
        rdma_destroy(&g_rdma);
    }
#endif
    free(g_src); free(g_dst); free(g_ref);
    free(g_wire); free(g_scratch); free(g_tls); free(g_evbuf); free(g_prebuf);
    MPI_Finalize();
    return 0;
}
