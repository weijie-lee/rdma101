/**
 * UD (Unreliable Datagram) Mode Loopback Demo
 *
 * Features:
 *   - Demonstrates UD mode core concepts: connectionless, Address Handle, GRH header
 *   - Single-machine loopback: creates two UD QPs, one for sending, one for receiving
 *   - Compares with RC mode, highlighting key differences of UD
 *   - Auto-detects IB/RoCE and adapts Address Handle creation
 *
 * Usage:
 *   ./ud_loopback
 *
 * Build:
 *   gcc -o ud_loopback ud_loopback.c -I../../common ../../common/librdma_utils.a -libverbs
 *
 * Key differences between UD and RC:
 *   1. QP type: IBV_QPT_UD (not IBV_QPT_RC)
 *   2. RTR transition is simpler: no need for dest_qp_num, rq_psn, etc. (connectionless!)
 *   3. Sending requires Address Handle: wr.wr.ud.ah / remote_qpn / remote_qkey
 *   4. Receive buffer needs extra 40 bytes for GRH header (auto-filled by NIC)
 *   5. Message size limit: max MTU (typically 4096 bytes)
 *   6. One QP can send to multiple targets (one-to-many)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constant Definitions ========== */

#define MSG_SIZE        64              /* Message size */
#define GRH_SIZE        40              /* GRH (Global Route Header) fixed 40 bytes */
#define RECV_BUF_SIZE   (GRH_SIZE + MSG_SIZE)   /* Receive buffer = GRH + message */
#define IB_PORT         1               /* RDMA port number */
#define GID_INDEX       0               /* GID table index */
#define CQ_DEPTH        16              /* CQ depth */
#define MAX_WR          16              /* QP maximum WR count */
#define UD_QKEY         0x11111111      /* UD Q_Key (sender and receiver must match) */

/* Test message */
#define TEST_MSG        "Hello UD mode! Unreliable Datagram works!"

/* ========== UD Resource Structure ========== */

struct ud_context {
    /* Device and protection domain */
    struct ibv_context      *ctx;           /* Device context */
    struct ibv_pd           *pd;            /* Protection domain (shared) */
    struct ibv_cq           *cq;            /* Completion queue (shared) */

    /* Two UD QPs: one for sending, one for receiving (loopback) */
    struct ibv_qp           *send_qp;       /* Sender QP */
    struct ibv_qp           *recv_qp;       /* Receiver QP */

    /* Memory regions */
    struct ibv_mr           *send_mr;       /* Send buffer MR */
    struct ibv_mr           *recv_mr;       /* Receive buffer MR */
    char                    *send_buf;      /* Send buffer */
    char                    *recv_buf;      /* Receive buffer (includes GRH) */

    /* Address Handle (UD specific) */
    struct ibv_ah           *ah;            /* Address handle: encapsulates target routing info */

    /* Transport layer type */
    enum rdma_transport     transport;      /* IB / RoCE */
    int                     is_roce;        /* Whether in RoCE mode */
};

/* ========== Function Declarations ========== */

static int  init_ud_resources(struct ud_context *uctx);
static int  create_ud_qp(struct ud_context *uctx, struct ibv_qp **qp);
static int  ud_qp_to_rts(struct ibv_qp *qp);
static int  create_address_handle(struct ud_context *uctx);
static int  do_ud_send_recv(struct ud_context *uctx);
static void cleanup_ud_resources(struct ud_context *uctx);

/* ========== Main Function ========== */

int main(void)
{
    struct ud_context uctx;
    int ret = 1;

    memset(&uctx, 0, sizeof(uctx));

    printf("========================================\n");
    printf("  UD (Unreliable Datagram) Loopback Demo\n");
    printf("========================================\n\n");

    /* Step 1: Initialize RDMA resources */
    printf("[Step 1] Initializing RDMA resources...\n");
    if (init_ud_resources(&uctx) != 0) {
        fprintf(stderr, "[Error] RDMA resource initialization failed\n");
        goto cleanup;
    }
    printf("[Step 1] Resource initialization complete ✓\n\n");

    /* Step 2: Create two UD QPs and transition to RTS */
    printf("[Step 2] Creating UD QPs and transitioning state...\n");
    printf("  --- Creating sender QP ---\n");
    if (create_ud_qp(&uctx, &uctx.send_qp) != 0) {
        fprintf(stderr, "[Error] Failed to create sender QP\n");
        goto cleanup;
    }
    printf("  Sender QP number: %u\n", uctx.send_qp->qp_num);

    printf("  --- Creating receiver QP ---\n");
    if (create_ud_qp(&uctx, &uctx.recv_qp) != 0) {
        fprintf(stderr, "[Error] Failed to create receiver QP\n");
        goto cleanup;
    }
    printf("  Receiver QP number: %u\n", uctx.recv_qp->qp_num);

    /*
     * UD QP state transition: RESET -> INIT -> RTR -> RTS
     *
     * Key differences from RC:
     *   - INIT: qp_access_flags used to set Q_Key
     *   - RTR:  No need for dest_qp_num! (because UD is connectionless)
     *   - RTS:  No need for timeout, retry_cnt, etc. (because UD doesn't guarantee reliability)
     *
     * RC's RTR requires:                  UD's RTR only requires:
     *   attr.dest_qp_num = remote_qpn    attr.qp_state = IBV_QPS_RTR
     *   attr.path_mtu = ...              (that simple!)
     *   attr.rq_psn = ...
     *   attr.max_dest_rd_atomic = ...
     *   attr.ah_attr = ...
     */
    printf("  --- Transitioning sender QP to RTS ---\n");
    if (ud_qp_to_rts(uctx.send_qp) != 0) goto cleanup;
    print_qp_state(uctx.send_qp);

    printf("  --- Transitioning receiver QP to RTS ---\n");
    if (ud_qp_to_rts(uctx.recv_qp) != 0) goto cleanup;
    print_qp_state(uctx.recv_qp);

    printf("[Step 2] UD QPs created and ready ✓\n\n");

    /* Step 3: Create Address Handle */
    printf("[Step 3] Creating Address Handle...\n");
    if (create_address_handle(&uctx) != 0) {
        fprintf(stderr, "[Error] Failed to create Address Handle\n");
        goto cleanup;
    }
    printf("[Step 3] Address Handle created ✓\n\n");

    /* Step 4: Perform send/receive test */
    printf("[Step 4] Performing UD Send/Recv test...\n");
    if (do_ud_send_recv(&uctx) != 0) {
        fprintf(stderr, "[Error] UD Send/Recv test failed\n");
        goto cleanup;
    }
    printf("[Step 4] Communication test complete ✓\n\n");

    printf("========================================\n");
    printf("  UD Loopback test passed!\n");
    printf("========================================\n");
    ret = 0;

cleanup:
    cleanup_ud_resources(&uctx);
    return ret;
}

/* ========== Initialize RDMA Resources ========== */

static int init_ud_resources(struct ud_context *uctx)
{
    struct ibv_device **dev_list = NULL;
    int num_devices;

    /* 1. Get device list and open device */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get RDMA device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }
    printf("  Found %d RDMA device(s), using: %s\n",
           num_devices, ibv_get_device_name(dev_list[0]));

    uctx->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(uctx->ctx, "Failed to open RDMA device");

    /* 2. Detect transport layer type */
    uctx->transport = detect_transport(uctx->ctx, IB_PORT);
    uctx->is_roce = (uctx->transport == RDMA_TRANSPORT_ROCE);
    printf("  Transport type: %s\n", transport_str(uctx->transport));

    /* 3. Allocate protection domain (two QPs share the same PD) */
    uctx->pd = ibv_alloc_pd(uctx->ctx);
    CHECK_NULL(uctx->pd, "Failed to allocate protection domain (PD)");

    /* 4. Create completion queue (two QPs share the same CQ) */
    uctx->cq = ibv_create_cq(uctx->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(uctx->cq, "Failed to create completion queue (CQ)");

    /* 5. Allocate and register send buffer */
    uctx->send_buf = (char *)malloc(MSG_SIZE);
    CHECK_NULL(uctx->send_buf, "Failed to allocate send buffer");
    memset(uctx->send_buf, 0, MSG_SIZE);

    uctx->send_mr = ibv_reg_mr(uctx->pd, uctx->send_buf, MSG_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(uctx->send_mr, "Failed to register send MR");

    /*
     * 6. Allocate and register receive buffer
     *
     * *** UD specific: receive buffer needs extra 40 bytes for GRH ***
     *
     * GRH (Global Route Header) contains routing info such as source/destination GID.
     * In UD mode, the NIC automatically prepends a 40-byte GRH to each received message.
     * Even in IB mode (not using GRH routing), these 40 bytes are still reserved.
     *
     * Buffer layout:
     * ┌──────────────────┬──────────────────────┐
     * │  GRH (40 bytes)  │  Actual data (payload)│
     * └──────────────────┴──────────────────────┘
     *
     * Compared to RC mode: receive buffer only needs payload size, no GRH overhead.
     */
    uctx->recv_buf = (char *)malloc(RECV_BUF_SIZE);
    CHECK_NULL(uctx->recv_buf, "Failed to allocate receive buffer");
    memset(uctx->recv_buf, 0, RECV_BUF_SIZE);

    uctx->recv_mr = ibv_reg_mr(uctx->pd, uctx->recv_buf, RECV_BUF_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(uctx->recv_mr, "Failed to register receive MR");

    printf("  Buffers: send=%d bytes, recv=%d bytes (including %d-byte GRH)\n",
           MSG_SIZE, RECV_BUF_SIZE, GRH_SIZE);

    ibv_free_device_list(dev_list);
    return 0;

cleanup:
    if (dev_list) ibv_free_device_list(dev_list);
    return -1;
}

/* ========== Create UD QP ========== */

/**
 * create_ud_qp - Create a UD type QP
 *
 * Differences between UD QP and RC QP creation:
 *   - qp_type = IBV_QPT_UD (not IBV_QPT_RC)
 *   - Other parameters (CQ, max_wr, max_sge) are exactly the same
 */
static int create_ud_qp(struct ud_context *uctx, struct ibv_qp **qp)
{
    struct ibv_qp_init_attr qp_init_attr;

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq  = uctx->cq;
    qp_init_attr.recv_cq  = uctx->cq;

    /*
     * *** UD key difference #1: QP type ***
     *
     * RC: qp_init_attr.qp_type = IBV_QPT_RC;  // Reliable Connected
     * UC: qp_init_attr.qp_type = IBV_QPT_UC;  // Unreliable Connected
     * UD: qp_init_attr.qp_type = IBV_QPT_UD;  // Unreliable Datagram
     */
    qp_init_attr.qp_type  = IBV_QPT_UD;

    qp_init_attr.cap.max_send_wr  = MAX_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    *qp = ibv_create_qp(uctx->pd, &qp_init_attr);
    CHECK_NULL(*qp, "Failed to create UD QP");

    return 0;

cleanup:
    return -1;
}

/* ========== UD QP State Transition: RESET -> INIT -> RTR -> RTS ========== */

/**
 * ud_qp_to_rts - Transition UD QP from RESET to RTS
 *
 * UD mode QP state transition is much simpler than RC:
 *
 *   RC mode:                           UD mode:
 *   ---------                           ---------
 *   RESET -> INIT:                       RESET -> INIT:
 *     port, pkey, access_flags            port, pkey, qkey (!)
 *                                         (access_flags doesn't need remote access)
 *
 *   INIT -> RTR:                         INIT -> RTR:
 *     dest_qp_num (!)                     Only qp_state = RTR
 *     path_mtu                            (No target info needed!)
 *     rq_psn
 *     max_dest_rd_atomic
 *     ah_attr (target address!)
 *
 *   RTR -> RTS:                          RTR -> RTS:
 *     sq_psn                              sq_psn
 *     timeout (!)                         (No timeout, retry_cnt needed)
 *     retry_cnt (!)                       (because UD doesn't guarantee reliability)
 *     rnr_retry (!)
 *     max_rd_atomic
 */
static int ud_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int ret;

    /* Step 1: RESET -> INIT */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state   = IBV_QPS_INIT;
    attr.pkey_index = RDMA_DEFAULT_PKEY_INDEX;
    attr.port_num   = IB_PORT;
    /*
     * UD specific: INIT phase requires setting Q_Key
     * Q_Key is used for UD message access control:
     *   - Sender's wr.wr.ud.remote_qkey must match the receiver QP's qkey
     *   - Messages with mismatched Q_Key will be dropped
     */
    attr.qkey       = UD_QKEY;

    ret = ibv_modify_qp(qp, &attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT | IBV_QP_QKEY);
    CHECK_ERRNO(ret, "UD QP RESET->INIT failed");
    printf("    RESET -> INIT (qkey=0x%x)\n", UD_QKEY);

    /*
     * Step 2: INIT -> RTR
     *
     * *** UD key difference #2: RTR does not need target info! ***
     *
     * RC's RTR requires:                UD's RTR only requires:
     *   dest_qp_num = remote_qpn          qp_state = IBV_QPS_RTR
     *   path_mtu = IBV_MTU_1024           (done!)
     *   rq_psn = remote_psn
     *   max_dest_rd_atomic = 1
     *   ah_attr = { dlid/dgid... }
     *
     * This is the essence of UD being "connectionless": QP is not bound to a specific target.
     */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;

    ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE);
    CHECK_ERRNO(ret, "UD QP INIT->RTR failed");
    printf("    INIT -> RTR (no target info needed, UD is connectionless!)\n");

    /*
     * Step 3: RTR -> RTS
     *
     * UD's RTS is simpler than RC:
     *   - Only needs sq_psn (Send Queue PSN)
     *   - No need for timeout, retry_cnt, rnr_retry (UD doesn't do retransmission)
     */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn   = RDMA_DEFAULT_PSN;

    ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
    CHECK_ERRNO(ret, "UD QP RTR->RTS failed");
    printf("    RTR -> RTS (sq_psn=%u)\n", RDMA_DEFAULT_PSN);

    return 0;

cleanup:
    return -1;
}

/* ========== Create Address Handle ========== */

/**
 * create_address_handle - Create UD mode Address Handle
 *
 * Address Handle (AH) is the core concept of UD mode:
 *   - RC mode: target routing info is bound in QP's RTR attributes (one-to-one)
 *   - UD mode: target routing info is encapsulated in AH, specified at send time (one-to-many)
 *
 * AH contains:
 *   - dlid:       target LID (IB mode)
 *   - is_global:  whether to use GRH (must be 1 for RoCE)
 *   - grh.dgid:   target GID (RoCE mode)
 *   - sl:         Service Level
 *   - port_num:   local egress port
 *
 * This example is loopback, so AH points to self (own LID/GID).
 */
static int create_address_handle(struct ud_context *uctx)
{
    struct ibv_ah_attr ah_attr;
    struct ibv_port_attr port_attr;
    union ibv_gid local_gid;

    /* Query local port attributes to get LID */
    if (ibv_query_port(uctx->ctx, IB_PORT, &port_attr) != 0) {
        fprintf(stderr, "[Error] Failed to query port attributes\n");
        goto cleanup;
    }

    /* Query local GID */
    if (ibv_query_gid(uctx->ctx, IB_PORT, GID_INDEX, &local_gid) != 0) {
        fprintf(stderr, "[Error] Failed to query GID[%d]\n", GID_INDEX);
        goto cleanup;
    }

    /* Build AH attributes */
    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.sl       = RDMA_DEFAULT_SL;
    ah_attr.port_num = IB_PORT;

    if (uctx->is_roce) {
        /*
         * RoCE mode: must set is_global=1 and fill GRH
         *
         * Loopback: dgid = local GID (send to self)
         * Real use: dgid = remote GID (send to peer)
         */
        ah_attr.is_global         = 1;
        ah_attr.grh.dgid          = local_gid;     /* Loopback: target = self */
        ah_attr.grh.sgid_index    = GID_INDEX;
        ah_attr.grh.hop_limit     = 64;
        ah_attr.grh.traffic_class = 0;
        ah_attr.grh.flow_label    = 0;

        char gid_str[46];
        gid_to_str(&local_gid, gid_str, sizeof(gid_str));
        printf("  RoCE mode AH: dgid=%s\n", gid_str);
    } else {
        /*
         * IB mode: use LID addressing
         *
         * Loopback: dlid = local LID (send to self)
         * Real use: dlid = remote LID (send to peer)
         */
        ah_attr.is_global = 0;
        ah_attr.dlid      = port_attr.lid;          /* Loopback: target = self */

        printf("  IB mode AH: dlid=%u\n", port_attr.lid);
    }

    /* Create Address Handle */
    uctx->ah = ibv_create_ah(uctx->pd, &ah_attr);
    CHECK_NULL(uctx->ah, "Failed to create Address Handle (AH)");
    printf("  Address Handle created successfully\n");

    return 0;

cleanup:
    return -1;
}

/* ========== UD Send/Recv Test ========== */

/**
 * do_ud_send_recv - Perform UD mode Send/Recv test
 *
 * Flow:
 *   1. Post Recv on receiver QP (buffer size = GRH + MSG)
 *   2. Post Send on sender QP (specify AH, remote_qpn, remote_qkey)
 *   3. Poll CQ waiting for Send completion
 *   4. Poll CQ waiting for Recv completion
 *   5. Read data (skip first 40-byte GRH)
 */
static int do_ud_send_recv(struct ud_context *uctx)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    int ret;

    /* ---- Step 1: Post Recv on receiver QP ---- */
    /*
     * *** UD key difference #4: receive buffer needs extra 40-byte GRH ***
     *
     * RC mode: sge.length = MSG_SIZE;             // Only needs message size
     * UD mode: sge.length = GRH_SIZE + MSG_SIZE;  // First 40 bytes for GRH
     *
     * If the receive buffer doesn't reserve GRH space, the NIC will drop the
     * message because the buffer is too small.
     */
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)uctx->recv_buf;
    sge.length = RECV_BUF_SIZE;             /* GRH(40) + MSG_SIZE */
    sge.lkey   = uctx->recv_mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(uctx->recv_qp, &recv_wr, &bad_recv_wr);
    CHECK_ERRNO(ret, "Post Recv failed");
    printf("  Post Recv complete (receiver QP #%u, buffer %d bytes)\n",
           uctx->recv_qp->qp_num, RECV_BUF_SIZE);

    /* ---- Step 2: Prepare send data ---- */
    strncpy(uctx->send_buf, TEST_MSG, MSG_SIZE - 1);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)uctx->send_buf;
    sge.length = (uint32_t)(strlen(TEST_MSG) + 1);
    sge.lkey   = uctx->send_mr->lkey;

    /*
     * *** UD key difference #3: Send WR requires Address Handle ***
     *
     * RC mode Send WR:               UD mode Send WR:
     *   wr.opcode = IBV_WR_SEND          wr.opcode = IBV_WR_SEND
     *   (no need to specify target,       wr.wr.ud.ah = ah;          // Target routing
     *    because QP is bound to target)   wr.wr.ud.remote_qpn = N;  // Target QP
     *                                     wr.wr.ud.remote_qkey = K;  // Q_Key matching
     */
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    /* UD specific: set target info */
    send_wr.wr.ud.ah          = uctx->ah;              /* Address handle (target routing) */
    send_wr.wr.ud.remote_qpn  = uctx->recv_qp->qp_num; /* Target QP number */
    send_wr.wr.ud.remote_qkey = UD_QKEY;               /* Q_Key (must match) */

    /* ---- Step 3: Post Send ---- */
    ret = ibv_post_send(uctx->send_qp, &send_wr, &bad_send_wr);
    CHECK_ERRNO(ret, "Post Send failed");
    printf("  Post Send complete (sender QP #%u -> receiver QP #%u)\n",
           uctx->send_qp->qp_num, uctx->recv_qp->qp_num);
    printf("    AH target: %s, remote_qpn=%u, remote_qkey=0x%x\n",
           uctx->is_roce ? "GID" : "LID",
           uctx->recv_qp->qp_num, UD_QKEY);

    /* ---- Step 4: Poll CQ waiting for Send completion ---- */
    printf("\n  Waiting for Send completion...\n");
    ret = poll_cq_blocking(uctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Send) failed");
    printf("  Send complete: wr_id=%lu, status=%s\n",
           (unsigned long)wc.wr_id, ibv_wc_status_str(wc.status));
    print_wc_detail(&wc);

    /* ---- Step 5: Poll CQ waiting for Recv completion ---- */
    printf("  Waiting for Recv completion...\n");
    ret = poll_cq_blocking(uctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Recv) failed");
    printf("  Recv complete: wr_id=%lu, byte_len=%u\n",
           (unsigned long)wc.wr_id, wc.byte_len);

    /*
     * UD Recv WC extra info:
     *   wc.src_qp:  sender's QP number
     *   wc.slid:    sender's LID (IB mode)
     *   wc.byte_len: total length including GRH
     */
    printf("  Recv WC details: src_qp=%u, slid=%u, total_bytes=%u (including GRH %d)\n",
           wc.src_qp, wc.slid, wc.byte_len, GRH_SIZE);
    print_wc_detail(&wc);

    /* ---- Step 6: Read data (skip GRH) ---- */
    /*
     * *** UD key difference #4: actual data starts at offset 40 ***
     *
     * RC mode: payload = recv_buf;           // Starts from buffer head
     * UD mode: payload = recv_buf + 40;      // Skip 40-byte GRH
     *
     * GRH contents (40 bytes):
     *   - Version, traffic class, flow label (4 bytes)
     *   - Payload length, next header, hop limit (4 bytes)
     *   - Source GID (16 bytes)
     *   - Destination GID (16 bytes)
     */
    char *payload = uctx->recv_buf + GRH_SIZE;
    printf("\n  ╔══════════════════════════════════════════════╗\n");
    printf("  ║  [GRH header: %d bytes] (automatically skipped)\n", GRH_SIZE);
    printf("  ║  Received message: \"%s\"\n", payload);
    printf("  ╚══════════════════════════════════════════════╝\n");

    /* Verify message content */
    if (strcmp(payload, TEST_MSG) == 0) {
        printf("\n  Message verification successful! UD Loopback works correctly.\n");
    } else {
        fprintf(stderr, "\n  [Error] Message mismatch! Expected: \"%s\", received: \"%s\"\n",
                TEST_MSG, payload);
        goto cleanup;
    }

    return 0;

cleanup:
    return -1;
}

/* ========== Cleanup Resources ========== */

static void cleanup_ud_resources(struct ud_context *uctx)
{
    printf("\n[Cleanup] Releasing UD resources...\n");

    /* Address Handle */
    if (uctx->ah)       { ibv_destroy_ah(uctx->ah);       uctx->ah = NULL; }

    /* MR and buffers */
    if (uctx->recv_mr)  { ibv_dereg_mr(uctx->recv_mr);    uctx->recv_mr = NULL; }
    if (uctx->send_mr)  { ibv_dereg_mr(uctx->send_mr);    uctx->send_mr = NULL; }
    if (uctx->recv_buf) { free(uctx->recv_buf);            uctx->recv_buf = NULL; }
    if (uctx->send_buf) { free(uctx->send_buf);            uctx->send_buf = NULL; }

    /* QP */
    if (uctx->send_qp)  { ibv_destroy_qp(uctx->send_qp);  uctx->send_qp = NULL; }
    if (uctx->recv_qp)  { ibv_destroy_qp(uctx->recv_qp);  uctx->recv_qp = NULL; }

    /* CQ, PD, device */
    if (uctx->cq)        { ibv_destroy_cq(uctx->cq);       uctx->cq = NULL; }
    if (uctx->pd)        { ibv_dealloc_pd(uctx->pd);       uctx->pd = NULL; }
    if (uctx->ctx)       { ibv_close_device(uctx->ctx);    uctx->ctx = NULL; }

    printf("[Cleanup] Done\n");
}
