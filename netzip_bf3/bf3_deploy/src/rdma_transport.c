// rdma_transport.c — rdma_transport.h の実装。詳細はヘッダ参照。
#include "rdma_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    uint8_t  gid[16];
    uint8_t  mtu;
    uint8_t  link_layer;
} ConnInfo;

static int open_device(RdmaCtx *c) {
    int num = 0;
    struct ibv_device **list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "rdma: no RDMA devices found (is rdma-core installed?)\n");
        return -1;
    }
    const char *want = getenv("NETZIP_IB_DEV");
    struct ibv_context *ctx = NULL;
    struct ibv_port_attr pa;
    uint8_t port = 0;

    for (int i = 0; i < num && !ctx; i++) {
        const char *name = ibv_get_device_name(list[i]);
        if (want && strcmp(name, want) != 0) continue;
        struct ibv_context *t = ibv_open_device(list[i]);
        if (!t) continue;
        struct ibv_device_attr da;
        if (ibv_query_device(t, &da) == 0) {
            for (uint8_t p = 1; p <= da.phys_port_cnt; p++) {
                if (ibv_query_port(t, p, &pa) == 0 && pa.state == IBV_PORT_ACTIVE) {
                    ctx = t; port = p;
                    snprintf(c->dev_name, sizeof c->dev_name, "%s", name);
                    break;
                }
            }
        }
        if (!ctx) ibv_close_device(t);
    }
    ibv_free_device_list(list);
    if (!ctx) {
        fprintf(stderr, "rdma: no ACTIVE port found (NETZIP_IB_DEV=%s)\n",
                want ? want : "(any)");
        return -1;
    }
    c->verbs      = ctx;
    c->port       = port;
    c->link_layer = pa.link_layer;
    c->mtu        = pa.active_mtu;
    c->lid        = pa.lid;
    return 0;
}

int rdma_init(RdmaCtx *c, MPI_Comm comm, int peer_rank) {
    memset(c, 0, sizeof *c);
    if (open_device(c)) return -1;
    const char *gi = getenv("NETZIP_GID_INDEX");
    c->gid_index = gi ? atoi(gi) : 3;

    c->pd      = ibv_alloc_pd(c->verbs);
    c->send_cq = ibv_create_cq(c->verbs, RDMA_MAX_SEND_WR * 2, NULL, NULL, 0);
    c->recv_cq = ibv_create_cq(c->verbs, RDMA_MAX_RECV_WR * 2, NULL, NULL, 0);
    if (!c->pd || !c->send_cq || !c->recv_cq) {
        fprintf(stderr, "rdma: pd/cq allocation failed\n"); return -1;
    }

    struct ibv_qp_init_attr qia;
    memset(&qia, 0, sizeof qia);
    qia.send_cq          = c->send_cq;
    qia.recv_cq          = c->recv_cq;
    qia.cap.max_send_wr  = RDMA_MAX_SEND_WR;
    qia.cap.max_recv_wr  = RDMA_MAX_RECV_WR;
    qia.cap.max_send_sge = 1;
    qia.cap.max_recv_sge = 1;
    qia.qp_type          = IBV_QPT_RC;
    c->qp = ibv_create_qp(c->pd, &qia);
    if (!c->qp) { fprintf(stderr, "rdma: ibv_create_qp failed\n"); return -1; }

    struct ibv_qp_attr a;
    memset(&a, 0, sizeof a);
    a.qp_state        = IBV_QPS_INIT;
    a.pkey_index      = 0;
    a.port_num        = c->port;
    a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(c->qp, &a,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "rdma: modify_qp INIT failed\n"); return -1;
    }

    ConnInfo me, rem;
    memset(&me, 0, sizeof me);
    me.qpn        = c->qp->qp_num;
    me.psn        = 0;
    me.lid        = c->lid;
    me.mtu        = (uint8_t)c->mtu;
    me.link_layer = (uint8_t)c->link_layer;
    if (c->link_layer == IBV_LINK_LAYER_ETHERNET) {
        union ibv_gid g;
        if (ibv_query_gid(c->verbs, c->port, c->gid_index, &g)) {
            fprintf(stderr, "rdma: ibv_query_gid(idx=%d) failed\n", c->gid_index);
            return -1;
        }
        memcpy(me.gid, &g, 16);
    }
    MPI_Sendrecv(&me,  sizeof me,  MPI_BYTE, peer_rank, 901,
                 &rem, sizeof rem, MPI_BYTE, peer_rank, 901,
                 comm, MPI_STATUS_IGNORE);

    enum ibv_mtu mtu = (me.mtu < rem.mtu) ? (enum ibv_mtu)me.mtu
                                          : (enum ibv_mtu)rem.mtu;
    struct ibv_qp_attr r;
    memset(&r, 0, sizeof r);
    r.qp_state           = IBV_QPS_RTR;
    r.path_mtu           = mtu;
    r.dest_qp_num        = rem.qpn;
    r.rq_psn             = rem.psn;
    r.max_dest_rd_atomic = 1;
    r.min_rnr_timer      = 12;
    r.ah_attr.port_num   = c->port;
    if (c->link_layer == IBV_LINK_LAYER_ETHERNET) {
        r.ah_attr.is_global      = 1;
        memcpy(&r.ah_attr.grh.dgid, rem.gid, 16);
        r.ah_attr.grh.sgid_index = (uint8_t)c->gid_index;
        r.ah_attr.grh.hop_limit  = 64;
    } else {
        r.ah_attr.is_global = 0;
        r.ah_attr.dlid      = rem.lid;
    }
    if (ibv_modify_qp(c->qp, &r,
            IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "rdma: modify_qp RTR failed\n"); return -1;
    }

    struct ibv_qp_attr s;
    memset(&s, 0, sizeof s);
    s.qp_state      = IBV_QPS_RTS;
    s.timeout       = 14;
    s.retry_cnt     = 7;
    s.rnr_retry     = 7;
    s.sq_psn        = 0;
    s.max_rd_atomic = 1;
    if (ibv_modify_qp(c->qp, &s,
            IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
            IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "rdma: modify_qp RTS failed\n"); return -1;
    }

    // 両側が RTS に達してから戻る (片側だけ先に post しない)
    MPI_Barrier(comm);
    return 0;
}

void rdma_destroy(RdmaCtx *c) {
    if (c->qp)      ibv_destroy_qp(c->qp);
    if (c->send_cq) ibv_destroy_cq(c->send_cq);
    if (c->recv_cq) ibv_destroy_cq(c->recv_cq);
    if (c->pd)      ibv_dealloc_pd(c->pd);
    if (c->verbs)   ibv_close_device(c->verbs);
    memset(c, 0, sizeof *c);
}

struct ibv_mr *rdma_reg(RdmaCtx *c, void *buf, size_t len) {
    struct ibv_mr *mr = ibv_reg_mr(c->pd, buf, len,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) fprintf(stderr, "rdma: ibv_reg_mr(%zu bytes) failed\n", len);
    return mr;
}

int rdma_exchange_mr(MPI_Comm comm, int peer_rank, void *local_addr,
                     struct ibv_mr *local_mr, RdmaRemoteBuf *peer_buf) {
    RdmaRemoteBuf mine;
    mine.addr = (uint64_t)(uintptr_t)local_addr;
    mine.rkey = local_mr ? local_mr->rkey : 0;
    MPI_Sendrecv(&mine,    sizeof mine,      MPI_BYTE, peer_rank, 902,
                 peer_buf, sizeof *peer_buf, MPI_BYTE, peer_rank, 902,
                 comm, MPI_STATUS_IGNORE);
    return 0;
}

int rdma_post_write_imm(RdmaCtx *c, const void *local_buf, struct ibv_mr *mr,
                        uint64_t remote_addr, uint32_t rkey,
                        size_t len, uint32_t imm_data) {
    struct ibv_sge sge;
    sge.addr   = (uint64_t)(uintptr_t)local_buf;
    sge.length = (uint32_t)len;
    sge.lkey   = mr ? mr->lkey : 0;

    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof wr);
    wr.opcode             = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags         = IBV_SEND_SIGNALED;
    wr.imm_data           = imm_data;   // 両ノード同一エンディアン (aarch64 LE) 前提で生値を使用
    wr.sg_list            = &sge;
    wr.num_sge            = len ? 1 : 0;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;
    int rc = ibv_post_send(c->qp, &wr, &bad);
    if (rc) { fprintf(stderr, "rdma: ibv_post_send failed (rc=%d)\n", rc); return -1; }
    return 0;
}

int rdma_post_write(RdmaCtx *c, const void *local_buf, struct ibv_mr *mr,
                    uint64_t remote_addr, uint32_t rkey,
                    size_t len, int signaled) {
    struct ibv_sge sge;
    sge.addr   = (uint64_t)(uintptr_t)local_buf;
    sge.length = (uint32_t)len;
    sge.lkey   = mr ? mr->lkey : 0;

    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof wr);
    wr.opcode              = IBV_WR_RDMA_WRITE;
    wr.send_flags          = signaled ? IBV_SEND_SIGNALED : 0;
    wr.sg_list             = &sge;
    wr.num_sge             = len ? 1 : 0;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;
    int rc = ibv_post_send(c->qp, &wr, &bad);
    if (rc) { fprintf(stderr, "rdma: ibv_post_send(write) failed (rc=%d)\n", rc); return -1; }
    return 0;
}

int rdma_post_recv(RdmaCtx *c, int n) {
    for (int i = 0; i < n; i++) {
        struct ibv_recv_wr wr, *bad = NULL;
        memset(&wr, 0, sizeof wr);
        wr.wr_id   = (uint64_t)i;
        wr.sg_list = NULL;
        wr.num_sge = 0;
        int rc = ibv_post_recv(c->qp, &wr, &bad);
        if (rc) { fprintf(stderr, "rdma: ibv_post_recv failed (rc=%d)\n", rc); return -1; }
    }
    return 0;
}

int rdma_poll_send(RdmaCtx *c, int n) {
    struct ibv_wc wc[16];
    int got = 0;
    while (got < n) {
        int k = ibv_poll_cq(c->send_cq, 16, wc);
        if (k < 0) { fprintf(stderr, "rdma: poll send_cq failed\n"); return -1; }
        for (int i = 0; i < k; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "rdma: send wc error: %s\n",
                        ibv_wc_status_str(wc[i].status));
                return -1;
            }
        }
        got += k;
    }
    return 0;
}

int rdma_poll_recv_imm(RdmaCtx *c, uint32_t *imm_out) {
    struct ibv_wc wc;
    int k;
    while ((k = ibv_poll_cq(c->recv_cq, 1, &wc)) == 0) { /* busy poll */ }
    if (k < 0) { fprintf(stderr, "rdma: poll recv_cq failed\n"); return -1; }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "rdma: recv wc error: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }
    if (wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM) {
        fprintf(stderr, "rdma: unexpected recv opcode %d\n", (int)wc.opcode);
        return -1;
    }
    if (imm_out) *imm_out = wc.imm_data;
    return 0;
}
