/**
 * Event-Driven CQ Completion Notification Demo
 *
 * This program demonstrates the event-driven CQ notification mechanism
 * based on Completion Channel:
 *   1. Create ibv_comp_channel (completion notification channel)
 *   2. Create CQ and associate it with the channel
 *   3. Use ibv_req_notify_cq() to register notification (arm CQ)
 *   4. Use ibv_get_cq_event() to block-wait for completion events
 *   5. Use ibv_ack_cq_events() to acknowledge events
 *   6. Trigger real completion events via loopback Send/Recv
 *   7. Compare with busy polling approach
 *
 * Compile: gcc -o cq_event_driven cq_event_driven.c -I../../common ../../common/librdma_utils.a -libverbs -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE 4096
#define PORT_NUM    RDMA_DEFAULT_PORT_NUM
#define CQ_DEPTH    32
#define MSG_SIZE    64

int main(int argc, char *argv[])
{
    /* Resource declarations */
    struct ibv_device      **dev_list = NULL;
    struct ibv_context      *ctx      = NULL;
    struct ibv_pd           *pd       = NULL;
    struct ibv_comp_channel *channel  = NULL;  /* Completion notification channel */
    struct ibv_cq           *cq_event = NULL;  /* Event-driven CQ */
    struct ibv_cq           *cq_poll  = NULL;  /* Busy-polling CQ (for comparison) */
    struct ibv_qp           *qp       = NULL;
    struct ibv_mr           *mr       = NULL;
    char                    *buffer   = NULL;
    int                      num_devices;
    int                      ret;
    unsigned int             events_completed = 0;  /* Acknowledged event count */

    printf("============================================\n");
    printf("  Event-Driven CQ Completion Notification Demo\n");
    printf("============================================\n\n");

    /*
     * Background: Two modes of CQ completion notification
     *
     * Mode 1: Busy Polling
     *   while (1) { ne = ibv_poll_cq(cq, 1, &wc); if (ne > 0) break; }
     *   + Pros: Lowest latency (nanosecond level)
     *   - Cons: 100% CPU usage, wastes CPU resources
     *
     * Mode 2: Event-Driven
     *   ibv_req_notify_cq(cq, 0);        // Register notification
     *   ibv_get_cq_event(channel, ...);   // Block-wait
     *   ibv_poll_cq(cq, ...);             // Poll after receiving notification
     *   ibv_ack_cq_events(cq, 1);         // Acknowledge event
     *   + Pros: CPU-friendly, suitable for low-frequency scenarios
     *   - Cons: Extra latency (microsecond level), requires kernel involvement
     *
     * In practice, a hybrid mode is often used: busy-poll for a while first,
     * then switch to event-wait if no events arrive.
     */

    /* ========== Step 1: Initialize basic resources ========== */
    printf("[Step 1] Initialize basic RDMA resources\n");
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

    /* ========== Step 2: Create completion notification channel ========== */
    printf("[Step 2] Create Completion Notification Channel\n");

    /*
     * ibv_create_comp_channel() creates an fd (file descriptor),
     * which can be used with select/poll/epoll for I/O multiplexing.
     * When a CQ event arrives, this fd becomes readable.
     */
    channel = ibv_create_comp_channel(ctx);
    CHECK_NULL(channel, "Failed to create Completion Channel");
    printf("  Channel created successfully, fd=%d\n", channel->fd);
    printf("  -> Can be used with select/poll/epoll multiplexing\n\n");

    /* ========== Step 3: Create event-driven CQ ========== */
    printf("[Step 3] Create event-driven CQ (associated with Channel)\n");

    /*
     * The 4th parameter of ibv_create_cq() is comp_channel:
     *   - NULL: No channel association (busy polling only)
     *   - channel: Associate channel (supports event notification)
     *
     * The 5th parameter comp_vector: selects interrupt vector (multi-core load balancing)
     */
    cq_event = ibv_create_cq(ctx, CQ_DEPTH, NULL, channel, 0);
    CHECK_NULL(cq_event, "Failed to create event-driven CQ");
    printf("  Event-driven CQ created (depth=%d, associated channel fd=%d)\n",
           CQ_DEPTH, channel->fd);

    /* Also create a regular CQ (for comparison) */
    cq_poll = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq_poll, "Failed to create polling CQ");
    printf("  Polling CQ created (no channel, busy polling only)\n\n");

    /* ========== Step 4: Create QP and connect (loopback) ========== */
    printf("[Step 4] Create QP and establish loopback connection\n");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq_event,   /* Send completions go to event-driven CQ */
        .recv_cq = cq_event,   /* Recv completions also go to event-driven CQ */
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

    /* loopback connection */
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to fill endpoint info");

    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  Transport type: %s\n", transport_str(transport));

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("  QP connection failed, skipping event notification test\n");
        printf("  -> Hint: Ensure port state is ACTIVE\n");
        goto cleanup;
    }
    printf("  QP connected (loopback, RTS state)\n\n");

    /* ========== Step 5: Event-driven completion notification flow ========== */
    printf("========================================\n");
    printf("[Step 5] Event-driven completion notification flow\n");
    printf("========================================\n\n");

    /*
     * Step 5a: Post Recv first (receiver ready)
     */
    printf("  5a. Post Recv (prepare receive buffer)\n");
    struct ibv_sge recv_sge = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr recv_wr = {
        .wr_id   = 1000,
        .sg_list = &recv_sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv = NULL;
    ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
    CHECK_ERRNO(ret, "ibv_post_recv failed");
    printf("      Recv WR submitted (wr_id=1000)\n");

    /*
     * Step 5b: Arm CQ -- register completion notification
     *
     * ibv_req_notify_cq(cq, solicited_only):
     *   - solicited_only=0: Notify on all completion events
     *   - solicited_only=1: Only notify on completions with Solicited flag
     *
     * Note: Must be called before ibv_get_cq_event()!
     *        Arm is one-shot, must re-arm after each get_event.
     */
    printf("  5b. Arm CQ (ibv_req_notify_cq)\n");
    ret = ibv_req_notify_cq(cq_event, 0);
    CHECK_ERRNO(ret, "ibv_req_notify_cq failed");
    printf("      CQ notification registered (solicited_only=0)\n");

    /*
     * Step 5c: Post Send (trigger completion event)
     */
    printf("  5c. Post Send (trigger loopback Send/Recv)\n");
    snprintf(buffer + MSG_SIZE, MSG_SIZE, "Hello RDMA Event-Driven!");
    struct ibv_sge send_sge = {
        .addr   = (uintptr_t)(buffer + MSG_SIZE),
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id      = 2000,
        .sg_list    = &send_sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send = NULL;
    ret = ibv_post_send(qp, &send_wr, &bad_send);
    CHECK_ERRNO(ret, "ibv_post_send failed");
    printf("      Send WR submitted (wr_id=2000)\n");

    /*
     * Step 5d: Block-wait for CQ event
     *
     * ibv_get_cq_event() blocks until:
     *   - A new completion event arrives on the CQ
     *   - The underlying fd becomes readable
     *
     * In production, poll()/epoll() is often used to monitor channel->fd,
     * with timeout mechanisms to avoid permanent blocking.
     */
    printf("  5d. Waiting for CQ event (ibv_get_cq_event, blocking)...\n");

    struct ibv_cq *ev_cq = NULL;
    void *ev_ctx = NULL;
    ret = ibv_get_cq_event(channel, &ev_cq, &ev_ctx);
    CHECK_ERRNO(ret, "ibv_get_cq_event failed");
    events_completed++;
    printf("      Received CQ event notification! (ev_cq=%p)\n", (void *)ev_cq);

    /*
     * Step 5e: After receiving notification, poll CQ to get all completion events
     *
     * Important: A single CQ event notification may correspond to multiple WCs
     * (completion event coalescing). Must loop poll until 0 is returned to ensure
     * all completion events are retrieved.
     */
    printf("  5e. Poll CQ to get completion events\n");
    struct ibv_wc wc;
    int ne, total = 0;
    while ((ne = ibv_poll_cq(cq_event, 1, &wc)) > 0) {
        total++;
        printf("      WC[%d]: wr_id=%lu, status=%s, opcode=%s\n",
               total, (unsigned long)wc.wr_id,
               ibv_wc_status_str(wc.status),
               wc_opcode_str(wc.opcode));
    }
    printf("      Received %d completion event(s) in total\n", total);

    /*
     * Step 5f: Acknowledge CQ events (required!)
     *
     * ibv_ack_cq_events() must be called before ibv_destroy_cq()
     * to acknowledge all received events. If not acknowledged,
     * ibv_destroy_cq() will block.
     *
     * Can batch acknowledge: accumulate N get_events, then ack_events(cq, N).
     */
    printf("  5f. Acknowledge CQ events (ibv_ack_cq_events)\n");
    ibv_ack_cq_events(cq_event, events_completed);
    printf("      Acknowledged %u event(s)\n", events_completed);
    events_completed = 0;

    /*
     * Step 5g: Re-arm CQ (if continuing to listen)
     *
     * Arm is one-shot! Must re-arm after each get_event.
     * Recommended to re-arm immediately after polling CQ to minimize
     * the notification miss window.
     */
    printf("  5g. Re-arm CQ (if continuing to listen)\n");
    ret = ibv_req_notify_cq(cq_event, 0);
    CHECK_ERRNO(ret, "re-arm ibv_req_notify_cq failed");
    printf("      CQ re-armed, ready to wait for next round of events\n");

    /* ========== Summary ========== */
    printf("\n============================================\n");
    printf("  Event-Driven CQ Summary\n");
    printf("============================================\n");
    printf("  Complete flow:\n");
    printf("    1. ibv_create_comp_channel()      -> Create channel\n");
    printf("    2. ibv_create_cq(..., channel, 0)  -> Associate CQ with channel\n");
    printf("    3. ibv_req_notify_cq(cq, 0)        -> Arm CQ\n");
    printf("    4. ibv_post_send/recv(...)          -> Submit WR\n");
    printf("    5. ibv_get_cq_event(channel, ...)   -> Block-wait\n");
    printf("    6. ibv_poll_cq() loop               -> Retrieve all WCs\n");
    printf("    7. ibv_ack_cq_events(cq, N)        -> Acknowledge events\n");
    printf("    8. Go back to step 3 (re-arm)\n\n");
    printf("  Busy polling vs Event-driven:\n");
    printf("    Busy polling: latency ~100ns, CPU 100%%\n");
    printf("    Event-driven: latency ~5-10us, CPU nearly 0\n");
    printf("    Hybrid mode: poll N times first, switch to event-wait if nothing arrives\n\n");

cleanup:
    printf("[Cleanup] Releasing resources...\n");

    /* If there are unacknowledged events, must acknowledge first */
    if (events_completed > 0 && cq_event) {
        ibv_ack_cq_events(cq_event, events_completed);
    }

    if (qp)       ibv_destroy_qp(qp);
    if (cq_event) ibv_destroy_cq(cq_event);
    if (cq_poll)  ibv_destroy_cq(cq_poll);
    if (channel)  ibv_destroy_comp_channel(channel);
    if (mr)       ibv_dereg_mr(mr);
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  Done\n");
    return 0;
}
