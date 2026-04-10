/**
 * Atomic Operations Example
 * Uses Fetch & Add and Compare & Swap to implement a counter
 *
 * Supports IB/RoCE dual-mode auto-detection:
 *   - IB mode: uses LID addressing
 *   - RoCE mode: uses GID addressing (ah_attr.is_global=1 + GRH)
 *
 * Build: gcc -Wall -O2 -g -o atomic_ops atomic_ops.c \
 *        -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "../../common/rdma_utils.h"

#define BUFFER_SIZE 4096

struct connection_info {
    uint32_t qp_num;
    uint16_t lid;
    union ibv_gid gid;      /* GID (RoCE mode) */
    uint64_t counter_addr;
    uint32_t counter_rkey;
};

struct rdma_ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;         /* Counter MR */
    struct ibv_mr *result_mr;  /* result_buf MR */
    uint64_t *counter;   /* Counter */
    char *result_buf;    /* Storage for atomic operation results */
    int is_roce;         /* IB/RoCE auto-detection */
    union ibv_gid gid;   /* Local GID */
};

int init_ctx(struct rdma_ctx *ctx)
{
    struct ibv_device **list;
    int num;

    list = ibv_get_device_list(&num);
    if (!list) return -1;

    ctx->ctx = ibv_open_device(list[0]);
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    ctx->cq = ibv_create_cq(ctx->ctx, 256, NULL, NULL, 0);

    /* Detect transport layer type: IB or RoCE */
    enum rdma_transport transport = detect_transport(ctx->ctx, 1);
    ctx->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("Transport layer type: %s\n", transport_str(transport));

    /* Query GID in RoCE mode */
    if (ctx->is_roce) {
        ibv_query_gid(ctx->ctx, 1, RDMA_DEFAULT_GID_INDEX, &ctx->gid);
    }

    struct ibv_qp_init_attr attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 128, .max_recv_wr = 128,
                 .max_send_sge = 1, .max_recv_sge = 1 },
    };
    ctx->qp = ibv_create_qp(ctx->pd, &attr);

    /* Allocate counter memory - aligned to cache line (atomic ops require 8-byte alignment, 64 bytes avoids false sharing) */
    if (posix_memalign((void**)&ctx->counter, 64, sizeof(uint64_t)) != 0 ||
        posix_memalign((void**)&ctx->result_buf, 64, BUFFER_SIZE) != 0) {
        perror("posix_memalign");
        return -1;
    }
    *ctx->counter = 0;

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->counter, sizeof(uint64_t),
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
    if (!ctx->mr) {
        perror("ibv_reg_mr (counter)");
        return -1;
    }

    /* Register separate MR for result_buf (atomic operation result written back to local memory) */
    ctx->result_mr = ibv_reg_mr(ctx->pd, ctx->result_buf, sizeof(uint64_t),
                                IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->result_mr) {
        perror("ibv_reg_mr (result_buf)");
        return -1;
    }

    ibv_free_device_list(list);
    return 0;
}

int qp_connect(struct rdma_ctx *ctx, uint32_t remote_qp, uint16_t remote_lid,
               union ibv_gid *remote_gid)
{
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        perror("QP RESET->INIT");
        return -1;
    }

    /* RTR - supports IB/RoCE dual mode */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = remote_qp;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.sl = 0;
    attr.ah_attr.port_num = 1;

    if (ctx->is_roce) {
        /* RoCE mode: use GID addressing */
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = *remote_gid;
        attr.ah_attr.grh.sgid_index = RDMA_DEFAULT_GID_INDEX;
        attr.ah_attr.grh.hop_limit = 64;
        attr.ah_attr.dlid = 0;
    } else {
        /* IB mode: use LID addressing */
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid = remote_lid;
    }

    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                      IBV_QP_AV) != 0) {
        perror("QP INIT->RTR");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(ctx->qp, &attr,
                      IBV_QP_STATE | IBV_QP_SQ_PSN |
                      IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
        perror("QP RTR->RTS");
        return -1;
    }

    return 0;
}

/* Fetch and Add - atomically increment counter */
int atomic_fetch_add(struct rdma_ctx *ctx, uint64_t remote_addr,
                     uint32_t remote_rkey, uint64_t add_value)
{
    /* Result (old value) will be written back to local result_buf */
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->result_buf,
        .length = sizeof(uint64_t),
        .lkey = ctx->result_mr->lkey,
    };

    struct ibv_send_wr wr = {
        .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
        .wr.atomic = {
            .remote_addr = remote_addr,
            .rkey = remote_rkey,
            .compare_add = add_value,
        },
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED,
    };

    struct ibv_send_wr *bad;
    if (ibv_post_send(ctx->qp, &wr, &bad) != 0) {
        perror("ibv_post_send (FAA)");
        return -1;
    }

    /* Wait for completion */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);

    if (wc.status != IBV_WC_SUCCESS) {
        printf("Atomic FAA failed: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }

    /* result_buf contains the old value */
    return 0;
}

/* Compare and Swap - atomic conditional update */
int atomic_cas(struct rdma_ctx *ctx, uint64_t remote_addr,
               uint32_t remote_rkey, uint64_t expected, uint64_t new_value)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->result_buf,
        .length = sizeof(uint64_t),
        .lkey = ctx->result_mr->lkey,
    };

    struct ibv_send_wr wr = {
        .opcode = IBV_WR_ATOMIC_CMP_AND_SWP,
        .wr.atomic = {
            .remote_addr = remote_addr,
            .rkey = remote_rkey,
            .compare_add = expected,  /* Expected value */
            .swap = new_value,        /* New value */
        },
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED,
    };

    struct ibv_send_wr *bad;
    if (ibv_post_send(ctx->qp, &wr, &bad) != 0) {
        perror("ibv_post_send (CAS)");
        return -1;
    }

    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);

    return (wc.status == IBV_WC_SUCCESS) ? 0 : -1;
}

void exchange_info(struct rdma_ctx *ctx, const char *server_ip,
                   struct connection_info *local, struct connection_info *remote)
{
    local->qp_num = ctx->qp->qp_num;
    local->counter_addr = (uint64_t)ctx->counter;
    local->counter_rkey = ctx->mr->rkey;
    local->gid = ctx->gid;

    struct ibv_port_attr attr;
    ibv_query_port(ctx->ctx, 1, &attr);
    local->lid = attr.lid;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr = { .sin_family = AF_INET, .sin_port = htons(7777) };

    if (server_ip) {
        inet_pton(AF_INET, server_ip, &saddr.sin_addr);
        connect(sock, (struct sockaddr*)&saddr, sizeof(saddr));
        send(sock, local, sizeof(*local), 0);
        recv(sock, remote, sizeof(*remote), 0);
    } else {
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        saddr.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (struct sockaddr*)&saddr, sizeof(saddr));
        listen(sock, 1);
        int c = accept(sock, NULL, NULL);
        recv(c, remote, sizeof(*remote), 0);
        send(c, local, sizeof(*local), 0);
        close(c);
    }
    close(sock);
}

int main(int argc, char *argv[])
{
    int is_server = (argc > 1 && strcmp(argv[1], "server") == 0);
    const char *peer_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    struct rdma_ctx ctx = {0};
    if (init_ctx(&ctx) != 0) {
        fprintf(stderr, "init_ctx failed\n");
        return 1;
    }

    struct connection_info local_info = {0}, remote_info = {0};
    exchange_info(&ctx, peer_ip, &local_info, &remote_info);

    if (qp_connect(&ctx, remote_info.qp_num, remote_info.lid, &remote_info.gid) != 0) {
        fprintf(stderr, "QP connect failed\n");
        return 1;
    }
    printf("QP ready (RESET->INIT->RTR->RTS)\n");

    if (is_server) {
        printf("Server: counter at addr=%lu, rkey=0x%x\n",
               (unsigned long)local_info.counter_addr, local_info.counter_rkey);
        printf("Server: initial counter = %lu\n", *ctx.counter);

        /* Wait for client operations */
        sleep(5);
        printf("Server: final counter = %lu\n", *ctx.counter);
    } else {
        printf("Client: performing atomic operations...\n");
        printf("Remote: addr=%lu, rkey=0x%x\n",
               (unsigned long)remote_info.counter_addr, remote_info.counter_rkey);

        sleep(1);

        /* First FAA: +1 */
        if (atomic_fetch_add(&ctx, remote_info.counter_addr, remote_info.counter_rkey, 1) == 0) {
            printf("Client: FAA +1, old value = %lu\n", *(uint64_t*)ctx.result_buf);
        }

        /* Second FAA: +10 */
        if (atomic_fetch_add(&ctx, remote_info.counter_addr, remote_info.counter_rkey, 10) == 0) {
            printf("Client: FAA +10, old value = %lu\n", *(uint64_t*)ctx.result_buf);
        }

        /* CAS: try to change counter from current value (second FAA's old value +10) to 100 */
        uint64_t current = *(uint64_t*)ctx.result_buf + 10;
        if (atomic_cas(&ctx, remote_info.counter_addr, remote_info.counter_rkey, current, 100) == 0) {
            /* result_buf returns the old value before CAS; if old value == expected, swap succeeded */
            int swapped = (*(uint64_t*)ctx.result_buf == current);
            printf("Client: CAS expected=%lu, swapped=%s\n", current, swapped ? "YES" : "NO");
        }
    }

    /* Cleanup resources */
    if (ctx.result_mr) ibv_dereg_mr(ctx.result_mr);
    if (ctx.mr) ibv_dereg_mr(ctx.mr);
    if (ctx.result_buf) free(ctx.result_buf);
    if (ctx.counter) free(ctx.counter);
    if (ctx.qp) ibv_destroy_qp(ctx.qp);
    if (ctx.cq) ibv_destroy_cq(ctx.cq);
    if (ctx.pd) ibv_dealloc_pd(ctx.pd);
    if (ctx.ctx) ibv_close_device(ctx.ctx);

    return 0;
}
