/**
 * Shared Receive Queue (SRQ) Demo
 *
 * Demonstrates SRQ core features:
 *   - Creates 1 SRQ shared by 2 QPs for receive buffers
 *   - Posts recv WRs to SRQ (instead of each QP individually)
 *   - 2 QPs send messages to each other, SRQ auto-allocates recv buffers
 *
 * Principle:
 *   Without SRQ, each QP needs independent recv buffers:
 *     N QPs x M buffers = N*M recv WRs
 *
 *   With SRQ, all QPs share one recv pool:
 *     1 SRQ x M buffers = M recv WRs
 *
 *   This saves significant memory in scenarios with many connections (thousands of QPs).
 *   Example: 1000 QPs, each needing 128 recv buffers
 *     - Without SRQ: 1000 x 128 = 128000 recv WRs
 *     - With SRQ:    1 x 1000   = 1000 recv WRs (enough for concurrency)
 *
 * Build:
 *   gcc -o srq_demo srq_demo.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * Run: ./srq_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constant Definitions ========== */
#define NUM_QPS         2           /* Number of QPs */
#define MSG_SIZE        128         /* Message size */
#define BUFFER_SIZE     4096        /* Total buffer size */
#define CQ_DEPTH        64          /* CQ depth */
#define SRQ_MAX_WR      32          /* SRQ maximum WR count */
#define MAX_SEND_WR     32          /* Each QP's SQ maximum WR count */

/* ========== SRQ Demo Resource Structure ========== */
struct srq_resources {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_srq      *srq;           /* Shared Receive Queue */
    struct ibv_cq       *send_cq;       /* Send completion queue */
    struct ibv_cq       *recv_cq;       /* Receive completion queue (shared) */
    struct ibv_qp       *qp[NUM_QPS];   /* 2 QPs sharing the same SRQ */
    struct ibv_mr       *send_mr;
    struct ibv_mr       *recv_mr;
    char                *send_buf;
    char                *recv_buf;
    int                  is_roce;
};

/* ========== Initialize All Resources ========== */
static int init_resources(struct srq_resources *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices;
    int ret, i;

    memset(res, 0, sizeof(*res));

    /* 1. Open device */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        return -1;
    }

    res->ctx = ibv_open_device(dev_list[0]);
    if (!res->ctx) {
        fprintf(stderr, "[Error] Failed to open device\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("[1] Opened device: %s\n", ibv_get_device_name(dev_list[0]));
    ibv_free_device_list(dev_list);

    /* Auto-detect transport layer */
    res->is_roce = (detect_transport(res->ctx, RDMA_DEFAULT_PORT_NUM)
                    == RDMA_TRANSPORT_ROCE);
    printf("    Transport: %s\n", res->is_roce ? "RoCE" : "IB");

    /* 2. Allocate PD */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate PD");
    printf("[2] Allocated PD\n");

    /* 3. Create CQ */
    res->send_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->send_cq, "Failed to create send_cq");

    res->recv_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->recv_cq, "Failed to create recv_cq");
    printf("[3] Created CQ (send_cq + recv_cq)\n");

    /* 4. Create SRQ (key step!) */
    struct ibv_srq_init_attr srq_attr;
    memset(&srq_attr, 0, sizeof(srq_attr));
    srq_attr.attr.max_wr  = SRQ_MAX_WR;    /* Max recv WRs the SRQ can hold */
    srq_attr.attr.max_sge = 1;              /* SGE count per recv WR */

    res->srq = ibv_create_srq(res->pd, &srq_attr);
    CHECK_NULL(res->srq, "Failed to create SRQ");
    printf("[4] Created SRQ (max_wr=%d, max_sge=%d)\n",
           SRQ_MAX_WR, 1);
    printf("    All QPs will share this single receive queue\n");

    /* 5. Allocate and register memory */
    res->send_buf = malloc(BUFFER_SIZE);
    res->recv_buf = malloc(BUFFER_SIZE);
    CHECK_NULL(res->send_buf, "Failed to allocate send_buf");
    CHECK_NULL(res->recv_buf, "Failed to allocate recv_buf");
    memset(res->send_buf, 0, BUFFER_SIZE);
    memset(res->recv_buf, 0, BUFFER_SIZE);

    res->send_mr = ibv_reg_mr(res->pd, res->send_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->send_mr, "Failed to register send_mr");

    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->recv_mr, "Failed to register recv_mr");
    printf("[5] Allocated and registered memory (send + recv)\n");

    /* 6. Create 2 QPs, both using the same SRQ */
    for (i = 0; i < NUM_QPS; i++) {
        struct ibv_qp_init_attr qp_attr;
        memset(&qp_attr, 0, sizeof(qp_attr));
        qp_attr.send_cq = res->send_cq;
        qp_attr.recv_cq = res->recv_cq;
        qp_attr.srq      = res->srq;       /* Key: bind SRQ */
        qp_attr.qp_type  = IBV_QPT_RC;
        qp_attr.cap.max_send_wr  = MAX_SEND_WR;
        qp_attr.cap.max_send_sge = 1;
        /*
         * When using SRQ, the QP's own RQ is not used.
         * max_recv_wr and max_recv_sge can be set to 0.
         * Recv WRs are submitted to SRQ instead.
         */
        qp_attr.cap.max_recv_wr  = 0;
        qp_attr.cap.max_recv_sge = 0;

        res->qp[i] = ibv_create_qp(res->pd, &qp_attr);
        if (!res->qp[i]) {
            fprintf(stderr, "[Error] Failed to create QP[%d]: %s\n", i, strerror(errno));
            goto cleanup;
        }
        printf("[6] Created QP[%d] #%u (using SRQ, max_recv_wr=0)\n",
               i, res->qp[i]->qp_num);
    }

    return 0;

cleanup:
    return -1;
}

/* ========== Loopback Connection: QP[0] <--> QP[1] ========== */
static int connect_qps(struct srq_resources *res)
{
    struct rdma_endpoint ep[NUM_QPS];
    int ret, i;

    printf("\n[7] Loopback connection: QP[0] <--> QP[1]\n");

    /* Fill endpoint info for both QPs */
    for (i = 0; i < NUM_QPS; i++) {
        ret = fill_local_endpoint(res->ctx, res->qp[i],
                                  RDMA_DEFAULT_PORT_NUM,
                                  RDMA_DEFAULT_GID_INDEX, &ep[i]);
        if (ret != 0) {
            fprintf(stderr, "[Error] Failed to fill QP[%d] endpoint info\n", i);
            return -1;
        }
    }

    /* QP[0] connects to QP[1]'s endpoint, QP[1] connects to QP[0]'s endpoint */
    ret = qp_full_connect(res->qp[0], &ep[1], RDMA_DEFAULT_PORT_NUM,
                          res->is_roce, IBV_ACCESS_LOCAL_WRITE);
    if (ret != 0) {
        fprintf(stderr, "[Error] QP[0] connection failed\n");
        return -1;
    }

    ret = qp_full_connect(res->qp[1], &ep[0], RDMA_DEFAULT_PORT_NUM,
                          res->is_roce, IBV_ACCESS_LOCAL_WRITE);
    if (ret != 0) {
        fprintf(stderr, "[Error] QP[1] connection failed\n");
        return -1;
    }

    for (i = 0; i < NUM_QPS; i++) {
        printf("    QP[%d] #%u: ", i, res->qp[i]->qp_num);
        print_qp_state(res->qp[i]);
    }

    return 0;
}

/* ========== Post Recv WRs to SRQ ========== */
static int post_srq_recvs(struct srq_resources *res, int count)
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    int i, ret;

    printf("\n[8] Posting %d recv WRs to SRQ\n", count);
    printf("    Note: recv WRs are posted to SRQ, not to any individual QP\n");

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)(res->recv_buf + i * MSG_SIZE);
        sge.length = MSG_SIZE;
        sge.lkey   = res->recv_mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id   = i;        /* Use wr_id to mark recv buffer index */
        wr.sg_list = &sge;
        wr.num_sge = 1;

        /*
         * ibv_post_srq_recv(): Post recv WR to SRQ
         * Unlike ibv_post_recv(qp, ...), the parameter here is srq
         * When any QP bound to this SRQ receives a message, it consumes a recv WR from SRQ
         */
        ret = ibv_post_srq_recv(res->srq, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] ibv_post_srq_recv #%d failed: %s\n",
                    i, strerror(ret));
            return -1;
        }
    }

    printf("    Posted %d recv WRs to SRQ\n", count);
    return 0;
}

/* ========== Send Message via Specified QP ========== */
static int send_message(struct srq_resources *res, int qp_index,
                        const char *msg)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int ret;

    /* Copy message to send buffer (different QPs use different offsets to avoid overwrite) */
    int offset = qp_index * MSG_SIZE;
    snprintf(res->send_buf + offset, MSG_SIZE, "%s", msg);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)(res->send_buf + offset);
    sge.length = strlen(msg) + 1;
    sge.lkey   = res->send_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 1000 + qp_index;   /* Use wr_id to identify sender */
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(res->qp[qp_index], &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] QP[%d] post_send failed: %s\n",
                qp_index, strerror(ret));
        return -1;
    }

    /* Wait for send completion */
    ret = poll_cq_blocking(res->send_cq, &wc);
    if (ret != 0) {
        fprintf(stderr, "[Error] QP[%d] send CQE failed\n", qp_index);
        return -1;
    }

    printf("    QP[%d] sent: \"%s\" (len=%zu)\n", qp_index, msg, strlen(msg) + 1);
    return 0;
}

/* ========== Receive Messages from Recv CQ ========== */
static int recv_message(struct srq_resources *res, int expected_count)
{
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < expected_count; i++) {
        ret = poll_cq_blocking(res->recv_cq, &wc);
        if (ret != 0) {
            fprintf(stderr, "[Error] recv poll_cq failed\n");
            return -1;
        }

        /* Get info from WC */
        int buf_index = wc.wr_id;   /* Corresponds to SRQ recv WR index */
        char *data = res->recv_buf + buf_index * MSG_SIZE;

        printf("    SRQ received message: \"%s\" (wr_id=%lu, qp_num=%u, len=%u)\n",
               data, (unsigned long)wc.wr_id, wc.qp_num, wc.byte_len);

        /*
         * Note: wc.qp_num indicates which QP received this message
         * Since SRQ is used, messages can arrive from any QP
         * SRQ automatically allocates free recv buffers to the QP that needs them
         */
    }

    return 0;
}

/* ========== Cleanup Resources ========== */
static void cleanup_resources(struct srq_resources *res)
{
    int i;

    for (i = 0; i < NUM_QPS; i++) {
        if (res->qp[i]) ibv_destroy_qp(res->qp[i]);
    }
    if (res->srq)     ibv_destroy_srq(res->srq);
    if (res->send_cq) ibv_destroy_cq(res->send_cq);
    if (res->recv_cq) ibv_destroy_cq(res->recv_cq);
    if (res->send_mr) ibv_dereg_mr(res->send_mr);
    if (res->recv_mr) ibv_dereg_mr(res->recv_mr);
    if (res->pd)      ibv_dealloc_pd(res->pd);
    if (res->ctx)     ibv_close_device(res->ctx);
    free(res->send_buf);
    free(res->recv_buf);
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    struct srq_resources res;
    int ret;

    printf("=== Shared Receive Queue (SRQ) Demo ===\n\n");
    printf("Demo objectives:\n");
    printf("  - Create 1 SRQ, shared by 2 QPs\n");
    printf("  - Recv WRs are posted to SRQ, not to individual QPs\n");
    printf("  - Two QPs exchange messages, SRQ auto-allocates recv buffers\n\n");

    /* Initialize resources */
    ret = init_resources(&res);
    if (ret != 0) {
        fprintf(stderr, "[Error] Resource initialization failed\n");
        cleanup_resources(&res);
        return 1;
    }

    /* Connect QPs */
    ret = connect_qps(&res);
    if (ret != 0) {
        fprintf(stderr, "[Error] QP connection failed\n");
        cleanup_resources(&res);
        return 1;
    }

    /* Post recv WRs to SRQ */
    ret = post_srq_recvs(&res, 4);
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* QP[0] sends message to QP[1] */
    printf("\n[9] QP[0] -> QP[1] send message\n");
    ret = send_message(&res, 0, "Hello from QP[0]!");
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* QP[1] sends message to QP[0] */
    printf("\n[10] QP[1] -> QP[0] send message\n");
    ret = send_message(&res, 1, "Hello from QP[1]!");
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* Receive messages (2 messages) */
    printf("\n[11] Receiving messages from SRQ (recv CQ)\n");
    ret = recv_message(&res, 2);
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* ---- Summary ---- */
    printf("\n===== SRQ Principle Summary =====\n");
    printf("  1. ibv_create_srq(pd, &attr) creates a shared receive queue\n");
    printf("  2. When creating QP, set qp_attr.srq = srq, max_recv_wr=0\n");
    printf("  3. All recv WRs are submitted to SRQ via ibv_post_srq_recv()\n");
    printf("  4. When any QP bound to this SRQ receives a message, it auto-consumes a recv WR from SRQ\n");
    printf("  5. wc.qp_num in recv CQ identifies which QP the message came from\n");
    printf("\n===== SRQ Use Cases =====\n");
    printf("  - Databases: thousands of client connections, each QP uncertain when messages arrive\n");
    printf("  - Storage systems: many inter-node RDMA connections\n");
    printf("  - Key advantage: reduces N*M recv buffers to just M\n");
    printf("  - Memory savings: more QPs = more savings from SRQ\n");
    printf("  - Example: 1000 QP x 128 recv = 128000 -> SRQ only needs 1000\n");

    /* Cleanup */
    cleanup_resources(&res);
    printf("\nProgram finished\n");
    return 0;
}
