// rdma_transport.h — 2ランクMPIジョブ用の最小 RC-QP RDMA-Write(+imm) トランスポート
// ─────────────────────────────────────────────────────────────────────────────
// データパスは純 ibverbs (libibverbs)。MPI は接続確立 (QPN/PSN/LID/GID/MTU と
// rkey/addr の交換) と同期にのみ使用する。
//
//   送信側: rdma_post_write_imm() → rdma_poll_send()   (post と completion を分離計時可能)
//   受信側: rdma_post_recv() を事前に発行 → rdma_poll_recv_imm() で imm (=wire bytes) を取得
//
// 環境変数:
//   NETZIP_IB_DEV    使用する HCA 名 (例: mlx5_0)。未指定なら ACTIVE ポートを持つ最初のデバイス。
//   NETZIP_GID_INDEX RoCE (Ethernet link layer) の場合の GID index [default 3]。IB では未使用。
#ifndef RDMA_TRANSPORT_H
#define RDMA_TRANSPORT_H

#include <infiniband/verbs.h>
#include <mpi.h>
#include <stdint.h>
#include <stddef.h>

#define RDMA_MAX_SEND_WR  256
#define RDMA_MAX_RECV_WR  256

typedef struct {
    struct ibv_context *verbs;
    struct ibv_pd      *pd;
    struct ibv_cq      *send_cq;
    struct ibv_cq      *recv_cq;
    struct ibv_qp      *qp;
    uint8_t             port;
    int                 link_layer;   // IBV_LINK_LAYER_INFINIBAND / _ETHERNET
    int                 gid_index;
    uint16_t            lid;
    enum ibv_mtu        mtu;
    char                dev_name[64];
} RdmaCtx;

typedef struct { uint64_t addr; uint32_t rkey; } RdmaRemoteBuf;

// QP 作成 → INIT → (MPIで接続情報交換) → RTR → RTS。終端で MPI_Barrier。
int  rdma_init(RdmaCtx *c, MPI_Comm comm, int peer_rank);
void rdma_destroy(RdmaCtx *c);

// ibv_reg_mr ラッパ (LOCAL_WRITE|REMOTE_WRITE)。登録時間の計時は呼び出し側で行う。
struct ibv_mr *rdma_reg(RdmaCtx *c, void *buf, size_t len);

// 自分のバッファ (addr,rkey) を送り、相手のものを受け取る (対称呼び出し)。
// 相手に公開しない側は mr=NULL でよい (rkey=0 が送られる)。
int  rdma_exchange_mr(MPI_Comm comm, int peer_rank, void *local_addr,
                      struct ibv_mr *local_mr, RdmaRemoteBuf *peer_buf);

// RDMA Write with Immediate を1件 post する (SIGNALED)。len=0 も可 (制御メッセージ)。
int  rdma_post_write_imm(RdmaCtx *c, const void *local_buf, struct ibv_mr *mr,
                         uint64_t remote_addr, uint32_t rkey,
                         size_t len, uint32_t imm_data);

// imm なしの RDMA Write を1件 post する。signaled=0 なら CQE を生成しない
// (後続の signaled WR の CQE 回収時にキュースロットは回収される)。
// RC の順序保証により、後続 write-with-imm の受信 CQE 観測時には本 write の
// データ着弾が保証される (scatter 転送 + 末尾ヘッダ通知のパターン用)。
int  rdma_post_write(RdmaCtx *c, const void *local_buf, struct ibv_mr *mr,
                     uint64_t remote_addr, uint32_t rkey,
                     size_t len, int signaled);

// 受信 WR (0-length, write-with-imm 受け用) を n 件 post する。
int  rdma_post_recv(RdmaCtx *c, int n);

// send CQ から n 件の完了をビジーポーリングで回収する。
int  rdma_poll_send(RdmaCtx *c, int n);

// recv CQ から 1 件の完了を回収し imm_data を返す (RC なので到着順序は post 順)。
int  rdma_poll_recv_imm(RdmaCtx *c, uint32_t *imm_out);

#endif // RDMA_TRANSPORT_H
