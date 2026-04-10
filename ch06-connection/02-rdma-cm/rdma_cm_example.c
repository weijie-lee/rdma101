/**
 * RDMA CM Connection Management Example -- Establish RDMA Connection Using librdmacm
 *
 * Features:
 *   - Uses RDMA CM (Connection Manager) high-level API to establish connections
 *   - Server/client dual mode
 *   - CM automatically handles address resolution, route resolution, QP state transitions
 *   - Prints the name and details of each CM event
 *   - Performs Send/Recv test after connection to verify
 *
 * Usage:
 *   Server: ./rdma_cm_example -s <port>
 *   Client: ./rdma_cm_example -c <server_ip> <port>
 *
 * Build:
 *   gcc -o rdma_cm_example rdma_cm_example.c -lrdmacm -libverbs
 *
 * Run example:
 *   Terminal 1: ./rdma_cm_example -s 7471
 *   Terminal 2: ./rdma_cm_example -c 127.0.0.1 7471
 *
 * CM Event Flow:
 *   Server                              Client
 *   ------                              ------
 *   rdma_listen()
 *                                       rdma_resolve_addr()
 *                                       -> ADDR_RESOLVED
 *                                       rdma_resolve_route()
 *                                       -> ROUTE_RESOLVED
 *                                       rdma_connect()
 *   -> CONNECT_REQUEST
 *   rdma_accept()
 *   -> ESTABLISHED                       -> ESTABLISHED
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

/* ========== Constant Definitions ========== */

#define BUFFER_SIZE         1024        /* Send/receive buffer size */
#define TIMEOUT_MS          5000        /* Address/route resolution timeout (ms) */
#define CQ_DEPTH            16          /* CQ depth */
#define MAX_WR              16          /* QP maximum WR count */
#define SERVER_MSG          "Hello from RDMA CM server!"
#define CLIENT_MSG          "Hello from RDMA CM client!"

/* ========== Error Handling Macros ========== */

#define CM_CHECK_NULL(ptr, msg) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[Error] %s: %s (errno=%d: %s)\n", \
                (msg), #ptr, errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

#define CM_CHECK_ERRNO(ret, msg) do { \
    if ((ret) != 0) { \
        fprintf(stderr, "[Error] %s: ret=%d (errno=%d: %s)\n", \
                (msg), (ret), errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

/* ========== RDMA CM Resource Structure ========== */

struct cm_context {
    /* CM resources */
    struct rdma_event_channel   *ec;            /* Event channel */
    struct rdma_cm_id           *listen_id;     /* Server listen ID */
    struct rdma_cm_id           *cm_id;         /* Connection ID (for client/server communication) */

    /* Verbs resources */
    struct ibv_pd               *pd;            /* Protection domain */
    struct ibv_cq               *cq;            /* Completion queue */
    struct ibv_qp               *qp;            /* Queue pair */

    /* Memory regions */
    struct ibv_mr               *send_mr;       /* Send MR */
    struct ibv_mr               *recv_mr;       /* Receive MR */
    char                        *send_buf;      /* Send buffer */
    char                        *recv_buf;      /* Receive buffer */

    /* Runtime parameters */
    int                         is_server;      /* Whether in server mode */
    const char                  *server_ip;     /* Server IP */
    int                         port;           /* Listen port */
};

/* ========== CM Event Names ========== */

/**
 * cm_event_str - Convert CM event type to readable string
 *
 * RDMA CM defines a series of events for asynchronous notification of connection state changes.
 * Applications obtain events via rdma_get_cm_event() and must call rdma_ack_cm_event() to acknowledge.
 */
static const char *cm_event_str(enum rdma_cm_event_type event)
{
    switch (event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:       return "ADDR_RESOLVED (address resolved)";
    case RDMA_CM_EVENT_ADDR_ERROR:          return "ADDR_ERROR (address resolution failed)";
    case RDMA_CM_EVENT_ROUTE_RESOLVED:      return "ROUTE_RESOLVED (route resolved)";
    case RDMA_CM_EVENT_ROUTE_ERROR:         return "ROUTE_ERROR (route resolution failed)";
    case RDMA_CM_EVENT_CONNECT_REQUEST:     return "CONNECT_REQUEST (connection request received)";
    case RDMA_CM_EVENT_CONNECT_RESPONSE:    return "CONNECT_RESPONSE (connection response received)";
    case RDMA_CM_EVENT_CONNECT_ERROR:       return "CONNECT_ERROR (connection error)";
    case RDMA_CM_EVENT_UNREACHABLE:         return "UNREACHABLE (unreachable)";
    case RDMA_CM_EVENT_REJECTED:            return "REJECTED (connection rejected)";
    case RDMA_CM_EVENT_ESTABLISHED:         return "ESTABLISHED (connection established)";
    case RDMA_CM_EVENT_DISCONNECTED:        return "DISCONNECTED (disconnected)";
    case RDMA_CM_EVENT_DEVICE_REMOVAL:      return "DEVICE_REMOVAL (device removed)";
    case RDMA_CM_EVENT_MULTICAST_JOIN:      return "MULTICAST_JOIN (joined multicast group)";
    case RDMA_CM_EVENT_MULTICAST_ERROR:     return "MULTICAST_ERROR (multicast error)";
    case RDMA_CM_EVENT_ADDR_CHANGE:         return "ADDR_CHANGE (address changed)";
    case RDMA_CM_EVENT_TIMEWAIT_EXIT:       return "TIMEWAIT_EXIT (timewait exit)";
    default:                                return "UNKNOWN (unknown event)";
    }
}

/* ========== Function Declarations ========== */

static void print_usage(const char *prog);
static int  parse_args(struct cm_context *cm, int argc, char *argv[]);
static int  create_qp_on_cm_id(struct cm_context *cm, struct rdma_cm_id *id);
static int  alloc_buffers(struct cm_context *cm);
static int  wait_for_event(struct cm_context *cm, enum rdma_cm_event_type expected,
                           struct rdma_cm_id **out_id);
static int  run_server(struct cm_context *cm);
static int  run_client(struct cm_context *cm);
static int  do_send_recv_test(struct cm_context *cm);
static void cleanup_cm(struct cm_context *cm);

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    struct cm_context cm;
    int ret = 1;

    memset(&cm, 0, sizeof(cm));

    /* Parse command-line arguments */
    if (parse_args(&cm, argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  RDMA CM Connection Example (Send/Recv)\n");
    printf("  Mode: %s\n", cm.is_server ? "Server" : "Client");
    printf("  Port: %d\n", cm.port);
    printf("========================================\n\n");

    /* Run based on mode */
    if (cm.is_server) {
        ret = run_server(&cm);
    } else {
        ret = run_client(&cm);
    }

    cleanup_cm(&cm);
    return ret;
}

/* ========== Print Usage ========== */

static void print_usage(const char *prog)
{
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "  Server: %s -s <port>\n", prog);
    fprintf(stderr, "  Client: %s -c <server_ip> <port>\n", prog);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -s 7471\n", prog);
    fprintf(stderr, "  %s -c 127.0.0.1 7471\n", prog);
    fprintf(stderr, "\n");
}

/* ========== Parse Command-Line Arguments ========== */

static int parse_args(struct cm_context *cm, int argc, char *argv[])
{
    if (argc < 3) return -1;

    if (strcmp(argv[1], "-s") == 0) {
        cm->is_server = 1;
        cm->port = atoi(argv[2]);
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc < 4) return -1;
        cm->is_server = 0;
        cm->server_ip = argv[2];
        cm->port = atoi(argv[3]);
    } else {
        return -1;
    }

    return 0;
}

/* ========== Create QP on CM ID ========== */

/**
 * create_qp_on_cm_id - Create QP and associated resources on the specified rdma_cm_id
 *
 * RDMA CM's QP creation differs from using libibverbs directly:
 *   - Uses rdma_create_qp() instead of ibv_create_qp()
 *   - QP is automatically associated with cm_id, CM auto-manages QP state transitions
 *   - PD, CQ need to be created first using device context from cm_id->verbs
 */
static int create_qp_on_cm_id(struct cm_context *cm, struct rdma_cm_id *id)
{
    struct ibv_qp_init_attr qp_attr;
    int ret;

    /*
     * cm_id->verbs is the device context allocated by CM for us.
     * Server side: After receiving CONNECT_REQUEST event, the new cm_id will have verbs.
     * Client side: After ADDR_RESOLVED event, cm_id will have verbs.
     */
    if (!id->verbs) {
        fprintf(stderr, "[Error] cm_id->verbs is NULL, cannot create QP\n");
        return -1;
    }

    /* Allocate protection domain (PD) */
    cm->pd = ibv_alloc_pd(id->verbs);
    CM_CHECK_NULL(cm->pd, "Failed to allocate protection domain (PD)");

    /* Create completion queue (CQ) */
    cm->cq = ibv_create_cq(id->verbs, CQ_DEPTH, NULL, NULL, 0);
    CM_CHECK_NULL(cm->cq, "Failed to create completion queue (CQ)");

    /* Create QP (via RDMA CM interface) */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq  = cm->cq;
    qp_attr.recv_cq  = cm->cq;
    qp_attr.qp_type  = IBV_QPT_RC;         /* RC: Reliable Connected */
    qp_attr.cap.max_send_wr  = MAX_WR;
    qp_attr.cap.max_recv_wr  = MAX_WR;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    /*
     * rdma_create_qp() vs ibv_create_qp():
     *   - rdma_create_qp() binds the QP to cm_id
     *   - During subsequent rdma_accept() / rdma_connect(), CM auto-completes QP state transitions
     *   - No need to manually call ibv_modify_qp() for RESET->INIT->RTR->RTS
     */
    ret = rdma_create_qp(id, cm->pd, &qp_attr);
    CM_CHECK_ERRNO(ret, "rdma_create_qp failed");

    cm->qp = id->qp;
    printf("  QP created successfully: qp_num=%u (managed by RDMA CM)\n", cm->qp->qp_num);

    return 0;

cleanup:
    return -1;
}

/* ========== Allocate and Register Buffers ========== */

static int alloc_buffers(struct cm_context *cm)
{
    /* Send buffer */
    cm->send_buf = (char *)malloc(BUFFER_SIZE);
    CM_CHECK_NULL(cm->send_buf, "Failed to allocate send buffer");
    memset(cm->send_buf, 0, BUFFER_SIZE);

    cm->send_mr = ibv_reg_mr(cm->pd, cm->send_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CM_CHECK_NULL(cm->send_mr, "Failed to register send MR");

    /* Receive buffer */
    cm->recv_buf = (char *)malloc(BUFFER_SIZE);
    CM_CHECK_NULL(cm->recv_buf, "Failed to allocate receive buffer");
    memset(cm->recv_buf, 0, BUFFER_SIZE);

    cm->recv_mr = ibv_reg_mr(cm->pd, cm->recv_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CM_CHECK_NULL(cm->recv_mr, "Failed to register receive MR");

    printf("  Buffer registration complete: send_lkey=0x%x, recv_lkey=0x%x\n",
           cm->send_mr->lkey, cm->recv_mr->lkey);

    return 0;

cleanup:
    return -1;
}

/* ========== Wait for CM Event ========== */

/**
 * wait_for_event - Wait for a specific type of CM event
 *
 * @cm:       CM context
 * @expected: Expected event type
 * @out_id:   Output: cm_id associated with the event (can be NULL)
 *
 * RDMA CM uses an asynchronous event model:
 *   1. rdma_get_cm_event() blocks waiting for an event
 *   2. Process the event (check type, get cm_id, etc.)
 *   3. rdma_ack_cm_event() acknowledges the event (must be called!)
 *
 * Note: After rdma_ack_cm_event(), the event pointer is no longer valid.
 *       If you need event->id, you must save it before ack.
 */
static int wait_for_event(struct cm_context *cm,
                          enum rdma_cm_event_type expected,
                          struct rdma_cm_id **out_id)
{
    struct rdma_cm_event *event = NULL;
    int ret;

    printf("  Waiting for CM event: %s ...\n", cm_event_str(expected));

    /* Block waiting for the next CM event */
    ret = rdma_get_cm_event(cm->ec, &event);
    if (ret != 0) {
        fprintf(stderr, "[Error] rdma_get_cm_event failed: %s\n", strerror(errno));
        return -1;
    }

    /* Print the received event */
    printf("  ★ Received CM event: %s\n", cm_event_str(event->event));

    /* Check if the event type matches expectations */
    if (event->event != expected) {
        fprintf(stderr, "[Error] Expected event %s, but received %s (status=%d)\n",
                cm_event_str(expected),
                cm_event_str(event->event),
                event->status);
        rdma_ack_cm_event(event);
        return -1;
    }

    /* Save the cm_id associated with the event (before ack!) */
    if (out_id) {
        *out_id = event->id;
    }

    /*
     * Acknowledge the event -- must be called!
     * rdma_ack_cm_event() releases event resources.
     * After this, the event pointer is no longer valid.
     */
    rdma_ack_cm_event(event);

    return 0;
}

/* ========== Server Flow ========== */

/**
 * run_server - RDMA CM server flow
 *
 * Server-side CM event flow:
 *   1. Create event channel -> create cm_id -> bind address -> listen
 *   2. Wait for CONNECT_REQUEST event (client initiates connection)
 *   3. Create QP on the new cm_id
 *   4. rdma_accept() to accept the connection
 *   5. Wait for ESTABLISHED event (connection established)
 *   6. Data communication
 */
static int run_server(struct cm_context *cm)
{
    struct sockaddr_in addr;
    struct rdma_conn_param conn_param;
    struct rdma_cm_id *new_cm_id = NULL;
    int ret;

    /* Step 1: Create event channel
     * Event channel is used to receive async CM events, similar to epoll's fd.
     */
    printf("[Step 1] Creating RDMA CM event channel...\n");
    cm->ec = rdma_create_event_channel();
    CM_CHECK_NULL(cm->ec, "Failed to create event channel");
    printf("  Event channel created successfully\n");

    /* Step 2: Create CM ID (similar to socket)
     * RDMA_PS_TCP means using TCP-semantic connection mode (RC QP).
     * Can also use RDMA_PS_UDP to create UD QP.
     */
    printf("[Step 2] Creating RDMA CM ID...\n");
    ret = rdma_create_id(cm->ec, &cm->listen_id, NULL, RDMA_PS_TCP);
    CM_CHECK_ERRNO(ret, "rdma_create_id failed");
    printf("  CM ID created successfully (RDMA_PS_TCP)\n");

    /* Step 3: Bind address (similar to bind)
     * Bind to INADDR_ANY and specified port, accepting connections from any address.
     */
    printf("[Step 3] Binding address (port %d)...\n", cm->port);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cm->port);

    ret = rdma_bind_addr(cm->listen_id, (struct sockaddr *)&addr);
    CM_CHECK_ERRNO(ret, "rdma_bind_addr failed");
    printf("  Bind successful: 0.0.0.0:%d\n", cm->port);

    /* Step 4: Start listening (similar to listen)
     * backlog=1 means at most 1 pending connection.
     */
    printf("[Step 4] Starting to listen for connection requests...\n");
    ret = rdma_listen(cm->listen_id, 1);
    CM_CHECK_ERRNO(ret, "rdma_listen failed");
    printf("  Listening, waiting for client connection...\n\n");

    /* Step 5: Wait for CONNECT_REQUEST event
     * When the client calls rdma_connect(), the server receives this event.
     * The event contains a new cm_id (representing this specific connection).
     * Note: Do not create QP on listen_id, create it on the new cm_id instead.
     */
    printf("[Step 5] Waiting for client connection request...\n");
    ret = wait_for_event(cm, RDMA_CM_EVENT_CONNECT_REQUEST, &new_cm_id);
    CM_CHECK_ERRNO(ret, "Waiting for CONNECT_REQUEST failed");

    /* Save new cm_id for subsequent communication */
    cm->cm_id = new_cm_id;

    /* Step 6: Create QP on new cm_id and allocate buffers */
    printf("[Step 6] Creating QP and registering buffers...\n");
    ret = create_qp_on_cm_id(cm, cm->cm_id);
    CM_CHECK_ERRNO(ret, "Failed to create QP");

    ret = alloc_buffers(cm);
    CM_CHECK_ERRNO(ret, "Failed to allocate buffers");

    /* Step 7: Accept connection (similar to accept)
     * rdma_accept() will automatically transition QP state to RTR -> RTS.
     * conn_param can carry private data (up to 196 bytes).
     */
    printf("[Step 7] Accepting connection (rdma_accept)...\n");
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;     /* Number of RDMA Reads the peer can initiate */
    conn_param.responder_resources = 1; /* Number of RDMA Reads this end responds to */

    ret = rdma_accept(cm->cm_id, &conn_param);
    CM_CHECK_ERRNO(ret, "rdma_accept failed");

    /* Step 8: Wait for ESTABLISHED event (connection formally established) */
    printf("[Step 8] Waiting for connection establishment confirmation...\n");
    ret = wait_for_event(cm, RDMA_CM_EVENT_ESTABLISHED, NULL);
    CM_CHECK_ERRNO(ret, "Waiting for ESTABLISHED failed");

    printf("\n  *** Connection established! QP state auto-transitioned to RTS by CM ***\n\n");

    /* Step 9: Data communication test */
    printf("[Step 9] Performing Send/Recv communication test...\n");
    ret = do_send_recv_test(cm);
    CM_CHECK_ERRNO(ret, "Communication test failed");

    printf("\n========================================\n");
    printf("  Server: All tests passed!\n");
    printf("========================================\n");

    return 0;

cleanup:
    return 1;
}

/* ========== Client Flow ========== */

/**
 * run_client - RDMA CM client flow
 *
 * Client-side CM event flow:
 *   1. Create event channel -> create cm_id
 *   2. rdma_resolve_addr() -> wait for ADDR_RESOLVED
 *   3. rdma_resolve_route() -> wait for ROUTE_RESOLVED
 *   4. Create QP
 *   5. rdma_connect() -> wait for ESTABLISHED
 *   6. Data communication
 */
static int run_client(struct cm_context *cm)
{
    struct sockaddr_in addr;
    struct rdma_conn_param conn_param;
    int ret;

    /* Step 1: Create event channel */
    printf("[Step 1] Creating RDMA CM event channel...\n");
    cm->ec = rdma_create_event_channel();
    CM_CHECK_NULL(cm->ec, "Failed to create event channel");

    /* Step 2: Create CM ID */
    printf("[Step 2] Creating RDMA CM ID...\n");
    ret = rdma_create_id(cm->ec, &cm->cm_id, NULL, RDMA_PS_TCP);
    CM_CHECK_ERRNO(ret, "rdma_create_id failed");
    printf("  CM ID created successfully\n");

    /* Step 3: Resolve server address
     * rdma_resolve_addr() resolves IP address to RDMA address.
     * For RoCE, this includes finding the corresponding network interface and GID.
     * For IB, this includes querying SM for path information.
     * The operation is asynchronous, results are notified via CM events.
     */
    printf("[Step 3] Resolving server address: %s:%d ...\n", cm->server_ip, cm->port);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cm->port);
    if (inet_pton(AF_INET, cm->server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[Error] Invalid server IP: %s\n", cm->server_ip);
        goto cleanup;
    }

    ret = rdma_resolve_addr(cm->cm_id, NULL, (struct sockaddr *)&addr, TIMEOUT_MS);
    CM_CHECK_ERRNO(ret, "rdma_resolve_addr failed");

    /* Wait for ADDR_RESOLVED event */
    ret = wait_for_event(cm, RDMA_CM_EVENT_ADDR_RESOLVED, NULL);
    CM_CHECK_ERRNO(ret, "Address resolution failed");
    printf("  Address resolution complete: RDMA device and path found\n");

    /* Step 4: Resolve route
     * rdma_resolve_route() determines the complete path to the target.
     * For IB: obtains path record via SM (Subnet Manager).
     * For RoCE: determines next hop via system routing table.
     */
    printf("[Step 4] Resolving route...\n");
    ret = rdma_resolve_route(cm->cm_id, TIMEOUT_MS);
    CM_CHECK_ERRNO(ret, "rdma_resolve_route failed");

    /* Wait for ROUTE_RESOLVED event */
    ret = wait_for_event(cm, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL);
    CM_CHECK_ERRNO(ret, "Route resolution failed");
    printf("  Route resolution complete: path to target determined\n");

    /* Step 5: Create QP and allocate buffers
     * After route resolution, cm_id->verbs is available.
     * QP must be created before rdma_connect().
     */
    printf("[Step 5] Creating QP and registering buffers...\n");
    ret = create_qp_on_cm_id(cm, cm->cm_id);
    CM_CHECK_ERRNO(ret, "Failed to create QP");

    ret = alloc_buffers(cm);
    CM_CHECK_ERRNO(ret, "Failed to allocate buffers");

    /* Step 6: Initiate connection
     * rdma_connect() sends a connection request to the server.
     * CM will automatically transition QP to INIT and RTR states.
     * When the server calls rdma_accept(), QP will auto-transition to RTS.
     */
    printf("[Step 6] Initiating RDMA connection...\n");
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;         /* Retry count */
    conn_param.rnr_retry_count = 7;     /* RNR retry count (7 = infinite) */

    ret = rdma_connect(cm->cm_id, &conn_param);
    CM_CHECK_ERRNO(ret, "rdma_connect failed");

    /* Wait for ESTABLISHED event (connection formally established) */
    printf("[Step 7] Waiting for connection establishment confirmation...\n");
    ret = wait_for_event(cm, RDMA_CM_EVENT_ESTABLISHED, NULL);
    CM_CHECK_ERRNO(ret, "Waiting for ESTABLISHED failed");

    printf("\n  *** Connection established! QP state auto-transitioned to RTS by CM ***\n\n");

    /* Step 7: Data communication test */
    printf("[Step 8] Performing Send/Recv communication test...\n");
    ret = do_send_recv_test(cm);
    CM_CHECK_ERRNO(ret, "Communication test failed");

    printf("\n========================================\n");
    printf("  Client: All tests passed!\n");
    printf("========================================\n");

    return 0;

cleanup:
    return 1;
}

/* ========== Perform Send/Recv Communication Test ========== */

/**
 * do_send_recv_test - Bidirectional Send/Recv test
 *
 * Communication logic is identical to the manual connection version:
 *   1. First Post Recv (prepare to receive)
 *   2. Post Send (send message)
 *   3. Poll CQ waiting for Send completion
 *   4. Poll CQ waiting for Recv completion
 *   5. Print received message
 *
 * This demonstrates: once the connection is established, regardless of
 * manual or CM connection, the subsequent data path operations
 * (post_send/recv, poll_cq) are exactly the same.
 */
static int do_send_recv_test(struct cm_context *cm)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    int ret, ne;
    const char *msg;

    /* Step 1: Post Recv (prepare to receive peer's message) */
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)cm->recv_buf;
    sge.length = BUFFER_SIZE;
    sge.lkey   = cm->recv_mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(cm->qp, &recv_wr, &bad_recv_wr);
    CM_CHECK_ERRNO(ret, "Post Recv failed");
    printf("  Post Recv complete (waiting for peer's message)\n");

    /* Step 2: Prepare and send message */
    msg = cm->is_server ? SERVER_MSG : CLIENT_MSG;
    strncpy(cm->send_buf, msg, BUFFER_SIZE - 1);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)cm->send_buf;
    sge.length = (uint32_t)(strlen(msg) + 1);
    sge.lkey   = cm->send_mr->lkey;

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(cm->qp, &send_wr, &bad_send_wr);
    CM_CHECK_ERRNO(ret, "Post Send failed");
    printf("  Post Send complete (sent: \"%s\")\n", msg);

    /* Step 3: Poll CQ waiting for two completion events (Send + Recv) */
    int completions = 0;
    while (completions < 2) {
        ne = ibv_poll_cq(cm->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[Error] ibv_poll_cq failed\n");
            goto cleanup;
        }
        if (ne == 0) continue;

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[Error] WC failed: %s (status=%d, wr_id=%lu)\n",
                    ibv_wc_status_str(wc.status), wc.status,
                    (unsigned long)wc.wr_id);
            goto cleanup;
        }

        if (wc.wr_id == 2) {
            printf("  Send complete (wr_id=%lu, bytes=%u)\n",
                   (unsigned long)wc.wr_id, wc.byte_len);
        } else if (wc.wr_id == 1) {
            printf("  Recv complete (wr_id=%lu, bytes=%u)\n",
                   (unsigned long)wc.wr_id, wc.byte_len);
        }

        completions++;
    }

    /* Step 4: Print received message */
    printf("\n  ╔══════════════════════════════════════════╗\n");
    printf("  ║  Received message: \"%s\"\n", cm->recv_buf);
    printf("  ╚══════════════════════════════════════════╝\n");

    return 0;

cleanup:
    return -1;
}

/* ========== Cleanup Resources ========== */

static void cleanup_cm(struct cm_context *cm)
{
    printf("\n[Cleanup] Releasing RDMA CM resources...\n");

    /* Release MR and buffers first */
    if (cm->recv_mr)    { ibv_dereg_mr(cm->recv_mr);   cm->recv_mr = NULL; }
    if (cm->send_mr)    { ibv_dereg_mr(cm->send_mr);   cm->send_mr = NULL; }
    if (cm->recv_buf)   { free(cm->recv_buf);           cm->recv_buf = NULL; }
    if (cm->send_buf)   { free(cm->send_buf);           cm->send_buf = NULL; }

    /*
     * Destroy CM-managed QP:
     *   Use rdma_destroy_qp() instead of ibv_destroy_qp()
     *   because the QP was created via rdma_create_qp()
     */
    if (cm->cm_id && cm->qp) {
        rdma_destroy_qp(cm->cm_id);
        cm->qp = NULL;
    }

    /* Release CQ and PD */
    if (cm->cq) { ibv_destroy_cq(cm->cq); cm->cq = NULL; }
    if (cm->pd) { ibv_dealloc_pd(cm->pd); cm->pd = NULL; }

    /* Disconnect and destroy CM ID */
    if (cm->cm_id) {
        rdma_disconnect(cm->cm_id);
        rdma_destroy_id(cm->cm_id);
        cm->cm_id = NULL;
    }
    if (cm->listen_id) {
        rdma_destroy_id(cm->listen_id);
        cm->listen_id = NULL;
    }

    /* Destroy event channel */
    if (cm->ec) {
        rdma_destroy_event_channel(cm->ec);
        cm->ec = NULL;
    }

    printf("[Cleanup] Done\n");
}
