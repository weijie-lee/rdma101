/**
 * Send/Recv Communication Example
 * Complete Client-Server Implementation
 *
 * Supports IB/RoCE dual-mode auto-detection:
 *   - IB mode: uses LID addressing
 *   - RoCE mode: uses GID addressing (ah_attr.is_global=1 + GRH)
 *
 * Build: gcc -Wall -O2 -g -o send_recv send_recv.c \
 *        -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "../../common/rdma_utils.h"

#define BUFFER_SIZE 1024
#define MSG_HELLO 1
#define MSG_DATA 2
#define MSG_DONE 3

/* Message structure */
struct msg_header {
    uint32_t type;
    uint32_t size;
};

/* RDMA resources */
struct rdma_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    uint32_t qp_num;
    uint16_t lid;
    union ibv_gid gid;  /* GID (RoCE mode) */
    int is_roce;        /* IB/RoCE auto-detection result */
};

/* Initialize */
int init_rdma(struct rdma_context *ctx)
{
    struct ibv_device **list;
    struct ibv_qp_init_attr qp_init;
    int num;

    list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "No device\n");
        return -1;
    }

    ctx->ctx = ibv_open_device(list[0]);
    if (!ctx->ctx) {
        perror("open device");
        return -1;
    }

    /* Get local LID */
    struct ibv_port_attr attr;
    ibv_query_port(ctx->ctx, 1, &attr);
    ctx->lid = attr.lid;

    /* Detect transport layer type: IB or RoCE */
    enum rdma_transport transport = detect_transport(ctx->ctx, 1);
    ctx->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("Transport layer type: %s\n", transport_str(transport));

    /* Query GID in RoCE mode */
    if (ctx->is_roce) {
        ibv_query_gid(ctx->ctx, 1, RDMA_DEFAULT_GID_INDEX, &ctx->gid);
    }

    ctx->pd = ibv_alloc_pd(ctx->ctx);
    ctx->cq = ibv_create_cq(ctx->ctx, 256, NULL, NULL, 0);

    memset(&qp_init, 0, sizeof(qp_init));
    qp_init.send_cq = ctx->cq;
    qp_init.recv_cq = ctx->cq;
    qp_init.qp_type = IBV_QPT_RC;
    qp_init.cap.max_send_wr = 128;
    qp_init.cap.max_recv_wr = 128;
    qp_init.cap.max_send_sge = 1;
    qp_init.cap.max_recv_sge = 1;

    ctx->qp = ibv_create_qp(ctx->pd, &qp_init);
    ctx->qp_num = ctx->qp->qp_num;

    ctx->buf = malloc(BUFFER_SIZE);
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);

    ibv_free_device_list(list);
    return 0;
}

/* Modify QP state */
int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE,
    };
    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                         IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qp, uint16_t remote_lid,
                     union ibv_gid *remote_gid, int is_roce)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = remote_qp;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.sl = 0;
    attr.ah_attr.port_num = 1;

    if (is_roce) {
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

    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                         IBV_QP_AV);
}

int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = 0,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .max_rd_atomic = 1,
    };
    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_SQ_PSN |
                         IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
}

/* Exchange info (via socket)
 * server_ip == NULL means this side is server
 * Uses rdma_endpoint structure to exchange complete connection info (including GID)
 */
void exchange_info(const char *server_ip, uint32_t local_qp, uint16_t local_lid,
                   union ibv_gid *local_gid,
                   uint32_t *remote_qp, uint16_t *remote_lid,
                   union ibv_gid *remote_gid)
{
    /* Pack data for send/receive */
    struct {
        uint32_t qp_num;
        uint16_t lid;
        union ibv_gid gid;
    } local_data, remote_data;

    local_data.qp_num = local_qp;
    local_data.lid = local_lid;
    if (local_gid)
        local_data.gid = *local_gid;
    else
        memset(&local_data.gid, 0, sizeof(local_data.gid));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    if (server_ip) {
        /* Client: connect and exchange */
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9999);
        if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
            fprintf(stderr, "Invalid server IP\n");
            close(sock);
            return;
        }
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(sock);
            return;
        }

        send(sock, &local_data, sizeof(local_data), 0);
        recv(sock, &remote_data, sizeof(remote_data), 0);
    } else {
        /* Server: listen and exchange */
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9999);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind");
            close(sock);
            return;
        }
        if (listen(sock, 1) < 0) {
            perror("listen");
            close(sock);
            return;
        }

        int client = accept(sock, NULL, NULL);
        if (client < 0) {
            perror("accept");
            close(sock);
            return;
        }

        recv(client, &remote_data, sizeof(remote_data), 0);
        send(client, &local_data, sizeof(local_data), 0);

        close(client);
    }
    close(sock);

    *remote_qp = remote_data.qp_num;
    *remote_lid = remote_data.lid;
    if (remote_gid)
        *remote_gid = remote_data.gid;
}

/* Send message */
int send_message(struct rdma_context *ctx, void *data, size_t len, int opcode)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)data,
        .length = len,
        .lkey = ctx->mr->lkey,
    };

    struct ibv_send_wr wr = {
        .wr_id = opcode,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = opcode == MSG_DONE ? IBV_WR_SEND_WITH_IMM : IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };

    if (opcode == MSG_DONE) {
        wr.imm_data = htobe32(opcode);
    }

    struct ibv_send_wr *bad;
    if (ibv_post_send(ctx->qp, &wr, &bad)) {
        return -1;
    }

    /* Wait for completion */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);

    return (wc.status == IBV_WC_SUCCESS) ? 0 : -1;
}

/* Receive message */
int recv_message(struct rdma_context *ctx)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->buf,
        .length = BUFFER_SIZE,
        .lkey = ctx->mr->lkey,
    };

    struct ibv_recv_wr wr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
    };

    struct ibv_recv_wr *bad;
    if (ibv_post_recv(ctx->qp, &wr, &bad)) {
        return -1;
    }

    /* Wait for completion */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);

    if (wc.status != IBV_WC_SUCCESS) {
        return -1;
    }

    return wc.byte_len;
}

int main(int argc, char *argv[])
{
    struct rdma_context ctx = {0};
    int is_server = (argc > 1 && strcmp(argv[1], "server") == 0);
    const char *peer_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("Running as %s\n", is_server ? "server" : "client");

    if (init_rdma(&ctx) != 0) {
        fprintf(stderr, "RDMA init failed\n");
        return 1;
    }
    if (modify_qp_to_init(ctx.qp) != 0) {
        fprintf(stderr, "QP INIT failed\n");
        return 1;
    }

    /* Exchange connection info (including GID for RoCE support) */
    uint32_t remote_qp;
    uint16_t remote_lid;
    union ibv_gid remote_gid;
    memset(&remote_gid, 0, sizeof(remote_gid));
    exchange_info(peer_ip, ctx.qp_num, ctx.lid, &ctx.gid,
                  &remote_qp, &remote_lid, &remote_gid);
    printf("Remote: QP=%u, LID=%u\n", remote_qp, remote_lid);

    if (modify_qp_to_rtr(ctx.qp, remote_qp, remote_lid,
                          &remote_gid, ctx.is_roce) != 0) {
        fprintf(stderr, "QP RTR failed\n");
        return 1;
    }
    if (modify_qp_to_rts(ctx.qp) != 0) {
        fprintf(stderr, "QP RTS failed\n");
        return 1;
    }
    printf("QP ready (RESET->INIT->RTR->RTS)\n");

    if (is_server) {
        printf("Server ready, waiting for messages...\n");

        /* Pre-post recv: prepare one recv WR for each of 3 messages */
        for (int i = 0; i < 3; i++) {
            recv_message(&ctx);
        }

        /* Process messages */
        for (int i = 0; i < 3; i++) {
            /* Wait for CQ completion (recv_message internally already posts recv + polls CQ) */
            struct ibv_wc wc;
            while (ibv_poll_cq(ctx.cq, 1, &wc) == 0);

            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Recv WC error: %s\n", ibv_wc_status_str(wc.status));
                break;
            }

            struct msg_header *hdr = (struct msg_header*)ctx.buf;
            printf("Received: type=%u, size=%u, bytes=%u\n",
                   hdr->type, hdr->size, wc.byte_len);

            if (hdr->type == MSG_DONE) break;

            /* Reply ACK (post recv for subsequent messages first, then send) */
            recv_message(&ctx);
            char reply[] = "ACK";
            memcpy(ctx.buf, reply, sizeof(reply));
            send_message(&ctx, ctx.buf, strlen(reply), MSG_DATA);
        }
    } else {
        printf("Client sending messages...\n");

        /* Pre-post recv, ready to receive server's ACK */
        recv_message(&ctx);

        /* Send message */
        struct msg_header msg = {.type = MSG_HELLO, .size = 13};
        memcpy(ctx.buf, &msg, sizeof(msg));
        memcpy(ctx.buf + sizeof(msg), "Hello Server!", 13);
        send_message(&ctx, ctx.buf, sizeof(msg) + 13, MSG_DATA);

        /* Wait for ACK (poll CQ) */
        struct ibv_wc wc;
        while (ibv_poll_cq(ctx.cq, 1, &wc) == 0);
        if (wc.status == IBV_WC_SUCCESS) {
            printf("Got ACK: \"%.*s\"\n", (int)wc.byte_len, ctx.buf);
        }

        /* Send done signal */
        send_message(&ctx, NULL, 0, MSG_DONE);
    }

    printf("Done\n");
    return 0;
}
