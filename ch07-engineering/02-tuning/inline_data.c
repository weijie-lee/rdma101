/**
 * Inline Data Optimization Comparison Experiment
 *
 * Demonstrates Inline Data optimization technique:
 *   - Queries the device's max_inline_data capability
 *   - Compares performance of normal DMA send vs Inline send
 *   - Runs in loopback mode, no need for two machines
 *
 * Principle:
 *   Normal send path: CPU fills WQE(with data address) -> NIC reads data via DMA -> NIC sends
 *   Inline path:      CPU copies data into WQE         -> NIC takes data directly from WQE (saves one DMA)
 *
 *   For small messages (< max_inline_data), Inline can significantly reduce latency because:
 *   1. One fewer PCIe DMA read round-trip
 *   2. No lkey needed (data is already in the WQE)
 *
 * Build:
 *   gcc -o inline_data inline_data.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * Run: ./inline_data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constant Definitions ========== */
#define MSG_SIZE        32          /* Message size (bytes), suitable for inline */
#define NUM_MESSAGES    1000        /* Number of messages per test round */
#define BUFFER_SIZE     4096        /* Buffer size */
#define CQ_DEPTH        256         /* CQ depth */
#define MAX_SEND_WR     128         /* SQ maximum WR count */
#define MAX_RECV_WR     128         /* RQ maximum WR count */

/* ========== Get High-Precision Time (nanoseconds) ========== */
static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ========== Batch Post Recv WRs ========== */
static int post_recvs(struct ibv_qp *qp, struct ibv_mr *mr,
                      char *buf, int count)
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    int i, ret;

    /* Post one recv WR for each message */
    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)(buf + (i % 4) * MSG_SIZE);
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id   = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.next    = NULL;

        ret = ibv_post_recv(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] post_recv #%d failed: errno=%d\n", i, ret);
            return -1;
        }
    }
    return 0;
}

/* ========== Normal Send (DMA Path) ========== */
static int send_normal(struct ibv_qp *qp, struct ibv_cq *cq,
                       struct ibv_mr *mr, char *buf, int count)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        /* Build SGE: points to registered memory region */
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;      /* Normal send requires lkey */

        /* Build Send WR: does not use IBV_SEND_INLINE */
        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;  /* Each one generates CQE */

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] Normal post_send #%d failed: errno=%d\n", i, ret);
            return -1;
        }

        /* Wait for send completion */
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) return -1;
    }
    return 0;
}

/* ========== Inline Send (Data Embedded in WQE) ========== */
static int send_inline(struct ibv_qp *qp, struct ibv_cq *cq,
                       char *buf, int count)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        /*
         * Inline send: data in sge is directly copied into the WQE
         * Note: lkey can be set to 0, because NIC doesn't need to DMA read data
         *       In practice, setting lkey doesn't affect correctness
         */
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = 0;            /* Inline mode doesn't need lkey! */

        /* Build Send WR: use IBV_SEND_INLINE flag */
        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;  /* Key! */

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] Inline post_send #%d failed: errno=%d\n", i, ret);
            return -1;
        }

        /* Wait for send completion */
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) return -1;
    }
    return 0;
}

/* ========== Drain Receive-Side CQ ========== */
static int drain_recv_cq(struct ibv_cq *cq, int count)
{
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) {
            fprintf(stderr, "[Error] drain_recv_cq #%d failed\n", i);
            return -1;
        }
    }
    return 0;
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    /* RDMA resources */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *send_cq = NULL, *recv_cq = NULL;
    struct ibv_qp *qp = NULL;
    struct ibv_mr *send_mr = NULL, *recv_mr = NULL;
    char *send_buf = NULL, *recv_buf = NULL;
    int num_devices, ret;
    int is_roce;

    printf("=== Inline Data Optimization Comparison Experiment ===\n\n");

    /* ---- Step 1: Open device ---- */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "Failed to open device");
    printf("[1] Opened device: %s\n", ibv_get_device_name(dev_list[0]));

    /* Auto-detect transport layer type */
    is_roce = (detect_transport(ctx, RDMA_DEFAULT_PORT_NUM) == RDMA_TRANSPORT_ROCE);
    printf("    Transport: %s\n", is_roce ? "RoCE" : "IB");

    /* ---- Step 2: Query device inline capability ---- */
    struct ibv_device_attr dev_attr;
    ret = ibv_query_device(ctx, &dev_attr);
    CHECK_ERRNO(ret, "Failed to query device attributes");
    /*
     * Note: ibv_device_attr does not have a direct max_inline_data field.
     * max_inline_data is actually determined by the driver during QP creation,
     * can be set as a requested value in ibv_create_qp's cap,
     * and after creation, cap will return the actual supported value.
     */
    printf("[2] Device capabilities: max_qp_wr=%d, max_sge=%d\n",
           dev_attr.max_qp_wr, dev_attr.max_sge);

    /* ---- Step 3: Allocate resources ---- */
    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "Failed to allocate PD");

    send_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(send_cq, "Failed to create send_cq");

    recv_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(recv_cq, "Failed to create recv_cq");

    /* Allocate buffers */
    send_buf = malloc(BUFFER_SIZE);
    recv_buf = malloc(BUFFER_SIZE);
    CHECK_NULL(send_buf, "Failed to allocate send_buf");
    CHECK_NULL(recv_buf, "Failed to allocate recv_buf");
    memset(send_buf, 'A', BUFFER_SIZE);
    memset(recv_buf, 0, BUFFER_SIZE);

    /* Register memory */
    send_mr = ibv_reg_mr(pd, send_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(send_mr, "Failed to register send_mr");
    recv_mr = ibv_reg_mr(pd, recv_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(recv_mr, "Failed to register recv_mr");

    /* ---- Step 4: Create QP (with inline data) ---- */
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = send_cq;
    qp_init_attr.recv_cq = recv_cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = MAX_SEND_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_RECV_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = 256;  /* Request 256-byte inline */

    qp = ibv_create_qp(pd, &qp_init_attr);
    CHECK_NULL(qp, "Failed to create QP");

    /*
     * After creation, qp_init_attr.cap.max_inline_data is updated to the actual value.
     * Different hardware/drivers support different max_inline_data.
     */
    printf("[3] Created QP #%u\n", qp->qp_num);
    printf("    Requested max_inline_data = 256\n");
    printf("    Actual max_inline_data = %u\n", qp_init_attr.cap.max_inline_data);

    if (qp_init_attr.cap.max_inline_data < MSG_SIZE) {
        fprintf(stderr, "[Warning] Device doesn't support %d-byte inline, max %u bytes\n",
                MSG_SIZE, qp_init_attr.cap.max_inline_data);
        fprintf(stderr, "        Will skip inline test\n");
    }

    /* ---- Step 5: Loopback connection ---- */
    printf("[4] Loopback connection...\n");
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, RDMA_DEFAULT_PORT_NUM,
                              RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to fill local endpoint info");

    /* Loopback: remote is self */
    ret = qp_full_connect(qp, &local_ep, RDMA_DEFAULT_PORT_NUM,
                          is_roce, IBV_ACCESS_LOCAL_WRITE);
    CHECK_ERRNO(ret, "QP connection failed");
    print_qp_state(qp);

    /* ---- Step 6: Normal send test ---- */
    printf("\n===== Test 1: Normal Send (DMA Path) =====\n");
    printf("  Message size: %d bytes, message count: %d\n", MSG_SIZE, NUM_MESSAGES);

    /* Post all recvs first */
    ret = post_recvs(qp, recv_mr, recv_buf, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    uint64_t t1 = get_time_ns();
    ret = send_normal(qp, send_cq, send_mr, send_buf, NUM_MESSAGES);
    uint64_t t2 = get_time_ns();
    if (ret != 0) goto cleanup;

    /* Drain recv CQ */
    ret = drain_recv_cq(recv_cq, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    double normal_us = (double)(t2 - t1) / 1000.0;
    double normal_per_msg = normal_us / NUM_MESSAGES;
    printf("  Total time: %.2f us\n", normal_us);
    printf("  Average per message: %.3f us\n", normal_per_msg);

    /* ---- Step 7: Inline send test ---- */
    printf("\n===== Test 2: Inline Send (Data Embedded in WQE) =====\n");
    printf("  Message size: %d bytes, message count: %d\n", MSG_SIZE, NUM_MESSAGES);
    printf("  IBV_SEND_INLINE flag: enabled\n");
    printf("  lkey: not needed (data is directly in WQE)\n");

    /* Post recvs again */
    ret = post_recvs(qp, recv_mr, recv_buf, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    uint64_t t3 = get_time_ns();
    ret = send_inline(qp, send_cq, send_buf, NUM_MESSAGES);
    uint64_t t4 = get_time_ns();
    if (ret != 0) goto cleanup;

    /* Drain recv CQ */
    ret = drain_recv_cq(recv_cq, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    double inline_us = (double)(t4 - t3) / 1000.0;
    double inline_per_msg = inline_us / NUM_MESSAGES;
    printf("  Total time: %.2f us\n", inline_us);
    printf("  Average per message: %.3f us\n", inline_per_msg);

    /* ---- Results comparison ---- */
    printf("\n===== Performance Comparison =====\n");
    printf("  ┌──────────────┬───────────────┬──────────────┐\n");
    printf("  │ Mode         │ Total (us)    │ Per-msg (us) │\n");
    printf("  ├──────────────┼───────────────┼──────────────┤\n");
    printf("  │ Normal (DMA) │ %13.2f │ %12.3f │\n", normal_us, normal_per_msg);
    printf("  │ Inline       │ %13.2f │ %12.3f │\n", inline_us, inline_per_msg);
    printf("  └──────────────┴───────────────┴──────────────┘\n");

    if (inline_us < normal_us) {
        double speedup = normal_us / inline_us;
        printf("\n  Conclusion: Inline mode is %.2fx faster\n", speedup);
    } else {
        printf("\n  Conclusion: Inline did not show significant advantage in current environment\n");
        printf("  Note: SoftRoCE emulation may not reflect true Inline optimization\n");
        printf("        The difference is more noticeable on real hardware (ConnectX, etc.)\n");
    }

    printf("\n===== Principle Summary =====\n");
    printf("  1. Inline Data copies small message data directly into the WQE (Work Queue Element)\n");
    printf("  2. NIC takes data directly from WQE, skipping the DMA read step\n");
    printf("  3. Saves one PCIe round-trip, reducing small message latency\n");
    printf("  4. Inline send doesn't need lkey (data is no longer in MR)\n");
    printf("  5. Applicable when message size < max_inline_data\n");
    printf("  6. Not applicable for large messages (data exceeds WQE capacity)\n");

cleanup:
    /* Release resources in reverse order */
    if (qp)       ibv_destroy_qp(qp);
    if (send_cq)  ibv_destroy_cq(send_cq);
    if (recv_cq)  ibv_destroy_cq(recv_cq);
    if (send_mr)  ibv_dereg_mr(send_mr);
    if (recv_mr)  ibv_dereg_mr(recv_mr);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);
    free(send_buf);
    free(recv_buf);

    printf("\nProgram finished\n");
    return 0;
}
