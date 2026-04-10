/**
 * Manual TCP Exchange Connection Example -- RDMA RC Mode Dual-Process Connection
 *
 * Features:
 *   - Server/client dual mode, selected via command-line arguments
 *   - Auto-detects InfiniBand / RoCE transport type
 *   - Exchanges RDMA endpoint info (QPN, LID, GID, PSN) via TCP Socket
 *   - Uses qp_full_connect() to complete QP state transitions (RESET → INIT → RTR → RTS)
 *   - Performs bidirectional Send/Recv test after connection to verify
 *
 * Usage:
 *   Server: ./manual_connect -s [tcp_port]
 *   Client: ./manual_connect -c <server_ip> [tcp_port]
 *
 * Build:
 *   gcc -o manual_connect manual_connect.c -I../../common ../../common/librdma_utils.a -libverbs
 *
 * Run example:
 *   Terminal 1: ./manual_connect -s 7471
 *   Terminal 2: ./manual_connect -c 192.168.1.100 7471
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constant Definitions ========== */

#define BUFFER_SIZE     1024            /* Send/receive buffer size */
#define DEFAULT_PORT    7471            /* Default TCP port */
#define IB_PORT         1               /* RDMA port number */
#define GID_INDEX       0               /* GID table index (RoCE v2 typically uses 1 or 3) */
#define CQ_DEPTH        16              /* CQ depth */
#define MAX_WR          16              /* QP maximum WR count */

/* Message sent by server */
#define SERVER_MSG      "Hello from server!"
/* Message sent by client */
#define CLIENT_MSG      "Hello from client!"

/* ========== RDMA Resource Structure ========== */

struct rdma_context {
    /* Device and protection domain */
    struct ibv_context      *ctx;           /* Device context */
    struct ibv_pd           *pd;            /* Protection domain */
    struct ibv_cq           *cq;            /* Completion queue */
    struct ibv_qp           *qp;            /* Queue pair (RC type) */

    /* Memory regions */
    struct ibv_mr           *send_mr;       /* Send buffer MR */
    struct ibv_mr           *recv_mr;       /* Receive buffer MR */
    char                    *send_buf;      /* Send buffer */
    char                    *recv_buf;      /* Receive buffer */

    /* Connection info */
    struct rdma_endpoint    local_ep;       /* Local endpoint info */
    struct rdma_endpoint    remote_ep;      /* Remote endpoint info */

    /* Transport layer type */
    enum rdma_transport     transport;      /* IB / RoCE */
    int                     is_roce;        /* Whether in RoCE mode */

    /* Runtime parameters */
    int                     is_server;      /* Whether in server mode */
    const char              *server_ip;     /* Server IP (client mode) */
    int                     tcp_port;       /* TCP port number */
};

/* ========== Function Declarations ========== */

static void print_usage(const char *prog);
static int  parse_args(struct rdma_context *rctx, int argc, char *argv[]);
static int  init_rdma_resources(struct rdma_context *rctx);
static int  exchange_and_connect(struct rdma_context *rctx);
static int  do_send_recv_test(struct rdma_context *rctx);
static void cleanup_rdma_resources(struct rdma_context *rctx);
static void print_endpoint(const char *label, const struct rdma_endpoint *ep,
                           int is_roce);

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    struct rdma_context rctx;
    int ret = 1;

    memset(&rctx, 0, sizeof(rctx));
    rctx.tcp_port = DEFAULT_PORT;

    /* Step 1: Parse command-line arguments */
    if (parse_args(&rctx, argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  RDMA Manual Connection Example (RC Send/Recv)\n");
    printf("  Mode: %s\n", rctx.is_server ? "Server" : "Client");
    printf("  TCP Port: %d\n", rctx.tcp_port);
    printf("========================================\n\n");

    /* Step 2: Initialize RDMA resources */
    printf("[Step 1] Initializing RDMA resources...\n");
    if (init_rdma_resources(&rctx) != 0) {
        fprintf(stderr, "[Error] RDMA resource initialization failed\n");
        goto cleanup;
    }
    printf("[Step 1] RDMA resource initialization complete ✓\n\n");

    /* Step 3: Exchange endpoint info and establish connection */
    printf("[Step 2] Exchanging endpoint info and establishing QP connection...\n");
    if (exchange_and_connect(&rctx) != 0) {
        fprintf(stderr, "[Error] QP connection establishment failed\n");
        goto cleanup;
    }
    printf("[Step 2] QP connection established ✓\n\n");

    /* Step 4: Perform bidirectional Send/Recv test */
    printf("[Step 3] Performing bidirectional Send/Recv communication test...\n");
    if (do_send_recv_test(&rctx) != 0) {
        fprintf(stderr, "[Error] Send/Recv test failed\n");
        goto cleanup;
    }
    printf("[Step 3] Communication test complete ✓\n\n");

    printf("========================================\n");
    printf("  All tests passed! RDMA connection works correctly.\n");
    printf("========================================\n");
    ret = 0;

cleanup:
    cleanup_rdma_resources(&rctx);
    return ret;
}

/* ========== Print Usage ========== */

static void print_usage(const char *prog)
{
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "  Server: %s -s [tcp_port]\n", prog);
    fprintf(stderr, "  Client: %s -c <server_ip> [tcp_port]\n", prog);
    fprintf(stderr, "\nArguments:\n");
    fprintf(stderr, "  -s           Server mode (listen and wait for connection)\n");
    fprintf(stderr, "  -c <ip>      Client mode (connect to specified server)\n");
    fprintf(stderr, "  tcp_port     TCP port number (default: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -s 7471\n", prog);
    fprintf(stderr, "  %s -c 192.168.1.100 7471\n", prog);
    fprintf(stderr, "\n");
}

/* ========== Parse Command-Line Arguments ========== */

static int parse_args(struct rdma_context *rctx, int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }

    /* Parse -s (server) or -c (client) mode */
    if (strcmp(argv[1], "-s") == 0) {
        /* Server mode */
        rctx->is_server = 1;
        rctx->server_ip = NULL;
        /* Optional: TCP port */
        if (argc >= 3) {
            rctx->tcp_port = atoi(argv[2]);
        }
    } else if (strcmp(argv[1], "-c") == 0) {
        /* Client mode: server IP required */
        if (argc < 3) {
            fprintf(stderr, "[Error] Client mode requires server IP\n");
            return -1;
        }
        rctx->is_server = 0;
        rctx->server_ip = argv[2];
        /* Optional: TCP port */
        if (argc >= 4) {
            rctx->tcp_port = atoi(argv[3]);
        }
    } else {
        return -1;
    }

    return 0;
}

/* ========== Initialize RDMA Resources ========== */

static int init_rdma_resources(struct rdma_context *rctx)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    int num_devices;
    int ret = -1;

    /* 1. Get device list and open the first device */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get RDMA device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }
    printf("  Found %d RDMA device(s), using: %s\n",
           num_devices, ibv_get_device_name(dev_list[0]));

    rctx->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(rctx->ctx, "Failed to open RDMA device");

    /* 2. Detect transport layer type (IB / RoCE) */
    rctx->transport = detect_transport(rctx->ctx, IB_PORT);
    rctx->is_roce = (rctx->transport == RDMA_TRANSPORT_ROCE);
    printf("  Transport type: %s\n", transport_str(rctx->transport));

    /* 3. Allocate protection domain (PD) */
    rctx->pd = ibv_alloc_pd(rctx->ctx);
    CHECK_NULL(rctx->pd, "Failed to allocate protection domain (PD)");

    /* 4. Create completion queue (CQ) */
    rctx->cq = ibv_create_cq(rctx->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(rctx->cq, "Failed to create completion queue (CQ)");

    /* 5. Create RC type queue pair (QP) */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq  = rctx->cq;
    qp_init_attr.recv_cq  = rctx->cq;
    qp_init_attr.qp_type  = IBV_QPT_RC;    /* RC: Reliable Connected */
    qp_init_attr.cap.max_send_wr  = MAX_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    rctx->qp = ibv_create_qp(rctx->pd, &qp_init_attr);
    CHECK_NULL(rctx->qp, "Failed to create queue pair (QP)");
    printf("  QP number: %u (type: RC)\n", rctx->qp->qp_num);

    /* Print initial QP state (should be RESET) */
    print_qp_state(rctx->qp);

    /* 6. Allocate and register send buffer */
    rctx->send_buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(rctx->send_buf, "Failed to allocate send buffer");
    memset(rctx->send_buf, 0, BUFFER_SIZE);

    rctx->send_mr = ibv_reg_mr(rctx->pd, rctx->send_buf, BUFFER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(rctx->send_mr, "Failed to register send MR");

    /* 7. Allocate and register receive buffer */
    rctx->recv_buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(rctx->recv_buf, "Failed to allocate receive buffer");
    memset(rctx->recv_buf, 0, BUFFER_SIZE);

    rctx->recv_mr = ibv_reg_mr(rctx->pd, rctx->recv_buf, BUFFER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(rctx->recv_mr, "Failed to register receive MR");

    printf("  MR registration complete: send_lkey=0x%x, recv_lkey=0x%x\n",
           rctx->send_mr->lkey, rctx->recv_mr->lkey);

    ret = 0;

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }
    return ret;
}

/* ========== Print Endpoint Info ========== */

static void print_endpoint(const char *label, const struct rdma_endpoint *ep,
                           int is_roce)
{
    char gid_str[46];
    gid_to_str(&ep->gid, gid_str, sizeof(gid_str));

    printf("  %s endpoint info:\n", label);
    printf("    QP number (qp_num): %u\n", ep->qp_num);
    printf("    Port (port):        %u\n", ep->port_num);
    printf("    PSN:                %u\n", ep->psn);
    if (is_roce) {
        printf("    GID index:          %u\n", ep->gid_index);
        printf("    GID:                %s\n", gid_str);
    } else {
        printf("    LID:                %u\n", ep->lid);
    }
}

/* ========== Exchange Endpoint Info and Establish QP Connection ========== */

static int exchange_and_connect(struct rdma_context *rctx)
{
    int ret;

    /* 1. Fill local endpoint info (QPN, LID, GID, PSN, etc.) */
    ret = fill_local_endpoint(rctx->ctx, rctx->qp, IB_PORT,
                              GID_INDEX, &rctx->local_ep);
    CHECK_ERRNO(ret, "Failed to fill local endpoint info");

    printf("\n");
    print_endpoint("Local", &rctx->local_ep, rctx->is_roce);

    /* 2. Exchange endpoint info via TCP Socket */
    printf("\n  Exchanging endpoint info via TCP...\n");
    /*
     * exchange_endpoint_tcp():
     *   server_ip = NULL → Server mode (listen)
     *   server_ip = "x.x.x.x" → Client mode (connect)
     */
    ret = exchange_endpoint_tcp(rctx->server_ip, rctx->tcp_port,
                                &rctx->local_ep, &rctx->remote_ep);
    CHECK_ERRNO(ret, "TCP endpoint info exchange failed");

    printf("\n");
    print_endpoint("Remote", &rctx->remote_ep, rctx->is_roce);

    /* 3. Use qp_full_connect() to complete QP state transitions */
    /*
     * qp_full_connect() internally performs:
     *   RESET → INIT (set port and access permissions)
     *   INIT  → RTR  (set remote address, ready to receive)
     *   RTR   → RTS  (ready to send)
     *
     * For IB mode: uses LID addressing
     * For RoCE mode: uses GID addressing (is_global=1)
     */
    printf("\n  Performing QP state transitions (RESET → INIT → RTR → RTS)...\n");
    int access_flags = IBV_ACCESS_LOCAL_WRITE |
                       IBV_ACCESS_REMOTE_READ |
                       IBV_ACCESS_REMOTE_WRITE;

    ret = qp_full_connect(rctx->qp, &rctx->remote_ep,
                          IB_PORT, rctx->is_roce, access_flags);
    CHECK_ERRNO(ret, "QP connection failed (qp_full_connect)");

    /* Print final QP state (should be RTS) */
    print_qp_state(rctx->qp);
    printf("  QP connection established successfully! Transport type: %s\n", transport_str(rctx->transport));

    return 0;

cleanup:
    return -1;
}

/* ========== Perform Bidirectional Send/Recv Test ========== */

static int do_send_recv_test(struct rdma_context *rctx)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    int ret;

    /*
     * Communication flow:
     *   1. Both ends first Post Recv (prepare to receive)
     *   2. Server sends "Hello from server!"
     *   3. Client sends "Hello from client!"
     *   4. Both ends Poll CQ to get Send and Recv completions
     */

    /* ---- Step 1: Post Recv (prepare to receive peer's message) ---- */
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)rctx->recv_buf;
    sge.length = BUFFER_SIZE;
    sge.lkey   = rctx->recv_mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;       /* Use wr_id=1 to identify Recv */
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(rctx->qp, &recv_wr, &bad_recv_wr);
    CHECK_ERRNO(ret, "Post Recv failed");
    printf("  Post Recv complete (waiting for peer's message)\n");

    /* ---- Step 2: Prepare send message ---- */
    const char *msg = rctx->is_server ? SERVER_MSG : CLIENT_MSG;
    strncpy(rctx->send_buf, msg, BUFFER_SIZE - 1);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)rctx->send_buf;
    sge.length = (uint32_t)(strlen(msg) + 1);  /* Including '\0' */
    sge.lkey   = rctx->send_mr->lkey;

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;    /* Use wr_id=2 to identify Send */
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;     /* Request completion notification */

    /* ---- Step 3: Post Send ---- */
    ret = ibv_post_send(rctx->qp, &send_wr, &bad_send_wr);
    CHECK_ERRNO(ret, "Post Send failed");
    printf("  Post Send complete (sent: \"%s\")\n", msg);

    /* ---- Step 4: Poll CQ waiting for Send completion ---- */
    printf("\n  Waiting for Send completion...\n");
    ret = poll_cq_blocking(rctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Send) failed");
    printf("  Send complete:\n");
    print_wc_detail(&wc);

    /* ---- Step 5: Poll CQ waiting for Recv completion ---- */
    printf("  Waiting for Recv completion...\n");
    ret = poll_cq_blocking(rctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Recv) failed");
    printf("  Recv complete:\n");
    print_wc_detail(&wc);

    /* ---- Step 6: Print received message ---- */
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║  Received message: \"%s\"\n", rctx->recv_buf);
    printf("  ╚══════════════════════════════════════════╝\n");

    return 0;

cleanup:
    return -1;
}

/* ========== Cleanup RDMA Resources ========== */

static void cleanup_rdma_resources(struct rdma_context *rctx)
{
    printf("\n[Cleanup] Releasing RDMA resources...\n");

    /* Release resources in reverse order of creation */
    if (rctx->recv_mr)  { ibv_dereg_mr(rctx->recv_mr);    rctx->recv_mr = NULL; }
    if (rctx->send_mr)  { ibv_dereg_mr(rctx->send_mr);    rctx->send_mr = NULL; }
    if (rctx->recv_buf) { free(rctx->recv_buf);            rctx->recv_buf = NULL; }
    if (rctx->send_buf) { free(rctx->send_buf);            rctx->send_buf = NULL; }
    if (rctx->qp)       { ibv_destroy_qp(rctx->qp);       rctx->qp = NULL; }
    if (rctx->cq)       { ibv_destroy_cq(rctx->cq);       rctx->cq = NULL; }
    if (rctx->pd)       { ibv_dealloc_pd(rctx->pd);       rctx->pd = NULL; }
    if (rctx->ctx)      { ibv_close_device(rctx->ctx);    rctx->ctx = NULL; }

    printf("[Cleanup] Done\n");
}
