/**
 * Unsignaled Completion Optimization Comparison Experiment
 *
 * Demonstrates Unsignaled Send optimization technique:
 *   - Mode 1: Every Send has IBV_SEND_SIGNALED, poll CQ one by one
 *   - Mode 2: Only signal every N Sends, batch completion
 *   - Compares performance difference between the two modes
 *
 * Principle:
 *   Each IBV_SEND_SIGNALED Send generates a CQE upon completion,
 *   poll_cq consumes CPU time. If most Sends don't need completion confirmation,
 *   they can be set as unsignaled (without IBV_SEND_SIGNALED), only signaling
 *   at key points (e.g., every N requests).
 *
 *   Key constraints:
 *   - Must signal at least once before SQ is full (otherwise SQ overflows, can't reclaim WQEs)
 *   - The signaled CQE implicitly confirms all previous unsignaled WRs
 *   - Error handling: errors from unsignaled WRs are reported through subsequent signaled WRs
 *
 * Build:
 *   gcc -o unsignaled_send unsignaled_send.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * Run: ./unsignaled_send
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constant Definitions ========== */
#define MSG_SIZE        64          /* Message size (bytes) */
#define NUM_MESSAGES    10000       /* Number of messages per test round */
#define BUFFER_SIZE     4096        /* Buffer size */
#define CQ_DEPTH        256         /* CQ depth */
#define MAX_SEND_WR     128         /* SQ maximum WR count */
#define MAX_RECV_WR     256         /* RQ maximum WR count (need enough recvs) */
#define SIGNAL_INTERVAL 32          /* Signal every N Sends */

/* ========== Get High-Precision Time (nanoseconds) ========== */
static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ========== Batch Post Recv WRs ========== */
static int post_recv_batch(struct ibv_qp *qp, struct ibv_mr *mr,
                           char *buf, int count)
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    int i, ret;

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)(buf + (i % 4) * MSG_SIZE);
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id   = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        ret = ibv_post_recv(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] post_recv #%d failed: errno=%d\n", i, ret);
            return -1;
        }
    }
    return 0;
}

/* ========== Mode 1: All Signaled ========== */
static int test_all_signaled(struct ibv_qp *qp, struct ibv_cq *cq,
                             struct ibv_mr *mr, char *buf, int count)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;  /* Each one generates CQE */

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] Signaled post_send #%d failed: errno=%d\n", i, ret);
            return -1;
        }

        /* Must poll once for each send */
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) return -1;
    }
    return 0;
}

/* ========== Mode 2: Signal Only Every N ========== */
static int test_unsignaled(struct ibv_qp *qp, struct ibv_cq *cq,
                           struct ibv_mr *mr, char *buf, int count,
                           int signal_interval)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;
    int outstanding = 0;  /* Number of posted but unconfirmed WRs */

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;

        /*
         * Signal every signal_interval requests
         * or the last request must signal (to ensure all WRs are confirmed)
         *
         * Key: outstanding WR count must not exceed max_send_wr
         */
        if ((i + 1) % signal_interval == 0 || i == count - 1) {
            wr.send_flags = IBV_SEND_SIGNALED;  /* Generate CQE */
        } else {
            wr.send_flags = 0;                   /* No CQE generated */
        }

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[Error] Unsignaled post_send #%d failed: errno=%d\n", i, ret);
            return -1;
        }

        outstanding++;

        /* When encountering a signaled WR, poll CQ to reclaim all completed WRs */
        if (wr.send_flags & IBV_SEND_SIGNALED) {
            ret = poll_cq_blocking(cq, &wc);
            if (ret != 0) return -1;
            outstanding = 0;  /* Signaled CQE confirms all previous WRs */
        }

        /*
         * Safety check: if outstanding is about to reach max_send_wr,
         * need to force signal even before signal_interval
         * (the logic above via % already prevents overflow,
         *  but production code needs finer-grained control)
         */
    }

    return 0;
}

/* ========== Drain Receive-Side CQ ========== */
static int drain_recv_cq(struct ibv_cq *cq, int count)
{
    struct ibv_wc wc;
    int drained = 0, ne;

    while (drained < count) {
        ne = ibv_poll_cq(cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[Error] drain_recv poll_cq failed\n");
            return -1;
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[Error] recv CQE failed: %s\n",
                        ibv_wc_status_str(wc.status));
                return -1;
            }
            drained++;
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

    printf("=== Unsignaled Completion Optimization Comparison Experiment ===\n\n");

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

    /* ---- Step 2: Allocate resources ---- */
    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "Failed to allocate PD");

    send_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(send_cq, "Failed to create send_cq");

    recv_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(recv_cq, "Failed to create recv_cq");

    send_buf = malloc(BUFFER_SIZE);
    recv_buf = malloc(BUFFER_SIZE);
    CHECK_NULL(send_buf, "Failed to allocate send_buf");
    CHECK_NULL(recv_buf, "Failed to allocate recv_buf");
    memset(send_buf, 'B', BUFFER_SIZE);
    memset(recv_buf, 0, BUFFER_SIZE);

    send_mr = ibv_reg_mr(pd, send_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(send_mr, "Failed to register send_mr");
    recv_mr = ibv_reg_mr(pd, recv_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(recv_mr, "Failed to register recv_mr");

    /* ---- Step 3: Create QP ---- */
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = send_cq;
    qp_init_attr.recv_cq = recv_cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = MAX_SEND_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_RECV_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    /*
     * sq_sig_all = 0: by default don't auto-signal all send WRs
     * We manually control which WRs need signaling
     */
    qp_init_attr.sq_sig_all = 0;

    qp = ibv_create_qp(pd, &qp_init_attr);
    CHECK_NULL(qp, "Failed to create QP");
    printf("[2] Created QP #%u (sq_sig_all=0, max_send_wr=%d)\n",
           qp->qp_num, MAX_SEND_WR);
    printf("    Signal interval: signal every %d Sends\n", SIGNAL_INTERVAL);

    /* ---- Step 4: Loopback connection ---- */
    printf("[3] Loopback connection...\n");
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, RDMA_DEFAULT_PORT_NUM,
                              RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to fill local endpoint info");

    ret = qp_full_connect(qp, &local_ep, RDMA_DEFAULT_PORT_NUM,
                          is_roce, IBV_ACCESS_LOCAL_WRITE);
    CHECK_ERRNO(ret, "QP connection failed");
    print_qp_state(qp);

    /* ========== Test 1: All Signaled ========== */
    printf("\n===== Test 1: All Signaled (poll once per send) =====\n");
    printf("  Message size: %d bytes, message count: %d\n", MSG_SIZE, NUM_MESSAGES);

    /* Post enough recv WRs (in batches) */
    int recv_posted = 0;
    int batch = MAX_RECV_WR;
    while (recv_posted < NUM_MESSAGES) {
        int to_post = (NUM_MESSAGES - recv_posted) < batch ?
                      (NUM_MESSAGES - recv_posted) : batch;
        ret = post_recv_batch(qp, recv_mr, recv_buf, to_post);
        if (ret != 0) goto cleanup;
        recv_posted += to_post;

        /* If there are more to post, drain some recv CQ to free space
         * Note: simplified handling, production code needs finer-grained flow control
         */
        if (recv_posted < NUM_MESSAGES) {
            /* Run some send + poll to consume recv CQ */
            break;
        }
    }

    /* Simplified: post NUM_MESSAGES recvs at once (if RQ is large enough) */
    /* Re-post recv (above logic may be insufficient) */

    uint64_t t1 = get_time_ns();
    ret = test_all_signaled(qp, send_cq, send_mr, send_buf, NUM_MESSAGES);
    uint64_t t2 = get_time_ns();
    if (ret != 0) {
        fprintf(stderr, "[Error] All-signaled test failed\n");
        goto cleanup;
    }

    /* Drain recv-side CQEs */
    drain_recv_cq(recv_cq, NUM_MESSAGES);

    double signaled_us = (double)(t2 - t1) / 1000.0;
    double signaled_per_msg = signaled_us / NUM_MESSAGES;
    printf("  Total time: %.2f us\n", signaled_us);
    printf("  Average per message: %.3f us\n", signaled_per_msg);
    printf("  CQ poll count: %d (poll once per send)\n", NUM_MESSAGES);

    /* ========== Test 2: Unsignaled (signal every N) ========== */
    printf("\n===== Test 2: Unsignaled (signal every %d) =====\n",
           SIGNAL_INTERVAL);
    printf("  Message size: %d bytes, message count: %d\n", MSG_SIZE, NUM_MESSAGES);

    /* Re-post recvs */
    recv_posted = 0;
    batch = MAX_RECV_WR;
    while (recv_posted < NUM_MESSAGES) {
        int to_post = (NUM_MESSAGES - recv_posted) < batch ?
                      (NUM_MESSAGES - recv_posted) : batch;
        ret = post_recv_batch(qp, recv_mr, recv_buf, to_post);
        if (ret != 0) goto cleanup;
        recv_posted += to_post;
        if (recv_posted < NUM_MESSAGES) break;
    }

    uint64_t t3 = get_time_ns();
    ret = test_unsignaled(qp, send_cq, send_mr, send_buf,
                          NUM_MESSAGES, SIGNAL_INTERVAL);
    uint64_t t4 = get_time_ns();
    if (ret != 0) {
        fprintf(stderr, "[Error] Unsignaled test failed\n");
        goto cleanup;
    }

    drain_recv_cq(recv_cq, NUM_MESSAGES);

    double unsig_us = (double)(t4 - t3) / 1000.0;
    double unsig_per_msg = unsig_us / NUM_MESSAGES;
    int poll_count = (NUM_MESSAGES + SIGNAL_INTERVAL - 1) / SIGNAL_INTERVAL;
    printf("  Total time: %.2f us\n", unsig_us);
    printf("  Average per message: %.3f us\n", unsig_per_msg);
    printf("  CQ poll count: ~%d (reduced %dx)\n", poll_count, SIGNAL_INTERVAL);

    /* ========== Results Comparison ========== */
    printf("\n===== Performance Comparison =====\n");
    printf("  ┌───────────────────────┬───────────────┬──────────────┬────────────┐\n");
    printf("  │ Mode                  │ Total (us)    │ Per-msg (us) │ Poll count │\n");
    printf("  ├───────────────────────┼───────────────┼──────────────┼────────────┤\n");
    printf("  │ Signaled (poll each)  │ %13.2f │ %12.3f │ %10d │\n",
           signaled_us, signaled_per_msg, NUM_MESSAGES);
    printf("  │ Unsignaled (every %d) │ %13.2f │ %12.3f │ %10d │\n",
           SIGNAL_INTERVAL, unsig_us, unsig_per_msg, poll_count);
    printf("  └───────────────────────┴───────────────┴──────────────┴────────────┘\n");

    if (unsig_us < signaled_us) {
        double speedup = signaled_us / unsig_us;
        printf("\n  Conclusion: Unsignaled mode is %.2fx faster\n", speedup);
    } else {
        printf("\n  Conclusion: Difference is not significant in current environment\n");
        printf("  Note: In SoftRoCE, poll overhead is small; real hardware shows bigger difference\n");
    }

    printf("\n===== Principle Summary =====\n");
    printf("  1. IBV_SEND_SIGNALED causes Send to generate CQE upon completion, detectable by poll_cq\n");
    printf("  2. Send without SIGNALED does not generate CQE (unsignaled)\n");
    printf("  3. A signaled CQE's completion implicitly confirms all previous unsignaled WRs\n");
    printf("  4. Reduces poll_cq call count, lowering CPU overhead\n");
    printf("  5. Key constraint: must signal before SQ is full (otherwise SQ overflows)\n");
    printf("  6. Recommended strategy: signal_interval = min(max_send_wr/2, 32)\n");
    printf("  7. Error handling: unsignaled WR errors are reported through subsequent signaled WRs\n");

cleanup:
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
