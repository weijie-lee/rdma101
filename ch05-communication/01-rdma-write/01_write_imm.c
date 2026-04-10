/**
 * RDMA Write with Immediate Data Example
 *
 * Demonstrates the IBV_WR_RDMA_WRITE_WITH_IMM operation:
 *   - Client uses RDMA Write with Imm to write data to Server
 *   - Server must call ibv_post_recv() in advance to receive immediate data
 *   - Server's WC has opcode = IBV_WC_RECV_RDMA_WITH_IMM, and wc.imm_data is set
 *   - imm_data (32-bit) can serve as a "notification signal" to inform Server that data write is complete
 *
 * Comparison with regular RDMA Write:
 *   - Regular Write: Server side is completely unaware, no CQ notification at all
 *   - Write with Imm: Server side receives a recv completion with imm_data
 *   - Cost: Server must post recv WR in advance (consumes one recv WR slot)
 *
 * Usage:
 *   Server: ./01_write_imm server
 *   Client: ./01_write_imm client <server_ip>
 *
 * Build: gcc -Wall -O2 -g -o 01_write_imm 01_write_imm.c -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE     4096
#define TCP_PORT        19876
#define IMM_MAGIC       0x12345678  /* Custom immediate data: indicates "write complete" */

/* ========== RDMA Resources ========== */
struct write_imm_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *mr;
    char                *buf;
    uint8_t              port;
    int                  is_roce;   /* Auto-detect: 0=IB, 1=RoCE */
};

/* ========== Initialize RDMA Resources ========== */
static int init_resources(struct write_imm_ctx *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices = 0;
    int ret = -1;

    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get RDMA device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }

    res->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(res->ctx, "Failed to open RDMA device");
    printf("[Info] Opened device: %s\n", ibv_get_device_name(dev_list[0]));

    /* Detect transport layer type */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[Info] Transport layer type: %s\n", transport_str(transport));

    /* Allocate PD */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate protection domain");

    /* Create CQ */
    res->cq = ibv_create_cq(res->ctx, 128, NULL, NULL, 0);
    CHECK_NULL(res->cq, "Failed to create completion queue");

    /* Create RC QP */
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "Failed to create queue pair");

    /* Allocate and register memory */
    res->buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(res->buf, "Failed to allocate buffer");
    memset(res->buf, 0, BUFFER_SIZE);

    /* Server needs REMOTE_WRITE permission; Client needs LOCAL_WRITE */
    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    CHECK_NULL(res->mr, "Failed to register memory region");

    printf("[Info] QP num=%u, MR addr=%p, lkey=0x%x, rkey=0x%x\n",
           res->qp->qp_num, (void *)res->buf, res->mr->lkey, res->mr->rkey);

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Server: pre-post recv to receive imm_data ========== */
static int server_post_recv(struct write_imm_ctx *res)
{
    /*
     * Write with Immediate consumes a recv WR on the remote side.
     * Server must post recv in advance, otherwise an RNR error will occur.
     * The recv WR's SGE can be empty (length=0) because data is written directly via RDMA Write,
     * but here we still provide an SGE so we can see byte_len in the WC.
     */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = BUFFER_SIZE,
        .lkey   = res->mr->lkey,
    };

    struct ibv_recv_wr wr = {
        .wr_id   = 1001,   /* Custom ID for easy identification in WC */
        .sg_list = &sge,
        .num_sge = 1,
    };

    struct ibv_recv_wr *bad_wr = NULL;
    int ret = ibv_post_recv(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] ibv_post_recv failed: ret=%d errno=%d\n", ret, errno);
        return -1;
    }
    printf("[Server] Posted recv (wr_id=1001), waiting for Write with Imm notification\n");
    return 0;
}

/* ========== Client: perform RDMA Write with Immediate ========== */
static int client_write_imm(struct write_imm_ctx *res,
                            uint64_t remote_addr, uint32_t remote_rkey)
{
    /* Prepare data to write */
    const char *msg = "Hello! This message was written via RDMA Write with Immediate Data!";
    size_t msg_len = strlen(msg) + 1;
    memcpy(res->buf, msg, msg_len);

    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = (uint32_t)msg_len,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 2001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE_WITH_IMM;  /* Key: Write + Immediate */
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data   = htobe32(IMM_MAGIC);  /* Immediate data (network byte order) */
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] ibv_post_send failed: ret=%d errno=%d\n", ret, errno);
        return -1;
    }

    printf("[Client] Sent RDMA Write with Imm (imm_data=0x%X)\n", IMM_MAGIC);
    printf("[Client]   Data: \"%s\" (%zu bytes)\n", msg, msg_len);
    printf("[Client]   Target: remote_addr=0x%lx, rkey=0x%x\n",
           (unsigned long)remote_addr, remote_rkey);

    /* Wait for send completion */
    struct ibv_wc wc;
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;

    printf("\n[Client] === Sender WC Details ===\n");
    print_wc_detail(&wc);

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[Error] Send failed: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }

    printf("[Client] RDMA Write with Imm completed!\n");
    return 0;
}

/* ========== Server: wait and process Write with Imm recv completion ========== */
static int server_wait_imm(struct write_imm_ctx *res)
{
    struct ibv_wc wc;
    int ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;

    printf("\n[Server] === Receiver WC Details ===\n");
    print_wc_detail(&wc);

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[Error] Receive WC failed: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }

    /* Verify opcode */
    if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        uint32_t imm = be32toh(wc.imm_data);
        printf("\n[Server] *** Received Write with Immediate notification! ***\n");
        printf("[Server]   opcode = IBV_WC_RECV_RDMA_WITH_IMM (correct)\n");
        printf("[Server]   imm_data = 0x%X", imm);
        if (imm == IMM_MAGIC) {
            printf(" (matches IMM_MAGIC, write completion confirmed)\n");
        } else {
            printf(" (unknown immediate data)\n");
        }
        printf("[Server]   wr_id = %lu\n", (unsigned long)wc.wr_id);
    } else {
        printf("[Server] Unexpected opcode: %d\n", wc.opcode);
    }

    /* Read data written by RDMA Write */
    printf("\n[Server] Data in buffer: \"%s\"\n", res->buf);

    return 0;
}

/* ========== Comparison: Regular RDMA Write ========== */
static void print_comparison(void)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║       Regular RDMA Write  vs  RDMA Write with Immediate     ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Feature        │ Regular Write   │ Write with Imm           ║\n");
    printf("║────────────────┼─────────────────┼──────────────────────────║\n");
    printf("║ Server notif.  │ None (unaware)  │ Yes (CQ recv WC)        ║\n");
    printf("║ Server post_rv │ Not needed      │ Must pre-post recv      ║\n");
    printf("║ WC opcode      │ None            │ IBV_WC_RECV_RDMA_WITH_IM║\n");
    printf("║ imm_data       │ None            │ 32-bit custom data      ║\n");
    printf("║ Use case       │ No notif needed │ Write completion notif  ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/* ========== Cleanup Resources ========== */
static void cleanup_resources(struct write_imm_ctx *res)
{
    if (res->mr)      ibv_dereg_mr(res->mr);
    if (res->buf)     free(res->buf);
    if (res->qp)      ibv_destroy_qp(res->qp);
    if (res->cq)      ibv_destroy_cq(res->cq);
    if (res->pd)      ibv_dealloc_pd(res->pd);
    if (res->ctx)     ibv_close_device(res->ctx);
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s server|client [server_ip]\n", argv[0]);
        fprintf(stderr, "  Server: %s server\n", argv[0]);
        fprintf(stderr, "  Client: %s client <server_ip>\n", argv[0]);
        return 1;
    }

    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("========================================\n");
    printf("  RDMA Write with Immediate Data Example\n");
    printf("  Role: %s\n", is_server ? "Server" : "Client");
    printf("========================================\n\n");

    print_comparison();

    struct write_imm_ctx res;
    memset(&res, 0, sizeof(res));

    /* 1. Initialize RDMA resources */
    if (init_resources(&res) != 0) {
        fprintf(stderr, "[Error] Failed to initialize RDMA resources\n");
        return 1;
    }

    /* 2. Fill local endpoint info */
    struct rdma_endpoint local_ep, remote_ep;
    memset(&local_ep, 0, sizeof(local_ep));
    memset(&remote_ep, 0, sizeof(remote_ep));

    if (fill_local_endpoint(res.ctx, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &local_ep) != 0) {
        fprintf(stderr, "[Error] Failed to fill local endpoint info\n");
        cleanup_resources(&res);
        return 1;
    }
    /* Attach MR info for remote RDMA Write use */
    local_ep.buf_addr = (uint64_t)res.buf;
    local_ep.buf_rkey = res.mr->rkey;

    printf("[Info] Local endpoint: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           local_ep.qp_num, local_ep.lid,
           (unsigned long)local_ep.buf_addr, local_ep.buf_rkey);

    /* 3. TCP exchange endpoint info */
    printf("\n[Info] Exchanging connection info via TCP (port %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[Error] TCP info exchange failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] Remote endpoint: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           remote_ep.qp_num, remote_ep.lid,
           (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);

    /* 4. Server: post recv before connection (QP can post recv in INIT state) */
    /*    First do INIT transition */
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    if (qp_to_init(res.qp, res.port, access) != 0) {
        fprintf(stderr, "[Error] QP RESET->INIT failed\n");
        cleanup_resources(&res);
        return 1;
    }

    if (is_server) {
        /* Server: post recv in INIT state, ready to receive imm_data notification */
        if (server_post_recv(&res) != 0) {
            cleanup_resources(&res);
            return 1;
        }
    }

    /* 5. QP state transition: INIT -> RTR -> RTS */
    if (qp_to_rtr(res.qp, &remote_ep, res.port, res.is_roce) != 0) {
        fprintf(stderr, "[Error] QP INIT->RTR failed\n");
        cleanup_resources(&res);
        return 1;
    }
    if (qp_to_rts(res.qp) != 0) {
        fprintf(stderr, "[Error] QP RTR->RTS failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] QP state: RESET -> INIT -> RTR -> RTS (ready)\n\n");

    /* 6. Execute operation */
    if (is_server) {
        printf("[Server] Waiting for Client's RDMA Write with Immediate...\n");
        if (server_wait_imm(&res) != 0) {
            fprintf(stderr, "[Server] Receive failed\n");
        }
    } else {
        /* Client: perform Write with Immediate to Server's MR */
        if (client_write_imm(&res, remote_ep.buf_addr, remote_ep.buf_rkey) != 0) {
            fprintf(stderr, "[Client] Write failed\n");
        }
    }

    printf("\n[Info] Program finished, cleaning up resources...\n");
    cleanup_resources(&res);
    return 0;
}
