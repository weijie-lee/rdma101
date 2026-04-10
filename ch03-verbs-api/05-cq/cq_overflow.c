/**
 * CQ Overflow Demo
 *
 * This program demonstrates what happens when CQ is full and more completion events are generated:
 *   1. Create a very small CQ (depth=2)
 *   2. Rapidly submit multiple Send WRs, generating a large number of completion events
 *   3. Intentionally do not poll CQ, causing CQ overflow
 *   4. Capture and print overflow errors
 *   5. Explain how to avoid this in production (proper CQ sizing, timely polling)
 *
 * Compile: gcc -o cq_overflow cq_overflow.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE 4096
#define PORT_NUM    RDMA_DEFAULT_PORT_NUM
#define MSG_SIZE    64

/*
 * Note: The minimum CQ depth is determined by hardware; some HCAs will round up
 * very small values. For example, requesting cq_depth=2 may actually allocate 16 or more.
 * We request the smallest value possible, then try our best to trigger overflow.
 */
#define SMALL_CQ_DEPTH  2
#define NUM_WR_TO_POST  32    /* Number of WRs to submit (far exceeding CQ depth) */

int main(int argc, char *argv[])
{
    /* Resource declarations */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_cq      *cq_small = NULL;   /* Very small CQ */
    struct ibv_qp      *qp       = NULL;
    struct ibv_mr      *mr       = NULL;
    char               *buffer   = NULL;
    int                 num_devices;
    int                 ret;
    int                 i;

    printf("============================================\n");
    printf("  CQ Overflow Demo\n");
    printf("============================================\n\n");

    printf("  Experiment design:\n");
    printf("  - Create a very small CQ (requested depth=%d)\n", SMALL_CQ_DEPTH);
    printf("  - Submit %d Send WRs (all SIGNALED)\n", NUM_WR_TO_POST);
    printf("  - Intentionally do not poll CQ, causing CQ overflow\n");
    printf("  - Observe overflow behavior\n\n");

    /* ========== Initialize resources ========== */
    printf("[Step 1] Initialize RDMA resources\n");
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }
    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "Failed to open device");
    printf("  Device: %s\n", ibv_get_device_name(dev_list[0]));

    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "Failed to allocate PD");

    buffer = malloc(BUFFER_SIZE);
    CHECK_NULL(buffer, "malloc failed");
    memset(buffer, 0, BUFFER_SIZE);

    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(mr, "Failed to register MR");
    printf("  PD, MR created\n\n");

    /* ========== Step 2: Create small CQ ========== */
    printf("[Step 2] Create very small CQ (requested depth=%d)\n", SMALL_CQ_DEPTH);

    /*
     * The cqe parameter of ibv_create_cq() is a minimum guaranteed value;
     * the HCA may actually allocate more.
     * The actual allocated size can be checked via cq->cqe.
     */
    cq_small = ibv_create_cq(ctx, SMALL_CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq_small, "Failed to create small CQ");
    printf("  Requested depth: %d, actual allocated depth: %d\n", SMALL_CQ_DEPTH, cq_small->cqe);
    printf("  -> Note: Actual depth may be larger than requested (hardware alignment)\n\n");

    /* ========== Step 3: Create QP and loopback connect ========== */
    printf("[Step 3] Create QP and establish loopback connection\n");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq_small,
        .recv_cq = cq_small,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = NUM_WR_TO_POST,
            .max_recv_wr  = NUM_WR_TO_POST,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp = ibv_create_qp(pd, &qp_init);
    CHECK_NULL(qp, "Failed to create QP");
    printf("  QP created successfully, qp_num=%u\n", qp->qp_num);

    /* loopback connection */
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to fill endpoint info");

    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("  QP connection failed, skipping overflow test\n");
        goto cleanup;
    }
    printf("  QP connected (loopback, RTS state)\n\n");

    /* ========== Step 4: Post recv to prepare receive buffers ========== */
    printf("[Step 4] Post Recv to prepare receive buffers\n");
    for (i = 0; i < NUM_WR_TO_POST; i++) {
        struct ibv_sge recv_sge = {
            .addr   = (uintptr_t)(buffer + (i % 8) * MSG_SIZE),
            .length = MSG_SIZE,
            .lkey   = mr->lkey,
        };
        struct ibv_recv_wr recv_wr = {
            .wr_id   = 5000 + i,
            .sg_list = &recv_sge,
            .num_sge = 1,
        };
        struct ibv_recv_wr *bad_recv = NULL;
        ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
        if (ret != 0) {
            printf("  post_recv[%d] failed: %s\n", i, strerror(errno));
            break;
        }
    }
    printf("  Submitted %d Recv WRs\n\n", i);

    /* ========== Step 5: Rapidly submit many Send WRs (without polling CQ!) ========== */
    printf("[Step 5] Rapidly submit %d Send WRs (all SIGNALED, without polling CQ)\n", NUM_WR_TO_POST);
    printf("========================================\n");

    int posted = 0;
    for (i = 0; i < NUM_WR_TO_POST; i++) {
        struct ibv_sge sge = {
            .addr   = (uintptr_t)buffer,
            .length = MSG_SIZE,
            .lkey   = mr->lkey,
        };
        struct ibv_send_wr wr = {
            .wr_id      = 1000 + i,
            .sg_list    = &sge,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,  /* Each one requests completion notification */
        };
        struct ibv_send_wr *bad_wr = NULL;

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            printf("  post_send[%d] failed: errno=%d (%s)\n", i, errno, strerror(errno));
            printf("  -> May be because Send Queue is full (different from CQ overflow)\n");
            break;
        }
        posted++;
    }
    printf("  Successfully submitted %d Send WRs\n", posted);
    printf("  CQ depth=%d, but will generate %d completion events (send + recv)\n",
           cq_small->cqe, posted * 2);

    /* Wait a short time for HCA to process */
    usleep(100000);  /* 100ms */

    /* ========== Step 6: Try to poll CQ, observe overflow ========== */
    printf("\n[Step 6] Try to poll CQ, observe overflow effect\n");
    printf("========================================\n");

    struct ibv_wc wc;
    int ne;
    int success_count = 0;
    int error_count = 0;

    /* Try to poll all completion events */
    while ((ne = ibv_poll_cq(cq_small, 1, &wc)) != 0) {
        if (ne < 0) {
            printf("  ibv_poll_cq returned %d -- CQ may have overflowed!\n", ne);
            printf("  -> CQ overflow: When CQ is full, HCA cannot write new CQEs\n");
            printf("  -> Associated QP will be moved to ERROR state\n");
            error_count++;
            break;
        }
        if (wc.status == IBV_WC_SUCCESS) {
            success_count++;
        } else {
            error_count++;
            printf("  WC error: wr_id=%lu, status=%s (%d), vendor_err=0x%x\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status), wc.status,
                   wc.vendor_err);

            /*
             * When CQ overflows, QP is moved to ERROR state.
             * Subsequent WCs may show:
             *   - IBV_WC_WR_FLUSH_ERR: After QP enters ERROR, outstanding WRs are flushed
             *   - IBV_WC_GENERAL_ERR: General error
             */
            if (wc.status == IBV_WC_WR_FLUSH_ERR) {
                printf("  -> WR Flush Error: QP has entered ERROR state, WRs are flushed\n");
            }
        }
    }

    printf("\n  Result: successful completions=%d, error completions=%d\n", success_count, error_count);

    /* Check current QP state */
    printf("\n  Check QP state:\n");
    print_qp_state(qp);

    if (error_count > 0) {
        printf("\n  Observed CQ overflow effect!\n");
        printf("  -> When CQ is full, HCA cannot write new CQEs\n");
        printf("  -> QP is moved to ERROR state, all subsequent WRs are flushed\n");
    } else if (success_count == posted * 2) {
        printf("\n  All WCs succeeded (actual CQ depth is large enough: %d)\n", cq_small->cqe);
        printf("  -> Hardware actual CQ depth >= %d, no overflow\n", posted * 2);
        printf("  -> To trigger overflow, need to submit more WRs or use hardware with smaller CQ support\n");
    }

    /* ========== Summary ========== */
    printf("\n============================================\n");
    printf("  CQ Overflow Prevention Guide\n");
    printf("============================================\n");
    printf("  1. CQ size planning:\n");
    printf("     - CQ depth >= total send_wr + recv_wr of all associated QPs\n");
    printf("     - Formula: cq_depth >= N_qp * (max_send_wr + max_recv_wr)\n");
    printf("     - Leave headroom (typically 2x)\n\n");
    printf("  2. Poll CQ promptly:\n");
    printf("     - Busy polling: continuously call ibv_poll_cq()\n");
    printf("     - Event-driven: poll until empty immediately after notification\n");
    printf("     - Batch poll: ibv_poll_cq(cq, batch_size, wc_array)\n\n");
    printf("  3. Use Shared Receive Queue (SRQ):\n");
    printf("     - Multiple QPs share a receive queue, reducing total WR count\n\n");
    printf("  4. Selective Signaling:\n");
    printf("     - Not every WR needs a completion notification\n");
    printf("     - Remove IBV_SEND_SIGNALED, signal only every N WRs\n");
    printf("     - Greatly reduces CQ pressure\n\n");

cleanup:
    printf("[Cleanup] Releasing resources...\n");
    if (qp)       ibv_destroy_qp(qp);
    if (cq_small) ibv_destroy_cq(cq_small);
    if (mr)       ibv_dereg_mr(mr);
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  Done\n");
    return 0;
}
