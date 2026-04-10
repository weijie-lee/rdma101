/**
 * Batch RDMA Read Example - Single ibv_post_send() submits 4 chained RDMA Read WRs
 *
 * Key points:
 *   - Server prepares a 4KB buffer, divided into 4 x 1KB chunks: "CHUNK-00:data...", "CHUNK-01:data...", ...
 *   - Client creates 4 RDMA Read WRs, chained via next pointers:
 *       wr0.next = &wr1, wr1.next = &wr2, wr2.next = &wr3, wr3.next = NULL
 *   - Single ibv_post_send() submits all 4 WRs at once
 *   - Polls CQ 4 times to get completion events for each WR
 *   - Prints wr_id and read content for each chunk
 *
 * Advantages of chained submission:
 *   - Reduces ibv_post_send() call count (user-to-kernel transitions)
 *   - NIC can pipeline multiple requests, improving throughput
 *   - Note: max_rd_atomic limits the number of in-flight RDMA Reads
 *
 * Usage:
 *   Server: ./01_batch_read server
 *   Client: ./01_batch_read client <server_ip>
 *
 * Build:
 *   gcc -Wall -O2 -g -o 01_batch_read 01_batch_read.c \
 *       -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constants ========== */
#define BUFFER_SIZE     4096        /* Total buffer size: 4KB */
#define CHUNK_SIZE      1024        /* Each chunk: 1KB */
#define NUM_CHUNKS      4           /* Number of chunks */
#define TCP_PORT        19880       /* TCP info exchange port */

/* ========== RDMA Resource Context ========== */
struct batch_read_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *mr;
    char                *buf;       /* 4KB buffer */
    uint8_t              port;
    int                  is_roce;   /* Auto-detect: 0=IB, 1=RoCE */
};

/* ========== Initialize RDMA Resources ========== */
static int init_resources(struct batch_read_ctx *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices = 0;
    int ret = -1;

    /* Get device list */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get RDMA device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }

    /* Open first device */
    res->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(res->ctx, "Failed to open RDMA device");
    printf("[Info] Opened device: %s\n", ibv_get_device_name(dev_list[0]));

    /* Auto-detect transport layer type (IB / RoCE) */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[Info] Transport layer type: %s\n", transport_str(transport));

    /* Query device capabilities - confirm max_qp_rd_atomic (max in-flight RDMA Read limit) */
    struct ibv_device_attr dev_attr;
    if (ibv_query_device(res->ctx, &dev_attr) == 0) {
        printf("[Info] max_qp_rd_atomic = %d (max in-flight RDMA Read/Atomic limit)\n",
               dev_attr.max_qp_rd_atom);
    }

    /* Allocate protection domain */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate protection domain");

    /* Create completion queue - must hold at least NUM_CHUNKS completion events */
    res->cq = ibv_create_cq(res->ctx, 128, NULL, NULL, 0);
    CHECK_NULL(res->cq, "Failed to create completion queue");

    /* Create RC queue pair */
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;     /* Must be >= NUM_CHUNKS */
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "Failed to create queue pair");

    /* Allocate and register memory region */
    res->buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(res->buf, "Failed to allocate buffer");
    memset(res->buf, 0, BUFFER_SIZE);

    /*
     * Access permission notes:
     *   LOCAL_WRITE:  Client RDMA Read results need to be written to local MR
     *   REMOTE_READ:  Server allows remote reads (core permission for RDMA Read)
     *   REMOTE_WRITE: Reserved, kept for consistency
     */
    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_READ |
                         IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(res->mr, "Failed to register memory region");

    printf("[Info] QP num=%u, MR addr=%p, lkey=0x%x, rkey=0x%x\n",
           res->qp->qp_num, (void *)res->buf, res->mr->lkey, res->mr->rkey);

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Server: Fill 4 x 1KB data chunks ========== */
static void server_fill_chunks(char *buf)
{
    /*
     * Divide the 4KB buffer into 4 x 1KB chunks, each formatted as:
     *   "CHUNK-XX:data_xxxxxxxx..."
     * where XX is the chunk number (00~03), followed by hex sample data
     */
    printf("[Server] Filling %d data chunks (each %d bytes):\n", NUM_CHUNKS, CHUNK_SIZE);

    for (int i = 0; i < NUM_CHUNKS; i++) {
        char *chunk = buf + i * CHUNK_SIZE;
        int offset = 0;

        /* Write chunk header: "CHUNK-XX:data_" */
        offset += snprintf(chunk + offset, CHUNK_SIZE - offset,
                           "CHUNK-%02d:data_", i);

        /* Fill with recognizable hex data */
        for (int j = 0; j < 120 && offset < CHUNK_SIZE - 1; j++) {
            offset += snprintf(chunk + offset, CHUNK_SIZE - offset,
                               "%02X", (unsigned char)((i * 100 + j) & 0xFF));
        }
        chunk[CHUNK_SIZE - 1] = '\0';  /* Ensure null-terminated */

        /* Print first 60 characters as preview */
        char preview[64];
        snprintf(preview, sizeof(preview), "%.60s", chunk);
        printf("  Chunk %d (offset %4d): \"%s...\"\n", i, i * CHUNK_SIZE, preview);
    }
    printf("\n");
}

/* ========== Client: Build 4 chained WRs and batch submit RDMA Read ========== */
static int client_batch_read(struct batch_read_ctx *res,
                             uint64_t remote_addr, uint32_t remote_rkey)
{
    /*
     * Chained WR submission principle:
     *   wr[0].next -> &wr[1]
     *   wr[1].next -> &wr[2]
     *   wr[2].next -> &wr[3]
     *   wr[3].next -> NULL
     *
     * Calling ibv_post_send(qp, &wr[0], &bad_wr) submits the entire chain at once.
     * HCA processes in chain order, but completion order is not strictly guaranteed
     * under high concurrency (usually in order).
     */
    struct ibv_sge     sge[NUM_CHUNKS];
    struct ibv_send_wr wr[NUM_CHUNKS];

    memset(sge, 0, sizeof(sge));
    memset(wr, 0, sizeof(wr));

    printf("[Client] Building %d RDMA Read WRs (chained):\n", NUM_CHUNKS);

    for (int i = 0; i < NUM_CHUNKS; i++) {
        /* Each SGE points to a different 1KB region of the local buffer */
        sge[i].addr   = (uint64_t)(res->buf + i * CHUNK_SIZE);
        sge[i].length = CHUNK_SIZE;
        sge[i].lkey   = res->mr->lkey;

        /* Configure RDMA Read WR */
        wr[i].wr_id      = (uint64_t)i;            /* Use chunk number as wr_id */
        wr[i].opcode     = IBV_WR_RDMA_READ;
        wr[i].sg_list    = &sge[i];
        wr[i].num_sge    = 1;
        wr[i].send_flags = IBV_SEND_SIGNALED;       /* Each WR generates a CQ completion */

        /* Remote address: the i-th 1KB chunk of server's buffer */
        wr[i].wr.rdma.remote_addr = remote_addr + (uint64_t)(i * CHUNK_SIZE);
        wr[i].wr.rdma.rkey        = remote_rkey;

        /* Chain: all except the last point to the next WR */
        wr[i].next = (i < NUM_CHUNKS - 1) ? &wr[i + 1] : NULL;

        printf("  WR[%d]: wr_id=%d, remote_offset=%d, local_offset=%d, "
               "size=%d, next=%s\n",
               i, i, i * CHUNK_SIZE, i * CHUNK_SIZE, CHUNK_SIZE,
               wr[i].next ? "&wr[next]" : "NULL");
    }

    /* Single ibv_post_send() submits the entire WR chain */
    struct ibv_send_wr *bad_wr = NULL;
    printf("\n[Client] Calling ibv_post_send(&wr[0]), submitting %d WRs at once...\n",
           NUM_CHUNKS);

    int ret = ibv_post_send(res->qp, &wr[0], &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[Error] ibv_post_send failed: ret=%d, errno=%d (%s)\n",
                ret, errno, strerror(errno));
        if (bad_wr) {
            fprintf(stderr, "  Failed WR: wr_id=%lu\n",
                    (unsigned long)bad_wr->wr_id);
        }
        return -1;
    }
    printf("[Client] Submission succeeded!\n\n");

    /* Poll CQ, collect 4 completion events */
    printf("[Client] Starting CQ polling, waiting for %d completions...\n", NUM_CHUNKS);

    int completed = 0;
    int errors = 0;

    while (completed < NUM_CHUNKS) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(res->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[Error] ibv_poll_cq returned %d\n", ne);
            return -1;
        }
        if (ne == 0)
            continue;   /* CQ has no completions yet, keep polling */

        completed++;
        printf("\n--- Completion event #%d/%d ---\n", completed, NUM_CHUNKS);
        print_wc_detail(&wc);

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "  [FAIL] WR wr_id=%lu: %s\n",
                    (unsigned long)wc.wr_id, ibv_wc_status_str(wc.status));
            errors++;
            continue;
        }

        /* Print read content for this chunk (first 72 characters preview) */
        int chunk_idx = (int)wc.wr_id;
        if (chunk_idx >= 0 && chunk_idx < NUM_CHUNKS) {
            char preview[80];
            snprintf(preview, sizeof(preview), "%.76s",
                     res->buf + chunk_idx * CHUNK_SIZE);
            printf("  [PASS] wr_id=%d, byte_len=%u\n", chunk_idx, wc.byte_len);
            printf("  Data preview: \"%s...\"\n", preview);
        }
    }

    if (errors > 0) {
        fprintf(stderr, "\n[Client] %d WR(s) failed!\n", errors);
        return -1;
    }

    return 0;
}

/* ========== Cleanup RDMA Resources ========== */
static void cleanup_resources(struct batch_read_ctx *res)
{
    if (res->mr)  ibv_dereg_mr(res->mr);
    if (res->buf) free(res->buf);
    if (res->qp)  ibv_destroy_qp(res->qp);
    if (res->cq)  ibv_destroy_cq(res->cq);
    if (res->pd)  ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
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

    printf("================================================\n");
    printf("  Batch RDMA Read - 4 chained WRs single submit\n");
    printf("  Role: %s\n", is_server ? "Server (data provider)" : "Client (read initiator)");
    printf("  Buffer: %d bytes = %d chunks x %d bytes/chunk\n",
           BUFFER_SIZE, NUM_CHUNKS, CHUNK_SIZE);
    printf("================================================\n\n");

    /* 1. Initialize RDMA resources */
    struct batch_read_ctx res;
    memset(&res, 0, sizeof(res));

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

    /* Attach MR info: remote needs to know our buf address and rkey */
    local_ep.buf_addr = (uint64_t)res.buf;
    local_ep.buf_rkey = res.mr->rkey;

    printf("[Info] Local endpoint: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           local_ep.qp_num, local_ep.lid,
           (unsigned long)local_ep.buf_addr, local_ep.buf_rkey);

    /* Server fills data before exchanging info, ensuring data is ready when Client connects */
    if (is_server) {
        printf("\n");
        server_fill_chunks(res.buf);
    }

    /* 3. TCP out-of-band endpoint info exchange */
    printf("[Info] Exchanging connection info via TCP (port %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[Error] TCP info exchange failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] Remote endpoint: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           remote_ep.qp_num, remote_ep.lid,
           (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);

    /* 4. QP one-shot connection: RESET -> INIT -> RTR -> RTS */
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE;
    if (qp_full_connect(res.qp, &remote_ep, res.port, res.is_roce, access) != 0) {
        fprintf(stderr, "[Error] QP connection failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] QP state: RESET -> INIT -> RTR -> RTS (ready)\n\n");

    /* 5. Execute role-specific operations */
    if (is_server) {
        /*
         * Server side: data is ready, waiting for client to read remotely.
         * RDMA Read is entirely initiated by client, server needs no action (CPU bypass).
         */
        printf("[Server] Data is ready, waiting for Client to perform RDMA Read...\n");
        printf("[Server] (Server needs no action - RDMA Read is entirely Client-driven)\n");
        printf("[Server] Waiting 15 seconds before exit...\n");
        sleep(15);

        /* Verify: RDMA Read does not modify source data */
        printf("\n[Server] Verify: buffer contents unchanged (RDMA Read doesn't modify source):\n");
        for (int i = 0; i < NUM_CHUNKS; i++) {
            char preview[64];
            snprintf(preview, sizeof(preview), "%.60s",
                     res.buf + i * CHUNK_SIZE);
            printf("  Chunk %d: \"%s...\"\n", i, preview);
        }
        printf("[Server] Done.\n");
    } else {
        /* Client: wait for Server data to be ready, then batch RDMA Read */
        printf("[Client] Waiting 1 second for Server to get ready...\n");
        sleep(1);

        printf("[Client] Starting batch RDMA Read:\n");
        printf("  Remote: addr=0x%lx, rkey=0x%x\n",
               (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);
        printf("  Strategy: %d chained WRs, each reading %d bytes\n\n",
               NUM_CHUNKS, CHUNK_SIZE);

        if (client_batch_read(&res, remote_ep.buf_addr, remote_ep.buf_rkey) != 0) {
            fprintf(stderr, "[Client] Batch Read failed\n");
            cleanup_resources(&res);
            return 1;
        }

        /* Summary: print all chunks */
        printf("\n================================================\n");
        printf("[Client] Batch Read complete! Contents of all %d chunks:\n", NUM_CHUNKS);
        printf("================================================\n");
        for (int i = 0; i < NUM_CHUNKS; i++) {
            char preview[80];
            snprintf(preview, sizeof(preview), "%.76s",
                     res.buf + i * CHUNK_SIZE);
            printf("  Chunk %d (offset %4d): \"%s...\"\n",
                   i, i * CHUNK_SIZE, preview);
        }
        printf("================================================\n");
    }

    /* 6. Cleanup resources */
    printf("\n[Info] Cleaning up RDMA resources...\n");
    cleanup_resources(&res);
    return 0;
}
