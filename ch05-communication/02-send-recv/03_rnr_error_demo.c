/**
 * RNR (Receiver Not Ready) Error Demo
 *
 * What is an RNR error:
 *   When the sender issues an RDMA Send request, but the receiver has not
 *   called ibv_post_recv() in advance, the NIC cannot find a matching Recv WR
 *   and returns an RNR NAK.
 *
 * This program intentionally triggers an RNR error:
 *   1. Client immediately calls ibv_post_send(), without waiting for Server to post_recv
 *   2. Sets rnr_retry=0, so the QP won't retry and immediately reports an error
 *   3. Captures the IBV_WC_RNR_RETRY_EXC_ERR error
 *
 * How to avoid RNR errors:
 *   - Receiver posts enough recv WRs before sender sends
 *   - Set min_rnr_timer (0~31) to control NAK wait time
 *   - Set rnr_retry (0~7, 7=infinite retry) to control retry count
 *
 * Usage:
 *   Server: ./03_rnr_error_demo server
 *   Client: ./03_rnr_error_demo client <server_ip>
 *
 * Build: gcc -Wall -O2 -g -o 03_rnr_error_demo 03_rnr_error_demo.c -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE     1024
#define TCP_PORT        19877

/* ========== RDMA Resources ========== */
struct rnr_demo_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *mr;
    char                *buf;
    uint8_t              port;
    int                  is_roce;
};

/* ========== Initialize Resources ========== */
static int init_resources(struct rnr_demo_ctx *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices = 0;
    int ret = -1;

    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices\n");
        goto cleanup;
    }

    res->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(res->ctx, "Failed to open device");
    printf("[Info] Device: %s\n", ibv_get_device_name(dev_list[0]));

    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[Info] Transport layer: %s\n", transport_str(transport));

    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate PD");

    res->cq = ibv_create_cq(res->ctx, 64, NULL, NULL, 0);
    CHECK_NULL(res->cq, "Failed to create CQ");

    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "Failed to create QP");

    res->buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(res->buf, "Failed to allocate buffer");
    memset(res->buf, 0, BUFFER_SIZE);

    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->mr, "Failed to register MR");

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/**
 * Custom QP connection - set rnr_retry=0 to quickly trigger error
 *
 * Difference from normal connection:
 *   - rnr_retry = 0: no retry after receiving RNR NAK, immediately reports error
 *   - Normal programs typically set rnr_retry = 7 (infinite retry)
 */
static int qp_connect_rnr_demo(struct rnr_demo_ctx *res,
                               const struct rdma_endpoint *remote)
{
    struct ibv_qp_attr attr;
    int ret;

    /* RESET -> INIT */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.port_num        = res->port;
    attr.pkey_index      = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    ret = ibv_modify_qp(res->qp, &attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    CHECK_ERRNO_RETURN(ret, "QP RESET->INIT");

    /* INIT -> RTR */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = RDMA_DEFAULT_MTU;
    attr.dest_qp_num        = remote->qp_num;
    attr.rq_psn             = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 1;   /* Shortest wait time (0.01ms) */

    attr.ah_attr.dlid     = remote->lid;
    attr.ah_attr.sl       = 0;
    attr.ah_attr.port_num = res->port;

    /* RoCE mode: enable global routing */
    if (res->is_roce) {
        attr.ah_attr.is_global          = 1;
        attr.ah_attr.grh.dgid           = remote->gid;
        attr.ah_attr.grh.sgid_index     = RDMA_DEFAULT_GID_INDEX;
        attr.ah_attr.grh.flow_label     = 0;
        attr.ah_attr.grh.hop_limit      = 64;
        attr.ah_attr.grh.traffic_class  = 0;
    }

    ret = ibv_modify_qp(res->qp, &attr,
                        IBV_QP_STATE | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                        IBV_QP_AV);
    CHECK_ERRNO_RETURN(ret, "QP INIT->RTR");

    /* RTR -> RTS: Key - rnr_retry=0 */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.sq_psn         = RDMA_DEFAULT_PSN;
    attr.timeout        = 14;
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 0;   /* KEY: no RNR retry, immediately reports error */
    attr.max_rd_atomic  = 1;

    printf("\n[Key params] rnr_retry = 0 (no retry), min_rnr_timer = 1 (0.01ms)\n");
    printf("  rnr_retry meaning:\n");
    printf("    0   = No retry, immediately reports IBV_WC_RNR_RETRY_EXC_ERR\n");
    printf("    1-6 = Retry 1-6 times\n");
    printf("    7   = Infinite retry (recommended default)\n\n");

    ret = ibv_modify_qp(res->qp, &attr,
                        IBV_QP_STATE | IBV_QP_SQ_PSN |
                        IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    CHECK_ERRNO_RETURN(ret, "QP RTR->RTS");

    printf("[Info] QP connection complete (rnr_retry=0)\n");
    return 0;
}

/* ========== Client: send immediately, intentionally trigger RNR ========== */
static int client_trigger_rnr(struct rnr_demo_ctx *res)
{
    const char *msg = "RNR Test Message";
    size_t msg_len = strlen(msg) + 1;
    memcpy(res->buf, msg, msg_len);

    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = (uint32_t)msg_len,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 3001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad_wr = NULL;

    printf("[Client] Sending message immediately (Server has not posted recv yet)...\n");
    printf("[Client] This will trigger an RNR error!\n\n");

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] ibv_post_send failed: %d\n", ret);
        return -1;
    }

    /* Wait for completion - should receive RNR error */
    struct ibv_wc wc;
    printf("[Client] Waiting for CQ completion...\n");
    ret = poll_cq_blocking(res->cq, &wc);

    printf("\n[Client] ========== WC Details ==========\n");
    print_wc_detail(&wc);

    if (wc.status == IBV_WC_RNR_RETRY_EXC_ERR) {
        printf("\n[Client] *** Successfully captured RNR error! ***\n");
        printf("[Client] Error code: IBV_WC_RNR_RETRY_EXC_ERR (%d)\n", wc.status);
        printf("[Client] Meaning: Remote Receive Queue is empty, no available Recv WR\n");
        printf("[Client] Cause: Server did not call ibv_post_recv()\n");
        printf("\n[Client] === How to fix RNR errors ===\n");
        printf("  1. Receiver posts enough recv WRs in advance\n");
        printf("  2. Increase rnr_retry value (7=infinite retry)\n");
        printf("  3. Increase min_rnr_timer to give receiver more preparation time\n");
        printf("  4. Use SRQ (Shared Receive Queue) to centrally manage recv WRs\n");
    } else if (wc.status == IBV_WC_SUCCESS) {
        printf("\n[Client] Unexpected: Send succeeded (Server may have already posted recv)\n");
    } else {
        printf("\n[Client] Received other error: %s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
    }

    return 0;
}

/* ========== Cleanup ========== */
static void cleanup_resources(struct rnr_demo_ctx *res)
{
    if (res->mr)  ibv_dereg_mr(res->mr);
    if (res->buf) free(res->buf);
    if (res->qp)  ibv_destroy_qp(res->qp);
    if (res->cq)  ibv_destroy_cq(res->cq);
    if (res->pd)  ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s server|client [server_ip]\n", argv[0]);
        fprintf(stderr, "  Server: %s server\n", argv[0]);
        fprintf(stderr, "  Client: %s client <server_ip>\n", argv[0]);
        return 1;
    }

    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   RNR (Receiver Not Ready) Error Demo        ║\n");
    printf("║   Role: %-37s║\n", is_server ? "Server" : "Client");
    printf("╚══════════════════════════════════════════════╝\n\n");

    struct rnr_demo_ctx res;
    memset(&res, 0, sizeof(res));

    /* 1. Initialize resources */
    if (init_resources(&res) != 0) {
        return 1;
    }

    /* 2. Exchange endpoint info */
    struct rdma_endpoint local_ep, remote_ep;
    memset(&local_ep, 0, sizeof(local_ep));
    memset(&remote_ep, 0, sizeof(remote_ep));

    if (fill_local_endpoint(res.ctx, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &local_ep) != 0) {
        cleanup_resources(&res);
        return 1;
    }

    printf("[Info] TCP info exchange (port %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[Error] TCP info exchange failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] Remote: QP=%u, LID=%u\n", remote_ep.qp_num, remote_ep.lid);

    /* 3. Connect (rnr_retry=0) */
    if (qp_connect_rnr_demo(&res, &remote_ep) != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 4. Role actions */
    if (is_server) {
        /*
         * Server: intentionally does NOT post recv!
         * Normally ibv_post_recv() should be called here,
         * but to demonstrate the RNR error, we do nothing.
         */
        printf("\n[Server] Intentionally NOT calling ibv_post_recv()!\n");
        printf("[Server] Waiting for Client to send (will trigger RNR error)...\n");
        printf("[Server] Waiting 5 seconds...\n");
        sleep(5);
        printf("[Server] Done. Client should have received the RNR error.\n");

        /* Print QP state - QP may have entered ERROR state after RNR error */
        print_qp_state(res.qp);
    } else {
        /* Client: send immediately */
        /* Give Server a moment to ensure QP is connected but hasn't posted recv */
        sleep(1);
        client_trigger_rnr(&res);

        /* Print QP state - QP enters ERROR after RNR */
        printf("\n");
        print_qp_state(res.qp);
    }

    printf("\n[Info] Program finished\n");
    cleanup_resources(&res);
    return 0;
}
