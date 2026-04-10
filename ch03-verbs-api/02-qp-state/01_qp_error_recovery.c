/**
 * QP Error Recovery Demo
 *
 * This program demonstrates the complete flow of recovering a QP from ERROR state:
 *   1. Create QP and transition to RTS state (loopback)
 *   2. Intentionally trigger QP to enter ERROR state (submit WR with invalid parameters)
 *   3. Confirm QP is in ERROR state via ibv_query_qp()
 *   4. Perform recovery: ERROR -> RESET -> INIT -> RTR -> RTS
 *   5. Verify QP works normally after recovery
 *
 * Compile: gcc -o 01_qp_error_recovery 01_qp_error_recovery.c -I../../common ../../common/librdma_utils.a -libverbs
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
#define CQ_DEPTH    32
#define MSG_SIZE    64

/**
 * Query and print detailed QP attributes
 */
static void query_and_print_qp_detail(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    int mask = IBV_QP_STATE | IBV_QP_CUR_STATE | IBV_QP_PKEY_INDEX |
               IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PATH_MTU |
               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN |
               IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
               IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_MAX_DEST_RD_ATOMIC;

    memset(&attr, 0, sizeof(attr));
    memset(&init_attr, 0, sizeof(init_attr));

    if (ibv_query_qp(qp, &attr, mask, &init_attr) != 0) {
        fprintf(stderr, "  ibv_query_qp failed: %s\n", strerror(errno));
        return;
    }

    printf("  --- QP #%u Detailed Attributes ---\n", qp->qp_num);
    printf("    State (qp_state):        %s (%d)\n",
           qp_state_str(attr.qp_state), attr.qp_state);
    printf("    Current state (cur_qp_state): %s (%d)\n",
           qp_state_str(attr.cur_qp_state), attr.cur_qp_state);
    printf("    Port number:             %d\n", attr.port_num);
    printf("    PKey index:              %d\n", attr.pkey_index);

    /* Print more info only in non-RESET state */
    if (attr.qp_state >= IBV_QPS_INIT) {
        printf("    Access flags:            0x%x", attr.qp_access_flags);
        if (attr.qp_access_flags & IBV_ACCESS_LOCAL_WRITE)   printf(" LOCAL_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_READ)   printf(" REMOTE_READ");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_WRITE)  printf(" REMOTE_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_ATOMIC) printf(" REMOTE_ATOMIC");
        printf("\n");
    }
    if (attr.qp_state >= IBV_QPS_RTR) {
        printf("    Path MTU:                %d\n", attr.path_mtu);
        printf("    Dest QP number:          %u\n", attr.dest_qp_num);
        printf("    RQ PSN:                  %u\n", attr.rq_psn);
        printf("    Max dest RD atomic:      %d\n", attr.max_dest_rd_atomic);
    }
    if (attr.qp_state >= IBV_QPS_RTS) {
        printf("    SQ PSN:                  %u\n", attr.sq_psn);
        printf("    Timeout:                 %d\n", attr.timeout);
        printf("    Retry count:             %d\n", attr.retry_cnt);
        printf("    RNR retry:               %d\n", attr.rnr_retry);
        printf("    Max RD atomic:           %d\n", attr.max_rd_atomic);
    }
    printf("  -------------------------\n");
}

int main(int argc, char *argv[])
{
    /* Resource declarations */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_cq      *cq       = NULL;
    struct ibv_qp      *qp       = NULL;
    struct ibv_mr      *mr       = NULL;
    char               *buffer   = NULL;
    int                 num_devices;
    int                 ret;

    printf("============================================\n");
    printf("  QP Error Recovery Demo\n");
    printf("============================================\n\n");

    /*
     * ERROR state in the QP state machine:
     *
     *   RESET -> INIT -> RTR -> RTS -> (normal operation)
     *                                    | (error occurs)
     *                                  ERROR
     *                                    | (recovery)
     *                                  RESET -> INIT -> RTR -> RTS
     *
     * Common causes for QP entering ERROR state:
     *   1. Remote QP destroyed or entered ERROR
     *   2. CQ overflow
     *   3. Protection error (PD mismatch, lkey/rkey error)
     *   4. Retry count exhausted (retry_cnt exceeded)
     *   5. RNR retry exhausted
     */

    /* ========== Step 1: Initialize all resources ========== */
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

    cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq, "Failed to create CQ");

    buffer = malloc(BUFFER_SIZE);
    CHECK_NULL(buffer, "malloc failed");
    memset(buffer, 0, BUFFER_SIZE);

    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                     IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(mr, "Failed to register MR");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp = ibv_create_qp(pd, &qp_init);
    CHECK_NULL(qp, "Failed to create QP");
    printf("  QP created successfully, qp_num=%u\n", qp->qp_num);
    printf("  All basic resources are ready\n\n");

    /* ========== Step 2: Transition QP to RTS state (loopback) ========== */
    printf("[Step 2] Transition QP to RTS state (loopback connection)\n");

    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to fill endpoint info");

    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  Transport type: %s\n", transport_str(transport));

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("  QP connection failed, cannot continue test\n");
        goto cleanup;
    }
    printf("  QP has reached RTS state\n");
    query_and_print_qp_detail(qp);
    printf("\n");

    /* ========== Step 3: Verify QP works normally ========== */
    printf("[Step 3] Verify QP works normally (Send/Recv loopback)\n");

    /* Post Recv first */
    struct ibv_sge recv_sge = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr recv_wr = {
        .wr_id   = 100,
        .sg_list = &recv_sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv = NULL;
    ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
    CHECK_ERRNO(ret, "post_recv failed");

    /* Post Send */
    snprintf(buffer + MSG_SIZE, MSG_SIZE, "Before Error");
    struct ibv_sge send_sge = {
        .addr   = (uintptr_t)(buffer + MSG_SIZE),
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id      = 200,
        .sg_list    = &send_sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send = NULL;
    ret = ibv_post_send(qp, &send_wr, &bad_send);
    CHECK_ERRNO(ret, "post_send failed");

    /* Poll two completion events (send + recv) */
    struct ibv_wc wc;
    int completed = 0;
    int poll_attempts = 0;
    while (completed < 2 && poll_attempts < 1000000) {
        int ne = ibv_poll_cq(cq, 1, &wc);
        if (ne > 0) {
            printf("  Completion event: wr_id=%lu, status=%s, opcode=%s\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status),
                   wc_opcode_str(wc.opcode));
            completed++;
        }
        poll_attempts++;
    }
    printf("  QP normal operation confirmed (%d operations completed)\n\n", completed);

    /* ========== Step 4: Intentionally trigger QP to enter ERROR state ========== */
    printf("[Step 4] Intentionally trigger QP to enter ERROR state\n");
    printf("========================================\n");

    /*
     * Method: Issue RDMA Write with invalid rkey -> produces Remote Access Error
     * HCA will move QP to ERROR state.
     *
     * Alternatively, use ibv_modify_qp to directly move QP to ERROR:
     *   attr.qp_state = IBV_QPS_ERR;
     *   ibv_modify_qp(qp, &attr, IBV_QP_STATE);
     */
    printf("  Method: Send RDMA Write with invalid rkey (0xDEADBEEF)\n");

    struct ibv_sge bad_sge = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr bad_wr_s = {
        .wr_id      = 999,
        .sg_list    = &bad_sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = (uintptr_t)buffer,
            .rkey        = 0xDEADBEEF,   /* Invalid rkey! */
        },
    };
    struct ibv_send_wr *bad_wr_ptr = NULL;
    ret = ibv_post_send(qp, &bad_wr_s, &bad_wr_ptr);
    if (ret != 0) {
        printf("  post_send already failed: %s\n", strerror(errno));
        printf("  -> Trying to directly move QP to ERROR via ibv_modify_qp\n");
        struct ibv_qp_attr err_attr = { .qp_state = IBV_QPS_ERR };
        ret = ibv_modify_qp(qp, &err_attr, IBV_QP_STATE);
        if (ret != 0) {
            printf("  modify_qp to ERROR also failed: %s\n", strerror(errno));
            goto cleanup;
        }
    } else {
        /* Wait for error completion event */
        printf("  Waiting for HCA to process and report error...\n");
        usleep(100000);  /* 100ms */
        while (ibv_poll_cq(cq, 1, &wc) > 0) {
            printf("  CQ: wr_id=%lu, status=%s (%d)\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status), wc.status);
        }
    }

    /* Confirm QP is now in ERROR state */
    printf("\n  Query QP state (should be ERROR):\n");
    query_and_print_qp_detail(qp);

    /* ========== Step 5: Perform error recovery ========== */
    printf("\n[Step 5] Perform QP error recovery: ERROR -> RESET -> INIT -> RTR -> RTS\n");
    printf("========================================\n\n");

    /*
     * Recovery steps:
     * 1. ERROR -> RESET: Clear all state and outstanding WRs
     * 2. RESET -> INIT:  Re-set port and access permissions
     * 3. INIT -> RTR:    Re-set path parameters
     * 4. RTR -> RTS:     Re-set send parameters
     *
     * Note: After reset, qp_num remains unchanged, but all previous WRs are discarded.
     * If using SRQ, resetting QP does not affect WRs in the SRQ.
     */

    /* Step 5a: ERROR -> RESET */
    printf("  5a. ERROR -> RESET\n");
    ret = qp_to_reset(qp);
    CHECK_ERRNO(ret, "QP ERROR->RESET failed");
    printf("      Succeeded!\n");
    query_and_print_qp_detail(qp);

    /* Need to drain all residual WCs from CQ */
    printf("  Cleaning up residual events in CQ...\n");
    while (ibv_poll_cq(cq, 1, &wc) > 0) {
        printf("      Cleaned: wr_id=%lu, status=%s\n",
               (unsigned long)wc.wr_id, ibv_wc_status_str(wc.status));
    }
    printf("      CQ is empty\n\n");

    /* Step 5b: RESET -> INIT -> RTR -> RTS */
    printf("  5b. RESET -> INIT -> RTR -> RTS (using qp_full_connect)\n");

    /* Re-fetch endpoint info (qp_num unchanged) */
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to re-fill endpoint info");

    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("      QP reconnection failed!\n");
        goto cleanup;
    }
    printf("      Succeeded! QP has recovered to RTS state\n");
    query_and_print_qp_detail(qp);
    printf("\n");

    /* ========== Step 6: Verify QP works normally after recovery ========== */
    printf("[Step 6] Verify QP works normally after recovery\n");
    printf("========================================\n");

    /* Perform Send/Recv again */
    struct ibv_sge recv_sge2 = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr recv_wr2 = {
        .wr_id   = 300,
        .sg_list = &recv_sge2,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv2 = NULL;
    ret = ibv_post_recv(qp, &recv_wr2, &bad_recv2);
    CHECK_ERRNO(ret, "post_recv after recovery failed");

    snprintf(buffer + MSG_SIZE, MSG_SIZE, "After Recovery!");
    struct ibv_sge send_sge2 = {
        .addr   = (uintptr_t)(buffer + MSG_SIZE),
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr send_wr2 = {
        .wr_id      = 400,
        .sg_list    = &send_sge2,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send2 = NULL;
    ret = ibv_post_send(qp, &send_wr2, &bad_send2);
    CHECK_ERRNO(ret, "post_send after recovery failed");

    /* Poll completion events */
    completed = 0;
    poll_attempts = 0;
    while (completed < 2 && poll_attempts < 1000000) {
        int ne = ibv_poll_cq(cq, 1, &wc);
        if (ne > 0) {
            printf("  Completion event: wr_id=%lu, status=%s, opcode=%s\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status),
                   wc_opcode_str(wc.opcode));
            if (wc.status == IBV_WC_SUCCESS) completed++;
        }
        poll_attempts++;
    }

    if (completed >= 2) {
        printf("  QP recovery succeeded! Send/Recv works normally\n");
        printf("  Received data: \"%s\"\n", buffer);
    } else {
        printf("  Operations not completed after recovery (completed=%d)\n", completed);
    }

    /* ========== Summary ========== */
    printf("\n============================================\n");
    printf("  QP Error Recovery Summary\n");
    printf("============================================\n");
    printf("  Recovery path: ERROR -> RESET -> INIT -> RTR -> RTS\n\n");
    printf("  Notes:\n");
    printf("  1. Drain all residual WCs from CQ before reset\n");
    printf("  2. QP number remains unchanged, but remote peer may need notification\n");
    printf("  3. All outstanding WRs are discarded during RESET\n");
    printf("  4. Remote QP may also enter ERROR (need simultaneous recovery)\n");
    printf("  5. Best practice: Negotiate between both ends and rebuild connection simultaneously\n");
    printf("  6. Some errors (e.g. hardware failure) may not be recoverable via reset\n\n");

cleanup:
    printf("[Cleanup] Releasing resources...\n");
    if (qp)       ibv_destroy_qp(qp);
    if (cq)       ibv_destroy_cq(cq);
    if (mr)       ibv_dereg_mr(mr);
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  Done\n");
    return 0;
}
