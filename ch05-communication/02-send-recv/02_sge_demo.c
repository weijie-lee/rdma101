/**
 * Scatter-Gather Element (SGE) Demo - Loopback Mode
 *
 * Demonstrates the RDMA Scatter/Gather mechanism:
 *   - Sender: uses 3 SGEs pointing to 3 non-contiguous memory regions (header, payload, trailer)
 *   - NIC automatically "gathers" the 3 data blocks into a single contiguous network packet
 *   - Receiver: uses 1 large SGE to receive, data is "scattered" into the receive buffer
 *   - Verifies that received data == concatenation of header + payload + trailer
 *
 * How it works:
 *   Gather (sender): NIC reads data from multiple memory blocks in SGE order, concatenating into one RDMA message
 *   Scatter (receiver): NIC writes received data into multiple memory blocks in SGE order
 *
 * This program uses Loopback (QP connects to itself), runs in a single process.
 *
 * Build: gcc -Wall -O2 -g -o 02_sge_demo 02_sge_demo.c -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* Three non-contiguous data regions (spec: header=16, payload=32, trailer=8) */
#define HEADER_SIZE     16
#define PAYLOAD_SIZE    32
#define TRAILER_SIZE    8
#define TOTAL_SIZE      (HEADER_SIZE + PAYLOAD_SIZE + TRAILER_SIZE)

/* Receive buffer size */
#define RECV_BUF_SIZE   512

/* ========== RDMA Resources ========== */
struct sge_demo_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;

    /* Sender: 3 non-contiguous memory blocks */
    char *header;
    char *payload;
    char *trailer;
    struct ibv_mr *header_mr;
    struct ibv_mr *payload_mr;
    struct ibv_mr *trailer_mr;

    /* Receiver: 1 contiguous memory block */
    char *recv_buf;
    struct ibv_mr *recv_mr;

    uint8_t port;
    int is_roce;
};

/* ========== Initialize RDMA Resources ========== */
static int init_resources(struct sge_demo_ctx *res)
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
    printf("[Info] Opened device: %s\n", ibv_get_device_name(dev_list[0]));

    /* Detect transport layer type */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[Info] Transport layer: %s\n", transport_str(transport));

    /* PD */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate PD");

    /* CQ: needs to hold enough completion events for both send + recv */
    res->cq = ibv_create_cq(res->ctx, 64, NULL, NULL, 0);
    CHECK_NULL(res->cq, "Failed to create CQ");

    /* QP: max_send_sge=4, max_recv_sge=4 to support multi-SGE */
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 4;  /* Up to 4 SGEs on sender */
    qp_attr.cap.max_recv_sge = 4;  /* Up to 4 SGEs on receiver */

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "Failed to create QP");
    printf("[Info] QP num=%u, max_send_sge=%d, max_recv_sge=%d\n",
           res->qp->qp_num, qp_attr.cap.max_send_sge, qp_attr.cap.max_recv_sge);

    /* Allocate 3 non-contiguous send memory blocks */
    res->header  = (char *)malloc(HEADER_SIZE);
    res->payload = (char *)malloc(PAYLOAD_SIZE);
    res->trailer = (char *)malloc(TRAILER_SIZE);
    CHECK_NULL(res->header, "Failed to allocate header");
    CHECK_NULL(res->payload, "Failed to allocate payload");
    CHECK_NULL(res->trailer, "Failed to allocate trailer");

    /* Allocate receive buffer */
    res->recv_buf = (char *)malloc(RECV_BUF_SIZE);
    CHECK_NULL(res->recv_buf, "Failed to allocate recv_buf");
    memset(res->recv_buf, 0, RECV_BUF_SIZE);

    /* Register independent MR for each memory block */
    res->header_mr = ibv_reg_mr(res->pd, res->header, HEADER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->header_mr, "Failed to register header MR");

    res->payload_mr = ibv_reg_mr(res->pd, res->payload, PAYLOAD_SIZE,
                                 IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->payload_mr, "Failed to register payload MR");

    res->trailer_mr = ibv_reg_mr(res->pd, res->trailer, TRAILER_SIZE,
                                 IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->trailer_mr, "Failed to register trailer MR");

    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, RECV_BUF_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->recv_mr, "Failed to register recv MR");

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Loopback Connection (QP connects to itself) ========== */
static int setup_loopback(struct sge_demo_ctx *res)
{
    /* Fill local endpoint (peer is also self) */
    struct rdma_endpoint self_ep;
    memset(&self_ep, 0, sizeof(self_ep));
    if (fill_local_endpoint(res->ctx, res->qp, res->port,
                            RDMA_DEFAULT_GID_INDEX, &self_ep) != 0) {
        fprintf(stderr, "[Error] Failed to fill endpoint info\n");
        return -1;
    }

    /* QP state transition: RESET -> INIT -> RTR -> RTS */
    int access = IBV_ACCESS_LOCAL_WRITE;
    int ret = qp_full_connect(res->qp, &self_ep, res->port, res->is_roce, access);
    if (ret != 0) {
        fprintf(stderr, "[Error] Loopback QP connection failed\n");
        return -1;
    }
    printf("[Info] QP Loopback connection complete (RESET->INIT->RTR->RTS)\n");
    return 0;
}

/* ========== Fill Test Data ========== */
static void fill_test_data(struct sge_demo_ctx *res)
{
    /* Header: 16 bytes, starts with "HDR:" */
    memset(res->header, 0, HEADER_SIZE);
    snprintf(res->header, HEADER_SIZE, "HDR:HELLO_RDMA!");

    /* Payload: 32 bytes, starts with "PAYLOAD" */
    memset(res->payload, 0, PAYLOAD_SIZE);
    snprintf(res->payload, PAYLOAD_SIZE, "PAYLOAD:SGE-GATHER-DEMO-DATA123");

    /* Trailer: 8 bytes, starts with "END." */
    memset(res->trailer, 0, TRAILER_SIZE);
    snprintf(res->trailer, TRAILER_SIZE, "END.OK!");

    printf("\n[Info] === Sender Data Preparation ===\n");
    printf("  Header  (%3d bytes): addr=%p, content=\"%s\"\n",
           HEADER_SIZE, (void *)res->header, res->header);
    printf("  Payload (%3d bytes): addr=%p, content=\"%s\"\n",
           PAYLOAD_SIZE, (void *)res->payload, res->payload);
    printf("  Trailer (%3d bytes): addr=%p, content=\"%s\"\n",
           TRAILER_SIZE, (void *)res->trailer, res->trailer);
    printf("  Total: %d bytes (3 non-contiguous memory blocks)\n\n", TOTAL_SIZE);
}

/* ========== Execute Multi-SGE Send/Recv ========== */
static int do_sge_send_recv(struct sge_demo_ctx *res)
{
    int ret;

    /* ---- Step 1: Receiver posts recv (1 large SGE) ---- */
    printf("[Step 1] Receiver: post recv (1 SGE, %d-byte buffer)\n", RECV_BUF_SIZE);

    struct ibv_sge recv_sge = {
        .addr   = (uint64_t)res->recv_buf,
        .length = RECV_BUF_SIZE,
        .lkey   = res->recv_mr->lkey,
    };

    struct ibv_recv_wr recv_wr;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 100;
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;

    struct ibv_recv_wr *bad_recv_wr = NULL;
    ret = ibv_post_recv(res->qp, &recv_wr, &bad_recv_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] ibv_post_recv failed: %d\n", ret);
        return -1;
    }

    /* ---- Step 2: Sender posts send (3 SGEs) ---- */
    printf("[Step 2] Sender: post send (3 SGEs, gather mode)\n");

    /*
     * Build 3 SGEs pointing to 3 non-contiguous memory blocks.
     * NIC reads data from these 3 memory blocks in order (gather),
     * concatenating into a single RDMA message for transmission.
     */
    struct ibv_sge send_sges[3];

    /* SGE 0: Header */
    send_sges[0].addr   = (uint64_t)res->header;
    send_sges[0].length = (uint32_t)strlen(res->header);  /* Only send actual data */
    send_sges[0].lkey   = res->header_mr->lkey;

    /* SGE 1: Payload */
    send_sges[1].addr   = (uint64_t)res->payload;
    send_sges[1].length = (uint32_t)strlen(res->payload);
    send_sges[1].lkey   = res->payload_mr->lkey;

    /* SGE 2: Trailer */
    send_sges[2].addr   = (uint64_t)res->trailer;
    send_sges[2].length = (uint32_t)strlen(res->trailer);
    send_sges[2].lkey   = res->trailer_mr->lkey;

    /* Print details of each SGE */
    uint32_t total_send_bytes = 0;
    for (int i = 0; i < 3; i++) {
        printf("  SGE[%d]: addr=%p, length=%u, lkey=0x%x\n",
               i, (void *)send_sges[i].addr,
               send_sges[i].length, send_sges[i].lkey);
        total_send_bytes += send_sges[i].length;
    }
    printf("  Total send bytes: %u\n\n", total_send_bytes);

    struct ibv_send_wr send_wr;
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 200;
    send_wr.sg_list    = send_sges;
    send_wr.num_sge    = 3;         /* 3 SGEs! */
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad_send_wr = NULL;
    ret = ibv_post_send(res->qp, &send_wr, &bad_send_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] ibv_post_send failed: %d\n", ret);
        return -1;
    }

    /* ---- Step 3: Wait for send completion ---- */
    printf("[Step 3] Waiting for send completion...\n");
    struct ibv_wc wc;
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;
    printf("  Sender WC:\n");
    print_wc_detail(&wc);
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[Error] Send failed!\n");
        return -1;
    }

    /* ---- Step 4: Wait for recv completion ---- */
    printf("\n[Step 4] Waiting for recv completion...\n");
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;
    printf("  Receiver WC:\n");
    print_wc_detail(&wc);
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[Error] Recv failed!\n");
        return -1;
    }

    printf("\n[Info] Received %u bytes of data\n", wc.byte_len);

    return (int)wc.byte_len;
}

/* ========== Verify Data Integrity ========== */
static int verify_data(struct sge_demo_ctx *res, int recv_len)
{
    /* Build expected concatenation result */
    char expected[RECV_BUF_SIZE];
    memset(expected, 0, sizeof(expected));

    int hdr_len = (int)strlen(res->header);
    int pay_len = (int)strlen(res->payload);
    int trl_len = (int)strlen(res->trailer);
    int expect_total = hdr_len + pay_len + trl_len;

    memcpy(expected, res->header, hdr_len);
    memcpy(expected + hdr_len, res->payload, pay_len);
    memcpy(expected + hdr_len + pay_len, res->trailer, trl_len);

    printf("\n[Verify] === Data Integrity Check ===\n");
    printf("  Expected length: %d bytes\n", expect_total);
    printf("  Actual length: %d bytes\n", recv_len);

    if (recv_len != expect_total) {
        printf("  [FAIL] Length mismatch!\n");
        return -1;
    }

    if (memcmp(res->recv_buf, expected, expect_total) == 0) {
        printf("  [PASS] Data matches perfectly! NIC correctly gathered 3 non-contiguous memory blocks into one message\n");
    } else {
        printf("  [FAIL] Data mismatch!\n");
        printf("  Expected: \"%.*s\"\n", expect_total, expected);
        printf("  Actual: \"%.*s\"\n", recv_len, res->recv_buf);
        return -1;
    }

    /* Show breakdown of received data */
    printf("\n  Receive buffer content breakdown:\n");
    printf("    [0..%d)    Header part:  \"%.*s\"\n",
           hdr_len, hdr_len, res->recv_buf);
    printf("    [%d..%d)  Payload part: \"%.*s\"\n",
           hdr_len, hdr_len + pay_len, pay_len, res->recv_buf + hdr_len);
    printf("    [%d..%d) Trailer part: \"%.*s\"\n",
           hdr_len + pay_len, expect_total, trl_len,
           res->recv_buf + hdr_len + pay_len);

    return 0;
}

/* ========== Cleanup ========== */
static void cleanup_resources(struct sge_demo_ctx *res)
{
    if (res->recv_mr)    ibv_dereg_mr(res->recv_mr);
    if (res->trailer_mr) ibv_dereg_mr(res->trailer_mr);
    if (res->payload_mr) ibv_dereg_mr(res->payload_mr);
    if (res->header_mr)  ibv_dereg_mr(res->header_mr);
    if (res->recv_buf)   free(res->recv_buf);
    if (res->trailer)    free(res->trailer);
    if (res->payload)    free(res->payload);
    if (res->header)     free(res->header);
    if (res->qp)         ibv_destroy_qp(res->qp);
    if (res->cq)         ibv_destroy_cq(res->cq);
    if (res->pd)         ibv_dealloc_pd(res->pd);
    if (res->ctx)        ibv_close_device(res->ctx);
}

/* ========== Main Function ========== */
int main(void)
{
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   Scatter-Gather Element (SGE) Demo - Loopback  ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ Sender: 3 SGEs -> NIC gather -> 1 RDMA message  ║\n");
    printf("║ Receiver: 1 SGE -> NIC scatter -> recv buffer    ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    struct sge_demo_ctx res;
    memset(&res, 0, sizeof(res));

    /* 1. Initialize resources */
    if (init_resources(&res) != 0) {
        fprintf(stderr, "[Error] Failed to initialize resources\n");
        return 1;
    }

    /* 2. Loopback connection */
    if (setup_loopback(&res) != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 3. Fill test data */
    fill_test_data(&res);

    /* 4. Execute SGE Send/Recv */
    int recv_len = do_sge_send_recv(&res);
    if (recv_len < 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 5. Verify data */
    verify_data(&res, recv_len);

    /* 6. Cleanup */
    printf("\n[Info] Program finished\n");
    cleanup_resources(&res);
    return 0;
}
