// rdma_sanity.c — 生 RDMA Write 帯域のサニティテスト (BF3 2ノード, 無圧縮)
// ─────────────────────────────────────────────────────────────────────────────
// 目的: rdma_transport.c (ibverbs RC QP + RDMA Write with Imm) が
//       osu_bw / ib_write_bw と同等の帯域を出すことを確認する。
//
// 合格基準 (既知値, このクラスタの BF3 で確認済みの参照値):
//   - 4MB  warm (バッファ再利用, window=64) : ~24 GB/s  (osu_bw 4MB と一致)
//   - 60MB 級 (LLC に収まらない)            : ~11 GB/s  (osu_bw 60MB / 実E2E baseline と一致)
//
// warm = 同一バッファを再利用 (osu_bw 同様, 小サイズは LLC 常駐になる)
// cold = 各メッセージ前に送信側 LLC を 64MB スイープで追い出す (掃き出しは計時除外)
//
// --hugepages: 送受信バッファを 2MB hugepages で確保して ibv_reg_mr する。
//   優先: mmap(MAP_HUGETLB|MAP_POPULATE)。プール不足等で失敗したら
//   posix_memalign(2MB) + madvise(MADV_HUGEPAGE) (THP) にフォールバック。
//   どれになったかは stdout と CSV の page_mode 列 (hugetlb_2m/thp_madvise/base_4k) に記録。
//   実際のページサイズは /proc/self/smaps (KernelPageSize / AnonHugePages) を表示して確認。
//   仮説検証: 崖 (~24→~11 GB/s) の原因が NIC のアドレス変換キャッシュ (MTT) ミスなら、
//   2MB ページ化 (65.7MB ≈ 32 エントリ) で大サイズが ~24 GB/s に回復するはず。
//
// 出力: stdout 表 + results_rdma_sanity.csv
// 実行: mpirun -np 2 (2ノード, 1rank/node) ./rdma_sanity [--hugepages]
#define _GNU_SOURCE
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>

#include "rdma_transport.h"

#define MAX_BYTES   65667072UL          // 0212_lm_head.pkl.bin 全体と同じ
#define EVICT_BYTES (64UL * 1024 * 1024)
#define PREPOSTED   240                 // 受信側が常に維持する recv WR 数

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint8_t *g_evict;
static void evict_llc(void) {
    // 64MB を read+write でスイープして LLC(32MB) を確実に追い出す
    for (size_t i = 0; i < EVICT_BYTES; i += 64) g_evict[i]++;
#ifdef __aarch64__
    asm volatile("dsb sy" ::: "memory");
#endif
}

static uint8_t pattern(size_t i) { return (uint8_t)((i * 2654435761UL) >> 7); }

// ─── hugepages 確保 ──────────────────────────────────────────────────────────
#define HUGE_2M (2UL * 1024 * 1024)
static const char *g_page_mode = "base_4k";
static size_t g_map_len = 0;            // munmap 用 (hugetlb のときのみ非0)

static void *alloc_xfer_buf(size_t len, int hugepages) {
    if (hugepages) {
        size_t sz = (len + HUGE_2M - 1) & ~(HUGE_2M - 1);
        // MAP_POPULATE: プール不足なら fault 時 SIGBUS ではなく mmap 時 ENOMEM で検出
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
        if (p != MAP_FAILED) { g_page_mode = "hugetlb_2m"; g_map_len = sz; return p; }
        fprintf(stderr, "[hugepages] mmap(MAP_HUGETLB, %zu) 失敗: %s → "
                        "THP (madvise) にフォールバック\n", sz, strerror(errno));
        void *q = NULL;
        if (posix_memalign(&q, HUGE_2M, sz)) return NULL;
        if (madvise(q, sz, MADV_HUGEPAGE) == 0) g_page_mode = "thp_madvise";
        else fprintf(stderr, "[hugepages] madvise(MADV_HUGEPAGE) 失敗: %s "
                             "(page_mode=base_4k のまま)\n", strerror(errno));
        return q;   // THP は madvise 後の page fault で 2MB 化される (この後の fill で touch)
    }
    void *q = NULL;
    if (posix_memalign(&q, 4096, len)) return NULL;
    return q;
}

// バッファが実際にどのページサイズに載ったか /proc/self/smaps から表示
static void print_buf_pagesize(int rank, const void *buf) {
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return;
    char line[256];
    unsigned long a = (unsigned long)(uintptr_t)buf;
    int in = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long l, h;
        if (sscanf(line, "%lx-%lx", &l, &h) == 2) in = (a >= l && a < h);
        else if (in && (!strncmp(line, "KernelPageSize:", 15) ||
                        !strncmp(line, "AnonHugePages:", 14)))
            printf("[rank %d smaps] %s", rank, line);
    }
    fclose(f);
    fflush(stdout);
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 2) {
        if (rank == 0) fprintf(stderr, "ERROR: need exactly 2 ranks\n");
        MPI_Finalize(); return 1;
    }
    int peer = 1 - rank;

    int hugepages = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--hugepages")) hugepages = 1;

    RdmaCtx ctx;
    if (rdma_init(&ctx, MPI_COMM_WORLD, peer)) MPI_Abort(MPI_COMM_WORLD, 1);
    printf("[rank %d] dev=%s port=%d link=%s mtu=%d lid=%d\n",
           rank, ctx.dev_name, ctx.port,
           ctx.link_layer == IBV_LINK_LAYER_ETHERNET ? "RoCE" : "IB",
           128 << ctx.mtu, ctx.lid);
    fflush(stdout);

    uint8_t *buf = (uint8_t*)alloc_xfer_buf(MAX_BYTES, hugepages);
    if (!buf) MPI_Abort(MPI_COMM_WORLD, 1);
    g_evict = (uint8_t*)malloc(EVICT_BYTES);
    if (!g_evict) MPI_Abort(MPI_COMM_WORLD, 1);
    memset(g_evict, 1, EVICT_BYTES);
    // fill = 全ページ touch (THP は madvise 済みなのでここで 2MB ページ化される)
    if (rank == 0) for (size_t i = 0; i < MAX_BYTES; i++) buf[i] = pattern(i);
    else           memset(buf, 0, MAX_BYTES);
    printf("[rank %d] page_mode=%s%s\n", rank, g_page_mode,
           hugepages ? "" : " (--hugepages 未指定)");
    if (hugepages) print_buf_pagesize(rank, buf);

    double tr0 = now_s();
    struct ibv_mr *mr = rdma_reg(&ctx, buf, MAX_BYTES);
    double reg_ms = (now_s() - tr0) * 1e3;
    if (!mr) MPI_Abort(MPI_COMM_WORLD, 1);
    printf("[rank %d] ibv_reg_mr(%lu bytes) = %.3f ms (計時からは除外)\n",
           rank, MAX_BYTES, reg_ms);

    RdmaRemoteBuf remote;
    rdma_exchange_mr(MPI_COMM_WORLD, peer, buf, mr, &remote);

    // 両 rank の page_mode を突き合わせ (CSV には一致時そのまま、不一致時 "mixed")
    char peer_pm[24] = {0}, my_pm[24] = {0}, page_col[24] = {0};
    snprintf(my_pm, sizeof my_pm, "%s", g_page_mode);
    MPI_Sendrecv(my_pm, sizeof my_pm, MPI_CHAR, peer, 12,
                 peer_pm, sizeof peer_pm, MPI_CHAR, peer, 12,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    snprintf(page_col, sizeof page_col, "%s",
             strcmp(my_pm, peer_pm) == 0 ? my_pm : "mixed");

    // 受信側: recv WR を常時 PREPOSTED 件維持する (完了毎に 1 件補充)
    if (rank == 1) rdma_post_recv(&ctx, PREPOSTED);
    MPI_Barrier(MPI_COMM_WORLD);

    static const size_t sizes[] = {
        4194304, 16777216, 33554432, 50331648, 62914560, 65667072
    };
    static const char *size_lbl[] = { "4M", "16M", "32M", "48M", "60M", "65.7M(full)" };
    const int n_sizes = (int)(sizeof sizes / sizeof sizes[0]);

    static const char *SAN_HEADER =
        "msg_size_bytes,mode,window,n_msgs,elapsed_s,gbps,mr_reg_ms,page_mode,expected_ref";
    FILE *csv = NULL;
    if (rank == 1) {
        const char *path = "results_rdma_sanity.csv";
        int need_hdr = 1;
        FILE *t = fopen(path, "r");
        if (t) {
            char first[512] = "";
            if (fgets(first, sizeof first, t)) first[strcspn(first, "\n")] = 0;
            fclose(t);
            if (strcmp(first, SAN_HEADER) == 0) need_hdr = 0;
            else {  // 旧スキーマ → 退避
                char bak[256];
                snprintf(bak, sizeof bak, "%s.old.%ld", path, (long)time(NULL));
                rename(path, bak);
                fprintf(stderr, "note: %s のスキーマが変わったため %s へ退避しました\n", path, bak);
            }
        }
        csv = fopen(path, "a");
        if (csv && need_hdr) fprintf(csv, "%s\n", SAN_HEADER);
        printf("\n=== RDMA Write 帯域サニティ (raw, 無圧縮, page_mode=%s) ===\n", page_col);
        printf("%-14s %-5s %7s %7s %10s %10s   %s\n",
               "size", "mode", "window", "msgs", "sec", "GB/s", "expected");
    }

    int verified = -1;   // 最初の転送後に1回だけ内容検証
    for (int si = 0; si < n_sizes; si++) {
        size_t sz = sizes[si];
        for (int mode = 0; mode < 2; mode++) {            // 0=warm 1=cold
            // warm: osu_bw 同様の window 連続 post。cold: 1件ずつ + 事前 LLC 追い出し。
            int W = (mode == 0) ? (int)(268435456UL / sz) : 1;
            if (W < 1) W = 1;
            if (W > 64) W = 64;
            // 総転送量がほぼ一定 (~3GB) になるよう window 数を決める
            int nwin = (int)(3221225472UL / ((size_t)W * sz));
            if (nwin < 3) nwin = 3;
            if (nwin > 200) nwin = 200;
            int warm_win = 2;
            int total_msgs = (nwin + warm_win) * W;

            MPI_Barrier(MPI_COMM_WORLD);
            double elapsed = 0.0;
            if (rank == 0) {
                for (int w = 0; w < nwin + warm_win; w++) {
                    if (mode == 1) evict_llc();           // 計時除外
                    double t0 = now_s();
                    for (int i = 0; i < W; i++)
                        rdma_post_write_imm(&ctx, buf, mr, remote.addr, remote.rkey,
                                            sz, (uint32_t)sz);
                    rdma_poll_send(&ctx, W);
                    double t1 = now_s();
                    if (w >= warm_win) elapsed += t1 - t0;
                }
            } else {
                for (int m = 0; m < total_msgs; m++) {
                    uint32_t imm;
                    rdma_poll_recv_imm(&ctx, &imm);
                    rdma_post_recv(&ctx, 1);              // 補充
                    if (verified < 0) {                   // 1回だけ内容検証
                        verified = 1;
                        for (size_t i = 0; i < sz; i++)
                            if (buf[i] != pattern(i)) { verified = 0; break; }
                        printf("[rank 1] data verify (%zu bytes): %s\n",
                               sz, verified ? "PASS" : "FAIL");
                        fflush(stdout);
                    }
                }
            }
            MPI_Barrier(MPI_COMM_WORLD);

            // 帯域は送信側で計時 → rank1 に送って出力 (RC の send 完了 = リモート到達確認)
            double gbps = 0.0;
            if (rank == 0) {
                gbps = (double)sz * W * nwin / elapsed / 1e9;
                MPI_Send(&elapsed, 1, MPI_DOUBLE, 1, 10, MPI_COMM_WORLD);
                MPI_Send(&gbps,    1, MPI_DOUBLE, 1, 11, MPI_COMM_WORLD);
            } else {
                MPI_Recv(&elapsed, 1, MPI_DOUBLE, 0, 10, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                MPI_Recv(&gbps,    1, MPI_DOUBLE, 0, 11, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                const char *exp = "";
                if (sz == 4194304 && mode == 0)  exp = "~24 GB/s (osu_bw 4MB)";
                if (sz >= 50331648)              exp = "~11 GB/s (osu_bw 60MB)";
                printf("%-14s %-5s %7d %7d %10.4f %10.2f   %s\n",
                       size_lbl[si], mode ? "cold" : "warm", W, W * nwin,
                       elapsed, gbps, exp);
                fflush(stdout);
                if (csv)
                    fprintf(csv, "%zu,%s,%d,%d,%.6f,%.3f,%.3f,%s,%s\n",
                            sz, mode ? "cold" : "warm", W, W * nwin,
                            elapsed, gbps, reg_ms, page_col, exp);
            }
        }
    }

    if (rank == 1) {
        if (!hugepages) {
            printf("\n判定: 4M/warm が ~24 GB/s かつ 60M 級が ~11 GB/s なら実装は正しい\n");
            printf("      (ib_write_bw -a / osu_bw の値と比較すること)\n");
        } else {
            printf("\n判定 (MTT 仮説): page_mode=hugetlb_2m/thp_madvise で 65.7M が\n");
            printf("      ~24 GB/s に回復 → 崖はアドレス変換の粒度と関係 (RNIC 側の変換負荷と整合的。MTT ミスの直接証拠ではない)。\n");
            printf("      ~11 GB/s のまま → アドレス変換以外 (DRAM 帯域など) の律速を疑う。\n");
        }
        if (csv) { fclose(csv); printf("CSV appended -> results_rdma_sanity.csv\n"); }
    }

    ibv_dereg_mr(mr);
    rdma_destroy(&ctx);
    if (g_map_len) munmap(buf, g_map_len); else free(buf);
    free(g_evict);
    MPI_Finalize();
    return 0;
}
