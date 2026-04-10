/**
 * RDMA Error Triggering and Recovery Demo
 *
 * Deliberately triggers 4 typical RDMA errors, demonstrating:
 *   - Error WC status codes
 *   - Detailed error diagnostic information
 *   - QP error recovery flow (ERROR -> RESET -> INIT -> RTR -> RTS)
 *
 * Error scenarios triggered:
 *   a) IBV_WC_REM_ACCESS_ERR: RDMA Write with wrong rkey
 *   b) IBV_WC_LOC_PROT_ERR:  Using lkey from a different PD
 *   c) IBV_WC_WR_FLUSH_ERR:  Post send on an ERROR state QP
 *   d) IBV_WC_RNR_RETRY_EXC_ERR: Send without posting recv (rnr_retry=0)
 *
 * Build:
 *   gcc -o trigger_errors trigger_errors.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * Run: ./trigger_errors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constant Definitions ========== */
#define BUFFER_SIZE     4096
#define CQ_DEPTH        64
#define MAX_SEND_WR     64
#define MAX_RECV_WR     64

/* ========== Global RDMA Resources ========== */
struct test_resources {
    struct ibv_device    **dev_list;
    struct ibv_context   *ctx;
    struct ibv_pd        *pd;
    struct ibv_pd        *pd2;       /* Second PD (for triggering LOC_PROT_ERR) */
    struct ibv_cq        *send_cq;
    struct ibv_cq        *recv_cq;
    struct ibv_qp        *qp;
    struct ibv_mr        *mr;
    struct ibv_mr        *mr2;       /* MR registered with pd2 (for triggering errors) */
    char                 *buf;
    char                 *buf2;
    int                   is_roce;
    struct rdma_endpoint  local_ep;
};

/* ========== Print Error Diagnostic Info ========== */
static void print_error_diagnosis(int status)
{
    printf("\n  ┌─ Error Diagnosis ──────────────────────────────────────┐\n");

    switch (status) {
    case IBV_WC_SUCCESS:
        printf("  │ Status: IBV_WC_SUCCESS (0)                              │\n");
        printf("  │ Meaning: Operation completed successfully               │\n");
        break;

    case IBV_WC_REM_ACCESS_ERR:
        printf("  │ Status: IBV_WC_REM_ACCESS_ERR (5)                       │\n");
        printf("  │ Meaning: Remote access permission error                 │\n");
        printf("  │ Causes:                                                 │\n");
        printf("  │   1. rkey is wrong or has expired                       │\n");
        printf("  │   2. Target address exceeds MR registered range         │\n");
        printf("  │   3. MR lacks REMOTE_WRITE/READ permission              │\n");
        printf("  │ Fix:                                                    │\n");
        printf("  │   - Verify rkey and remote address were exchanged correctly │\n");
        printf("  │   - Ensure MR registered with IBV_ACCESS_REMOTE_WRITE etc. │\n");
        break;

    case IBV_WC_LOC_PROT_ERR:
        printf("  │ Status: IBV_WC_LOC_PROT_ERR (4)                        │\n");
        printf("  │ Meaning: Local protection domain error                  │\n");
        printf("  │ Causes:                                                 │\n");
        printf("  │   1. lkey's MR and QP are not in the same PD            │\n");
        printf("  │   2. lkey is invalid or has been deregistered           │\n");
        printf("  │   3. SGE address exceeds MR registered range            │\n");
        printf("  │ Fix:                                                    │\n");
        printf("  │   - Verify QP and MR belong to the same PD              │\n");
        printf("  │   - Check that lkey is correct                          │\n");
        break;

    case IBV_WC_WR_FLUSH_ERR:
        printf("  │ Status: IBV_WC_WR_FLUSH_ERR (5)                        │\n");
        printf("  │ Meaning: WR flushed (QP already in ERROR state)         │\n");
        printf("  │ Causes:                                                 │\n");
        printf("  │   1. QP entered ERROR state due to a previous error     │\n");
        printf("  │   2. All pending WRs will be marked as FLUSH_ERR        │\n");
        printf("  │ Fix:                                                    │\n");
        printf("  │   - First fix the root cause that put QP in ERROR       │\n");
        printf("  │   - Recover QP: ERROR -> RESET -> INIT -> RTR -> RTS    │\n");
        break;

    case IBV_WC_RNR_RETRY_EXC_ERR:
        printf("  │ Status: IBV_WC_RNR_RETRY_EXC_ERR (11)                  │\n");
        printf("  │ Meaning: RNR (Receiver Not Ready) retry exceeded        │\n");
        printf("  │ Causes:                                                 │\n");
        printf("  │   1. Peer didn't post recv before message arrived       │\n");
        printf("  │   2. Peer's recv WRs have been exhausted                │\n");
        printf("  │   3. rnr_retry set too low (0=no retry)                 │\n");
        printf("  │ Fix:                                                    │\n");
        printf("  │   - Ensure peer does post_recv before sending           │\n");
        printf("  │   - Set reasonable rnr_retry (recommend 7=infinite)     │\n");
        break;

    default:
        printf("  │ Status: %s (%d)\n", ibv_wc_status_str(status), status);
        printf("  │ Meaning: See error_diagnosis tool for details           │\n");
        break;
    }

    printf("  └──────────────────────────────────────────────────────┘\n");
}

/* ========== Initialize Resources ========== */
static int init_resources(struct test_resources *res)
{
    int num_devices, ret;

    memset(res, 0, sizeof(*res));

    /* Open device */
    res->dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(res->dev_list, "Failed to get device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }

    res->ctx = ibv_open_device(res->dev_list[0]);
    CHECK_NULL(res->ctx, "Failed to open device");
    printf("[Init] Device: %s\n", ibv_get_device_name(res->dev_list[0]));

    res->is_roce = (detect_transport(res->ctx, RDMA_DEFAULT_PORT_NUM)
                    == RDMA_TRANSPORT_ROCE);
    printf("[Init] Transport: %s\n", res->is_roce ? "RoCE" : "IB");

    /* Allocate two PDs (one normal, one for triggering LOC_PROT_ERR) */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate PD");

    res->pd2 = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd2, "Failed to allocate PD2");

    /* CQ */
    res->send_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->send_cq, "Failed to create send_cq");
    res->recv_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->recv_cq, "Failed to create recv_cq");

    /* Buffers and MR */
    res->buf = calloc(1, BUFFER_SIZE);
    res->buf2 = calloc(1, BUFFER_SIZE);
    CHECK_NULL(res->buf, "Failed to allocate buf");
    CHECK_NULL(res->buf2, "Failed to allocate buf2");

    /* mr belongs to pd (same PD as QP) */
    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    CHECK_NULL(res->mr, "Failed to register MR");

    /* mr2 belongs to pd2 (different PD, for triggering protection domain error) */
    res->mr2 = ibv_reg_mr(res->pd2, res->buf2, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->mr2, "Failed to register MR2");

    printf("[Init] Resources created (PD x2, CQ x2, MR x2)\n");
    return 0;

cleanup:
    return -1;
}

/* ========== Create and Connect QP (loopback) ========== */
static int create_and_connect_qp(struct test_resources *res, int rnr_retry)
{
    struct ibv_qp_init_attr qp_attr;
    int ret;

    /* Destroy old QP if exists */
    if (res->qp) {
        ibv_destroy_qp(res->qp);
        res->qp = NULL;
    }

    /* Create QP */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->send_cq;
    qp_attr.recv_cq = res->recv_cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = MAX_SEND_WR;
    qp_attr.cap.max_recv_wr  = MAX_RECV_WR;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "Failed to create QP");

    /* Fill endpoint */
    ret = fill_local_endpoint(res->ctx, res->qp, RDMA_DEFAULT_PORT_NUM,
                              RDMA_DEFAULT_GID_INDEX, &res->local_ep);
    CHECK_ERRNO(ret, "Failed to fill endpoint");

    /* Fill remote MR info (loopback uses own) */
    res->local_ep.buf_addr = (uint64_t)res->buf;
    res->local_ep.buf_rkey = res->mr->rkey;

    /* Manual connection (for custom rnr_retry) */
    ret = qp_to_init(res->qp, RDMA_DEFAULT_PORT_NUM,
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                     IBV_ACCESS_REMOTE_READ);
    CHECK_ERRNO(ret, "QP -> INIT failed");

    ret = qp_to_rtr(res->qp, &res->local_ep, RDMA_DEFAULT_PORT_NUM,
                    res->is_roce);
    CHECK_ERRNO(ret, "QP -> RTR failed");

    /* Custom RTS parameters (especially rnr_retry) */
    {
        struct ibv_qp_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state      = IBV_QPS_RTS;
        attr.sq_psn         = RDMA_DEFAULT_PSN;
        attr.timeout        = 14;
        attr.retry_cnt      = 7;
        attr.rnr_retry      = rnr_retry;   /* Customizable */
        attr.max_rd_atomic  = 1;

        ret = ibv_modify_qp(res->qp, &attr,
                            IBV_QP_STATE | IBV_QP_SQ_PSN |
                            IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
        CHECK_ERRNO(ret, "QP -> RTS failed");
    }

    return 0;

cleanup:
    return -1;
}

/* ========== Recover QP: ERROR -> RESET -> INIT -> RTR -> RTS ========== */
static int recover_qp(struct test_resources *res)
{
    int ret;

    printf("\n  [Recovery] QP state recovery flow:\n");
    print_qp_state(res->qp);

    printf("  [Recovery] ERROR -> RESET\n");
    ret = qp_to_reset(res->qp);
    if (ret != 0) {
        fprintf(stderr, "  [Recovery] -> RESET failed\n");
        return -1;
    }
    print_qp_state(res->qp);

    printf("  [Recovery] RESET -> INIT -> RTR -> RTS\n");
    ret = qp_full_connect(res->qp, &res->local_ep, RDMA_DEFAULT_PORT_NUM,
                          res->is_roce,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
    if (ret != 0) {
        fprintf(stderr, "  [Recovery] Reconnection failed\n");
        return -1;
    }
    print_qp_state(res->qp);
    printf("  [Recovery] QP recovered to RTS!\n");

    /* Drain residual CQEs from CQ */
    struct ibv_wc wc;
    while (ibv_poll_cq(res->send_cq, 1, &wc) > 0) { /* drain */ }
    while (ibv_poll_cq(res->recv_cq, 1, &wc) > 0) { /* drain */ }

    return 0;
}

/* ========== Helper: Non-blocking Poll CQ (with timeout) ========== */
static int poll_cq_with_timeout(struct ibv_cq *cq, struct ibv_wc *wc,
                                int timeout_us)
{
    int ne;
    int elapsed = 0;
    int step = 100;  /* Sleep 100us each time */

    while (elapsed < timeout_us) {
        ne = ibv_poll_cq(cq, 1, wc);
        if (ne < 0) return -1;
        if (ne > 0) return 0;
        usleep(step);
        elapsed += step;
    }
    return -2;  /* Timeout */
}

/* ========== Scenario A: IBV_WC_REM_ACCESS_ERR ========== */
static void test_rem_access_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ Scenario A: IBV_WC_REM_ACCESS_ERR (Remote Access Error) ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  Trigger method: RDMA Write with wrong rkey\n\n");

    if (create_and_connect_qp(res, 7) != 0) {
        fprintf(stderr, "  Failed to create QP, skipping this test\n");
        return;
    }

    /* Build RDMA Write WR with wrong rkey */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xA001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = (uint64_t)res->buf;
    wr.wr.rdma.rkey        = 0xDEADBEEF;   /* Deliberately wrong rkey! */

    printf("  Sending RDMA Write (rkey=0xDEADBEEF, deliberately wrong)...\n");

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  post_send failed: %s\n", strerror(ret));
        return;
    }

    /* Wait for error CQE */
    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 2000000);
    if (ret == 0) {
        printf("  Received WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else {
        printf("  Waiting for CQE timed out (remote may not have returned error)\n");
    }

    /* Recover QP */
    recover_qp(res);
}

/* ========== Scenario B: IBV_WC_LOC_PROT_ERR ========== */
static void test_loc_prot_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ Scenario B: IBV_WC_LOC_PROT_ERR (Local Protection Error) ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  Trigger method: Use lkey from MR in a different PD\n");
    printf("  QP belongs to PD1, but lkey comes from MR registered with PD2\n\n");

    if (create_and_connect_qp(res, 7) != 0) {
        fprintf(stderr, "  Failed to create QP, skipping this test\n");
        return;
    }

    /* Post recv first (to avoid RNR) */
    struct ibv_sge recv_sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };
    struct ibv_recv_wr recv_wr, *bad_recv;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;
    ibv_post_recv(res->qp, &recv_wr, &bad_recv);

    /* Use mr2 (belongs to pd2) lkey */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf2,
        .length = 64,
        .lkey   = res->mr2->lkey,   /* PD2's lkey, doesn't match QP's PD! */
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xB001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    printf("  Sending Send (lkey from different PD, deliberately wrong)...\n");

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  post_send failed: %s\n", strerror(ret));
        return;
    }

    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 2000000);
    if (ret == 0) {
        printf("  Received WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else {
        printf("  Waiting for CQE timed out\n");
    }

    recover_qp(res);
}

/* ========== Scenario C: IBV_WC_WR_FLUSH_ERR ========== */
static void test_wr_flush_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ Scenario C: IBV_WC_WR_FLUSH_ERR (WR Flushed)        ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  Trigger method: Put QP in ERROR state first, then post send\n\n");

    if (create_and_connect_qp(res, 7) != 0) {
        fprintf(stderr, "  Failed to create QP, skipping this test\n");
        return;
    }

    /* Manually transition QP to ERROR state */
    struct ibv_qp_attr err_attr;
    memset(&err_attr, 0, sizeof(err_attr));
    err_attr.qp_state = IBV_QPS_ERR;
    int ret = ibv_modify_qp(res->qp, &err_attr, IBV_QP_STATE);
    if (ret != 0) {
        fprintf(stderr, "  Failed to transition QP to ERROR: %s\n", strerror(errno));
        return;
    }

    printf("  QP manually transitioned to ERROR state\n");
    print_qp_state(res->qp);

    /* Post send in ERROR state */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xC001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    printf("  Posting send in ERROR state...\n");

    ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        printf("  post_send returned error: %s (some drivers reject directly)\n", strerror(ret));
    }

    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 1000000);
    if (ret == 0) {
        printf("  Received WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else if (ret == -2) {
        printf("  CQE timed out (post_send may have been rejected by driver)\n");
    }

    recover_qp(res);
}

/* ========== Scenario D: IBV_WC_RNR_RETRY_EXC_ERR ========== */
static void test_rnr_retry_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ Scenario D: IBV_WC_RNR_RETRY_EXC_ERR (RNR Retry Exceeded) ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  Trigger method: Send without posting recv (rnr_retry=0)\n");
    printf("  rnr_retry=0 means no retry, report error immediately\n\n");

    /* Create QP with rnr_retry=0 (no retry) */
    if (create_and_connect_qp(res, 0) != 0) {
        fprintf(stderr, "  Failed to create QP, skipping this test\n");
        return;
    }

    printf("  QP created (rnr_retry=0)\n");
    printf("  Deliberately not posting recv, sending directly...\n");

    /* Send directly without posting recv first */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xD001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  post_send failed: %s\n", strerror(ret));
        return;
    }

    /* Wait for error CQE (RNR needs timeout) */
    printf("  Waiting for RNR timeout...\n");
    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 5000000);  /* 5 second timeout */
    if (ret == 0) {
        printf("  Received WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else if (ret == -2) {
        printf("  Waiting for CQE timed out (RNR error may need more time)\n");
        printf("  Note: SoftRoCE RNR behavior may differ from hardware\n");
    }

    recover_qp(res);
}

/* ========== Cleanup Resources ========== */
static void cleanup_resources(struct test_resources *res)
{
    if (res->qp)       ibv_destroy_qp(res->qp);
    if (res->send_cq)  ibv_destroy_cq(res->send_cq);
    if (res->recv_cq)  ibv_destroy_cq(res->recv_cq);
    if (res->mr)       ibv_dereg_mr(res->mr);
    if (res->mr2)      ibv_dereg_mr(res->mr2);
    if (res->pd)       ibv_dealloc_pd(res->pd);
    if (res->pd2)      ibv_dealloc_pd(res->pd2);
    if (res->ctx)      ibv_close_device(res->ctx);
    if (res->dev_list) ibv_free_device_list(res->dev_list);
    free(res->buf);
    free(res->buf2);
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    struct test_resources res;

    printf("=== RDMA Error Triggering and Recovery Demo ===\n\n");
    printf("This program will deliberately trigger 4 typical RDMA errors,\n");
    printf("display error information and demonstrate QP recovery flow.\n");

    /* Initialize */
    if (init_resources(&res) != 0) {
        fprintf(stderr, "[Error] Resource initialization failed\n");
        cleanup_resources(&res);
        return 1;
    }

    /* Trigger 4 errors in sequence */
    test_rem_access_err(&res);
    test_loc_prot_err(&res);
    test_wr_flush_err(&res);
    test_rnr_retry_err(&res);

    /* Summary */
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║                      Summary                         ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  RDMA error handling key points:\n");
    printf("  1. Always check wc.status returned by ibv_poll_cq\n");
    printf("  2. Use ibv_wc_status_str() to get readable error description\n");
    printf("  3. QP automatically enters ERROR state on error\n");
    printf("  4. Recovery path: ERROR -> RESET -> INIT -> RTR -> RTS\n");
    printf("  5. FLUSH_ERR is a secondary error, need to find root cause\n");
    printf("  6. Set reasonable rnr_retry (recommend 7=infinite retry)\n");
    printf("  7. Production code should register async event handler (ibv_get_async_event)\n");

    /* Cleanup */
    cleanup_resources(&res);
    printf("\nProgram finished\n");
    return 0;
}
