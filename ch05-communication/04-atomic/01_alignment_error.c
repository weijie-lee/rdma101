/**
 * Atomic Operation Alignment Error Demo
 *
 * Hard requirements for RDMA atomic operations (Fetch-and-Add, Compare-and-Swap):
 *   * Target remote address must be 8-byte aligned (because the operation is on uint64_t, i.e. 8 bytes)
 *   * If the address is not aligned, the NIC hardware cannot perform the atomic operation and returns an error WC
 *
 * This program has two experiments:
 *   Experiment 1: Intentionally use malloc + offset of 3 bytes to create a non-8-byte-aligned address
 *           -> Register MR, execute FAA -> Capture error WC, print with print_wc_detail()
 *   Experiment 2: Use posix_memalign(64, sizeof(uint64_t)) to allocate correctly aligned memory
 *           -> Register new MR, execute FAA -> Success, print old value
 *
 * Run mode: server/client dual-process (loopback mode, exchange endpoint info via TCP)
 *
 * Usage:
 *   Terminal 1: ./01_alignment_error server
 *   Terminal 2: ./01_alignment_error client [server_ip]
 *
 * Build:
 *   gcc -Wall -O2 -g -o 01_alignment_error 01_alignment_error.c \
 *       -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== Constants ========== */
#define RAW_BUF_SIZE    128         /* Raw buffer size (for intentional offset) */
#define TCP_PORT        19885       /* TCP info exchange port */
#define MISALIGN_OFFSET 3           /* Intentionally offset 3 bytes to break 8-byte alignment */

/* ========== RDMA Resource Context ========== */
struct alignment_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;

    /* Intentionally misaligned memory (Experiment 1) */
    char                *raw_buf;       /* malloc raw pointer */
    struct ibv_mr       *raw_mr;

    /* Correctly aligned memory (Experiment 2) */
    uint64_t            *aligned_buf;
    struct ibv_mr       *aligned_mr;

    /* FAA operation result buffer (old value written back locally) */
    uint64_t            *result_buf;
    struct ibv_mr       *result_mr;

    uint8_t              port;
    int                  is_roce;
};

/* ========== Initialize RDMA Resources ========== */
static int init_resources(struct alignment_ctx *res)
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

    /* Auto-detect transport layer: IB or RoCE */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[Info] Transport layer type: %s\n", transport_str(transport));

    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "Failed to allocate protection domain");

    res->cq = ibv_create_cq(res->ctx, 64, NULL, NULL, 0);
    CHECK_NULL(res->cq, "Failed to create completion queue");

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

    /*
     * Allocate raw buffer (for Experiment 1: intentional offset to create misaligned address)
     * Note: ibv_reg_mr registers the entire buffer, but the atomic operation target address
     * is at an offset within the buffer
     */
    res->raw_buf = (char *)malloc(RAW_BUF_SIZE);
    CHECK_NULL(res->raw_buf, "Failed to allocate raw_buf");
    memset(res->raw_buf, 0, RAW_BUF_SIZE);

    /* MR registration must include REMOTE_ATOMIC permission */
    res->raw_mr = ibv_reg_mr(res->pd, res->raw_buf, RAW_BUF_SIZE,
                             IBV_ACCESS_LOCAL_WRITE |
                             IBV_ACCESS_REMOTE_WRITE |
                             IBV_ACCESS_REMOTE_READ |
                             IBV_ACCESS_REMOTE_ATOMIC);
    CHECK_NULL(res->raw_mr, "Failed to register raw MR");

    /*
     * Allocate correctly aligned memory (Experiment 2)
     * posix_memalign: first parameter is alignment boundary (64 bytes, meets 8-byte requirement)
     *                 second parameter is allocation size
     */
    if (posix_memalign((void **)&res->aligned_buf, 64, sizeof(uint64_t)) != 0) {
        fprintf(stderr, "[Error] posix_memalign(aligned_buf) failed\n");
        goto cleanup;
    }
    *res->aligned_buf = 0;  /* Initialize to 0 */

    res->aligned_mr = ibv_reg_mr(res->pd, res->aligned_buf, sizeof(uint64_t),
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_WRITE |
                                 IBV_ACCESS_REMOTE_READ |
                                 IBV_ACCESS_REMOTE_ATOMIC);
    CHECK_NULL(res->aligned_mr, "Failed to register aligned MR");

    /* FAA result buffer: old value written back locally */
    if (posix_memalign((void **)&res->result_buf, 64, sizeof(uint64_t)) != 0) {
        fprintf(stderr, "[Error] posix_memalign(result_buf) failed\n");
        goto cleanup;
    }
    *res->result_buf = 0;

    res->result_mr = ibv_reg_mr(res->pd, res->result_buf, sizeof(uint64_t),
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->result_mr, "Failed to register result MR");

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Execute FAA Operation and Print Result ========== */
static int do_faa(struct alignment_ctx *res, uint64_t target_addr,
                  uint32_t target_rkey, uint64_t add_val, const char *desc)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->result_buf,
        .length = sizeof(uint64_t),
        .lkey   = res->result_mr->lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_ATOMIC_FETCH_AND_ADD;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = target_addr;
    wr.wr.atomic.rkey        = target_rkey;
    wr.wr.atomic.compare_add = add_val;

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  [Error] ibv_post_send failed: ret=%d, errno=%d\n", ret, errno);
        return -1;
    }

    /* Blocking wait for completion */
    struct ibv_wc wc;
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) {
        fprintf(stderr, "  [Error] poll_cq_blocking failed\n");
        return -1;
    }

    /* Print complete WC info with print_wc_detail() */
    printf("  %s - WC details:\n", desc);
    print_wc_detail(&wc);

    if (wc.status == IBV_WC_SUCCESS) {
        printf("  [PASS] FAA +%lu, returned old value = %lu\n\n",
               (unsigned long)add_val, (unsigned long)*res->result_buf);
        return 0;
    } else {
        printf("  [FAIL] Status: %s (code=%d)\n\n",
               ibv_wc_status_str(wc.status), wc.status);
        return -1;
    }
}

/* ========== Cleanup RDMA Resources ========== */
static void cleanup_resources(struct alignment_ctx *res)
{
    if (res->result_mr)  ibv_dereg_mr(res->result_mr);
    if (res->aligned_mr) ibv_dereg_mr(res->aligned_mr);
    if (res->raw_mr)     ibv_dereg_mr(res->raw_mr);
    if (res->result_buf) free(res->result_buf);
    if (res->aligned_buf) free(res->aligned_buf);
    if (res->raw_buf)    free(res->raw_buf);
    if (res->qp)         ibv_destroy_qp(res->qp);
    if (res->cq)         ibv_destroy_cq(res->cq);
    if (res->pd)         ibv_dealloc_pd(res->pd);
    if (res->ctx)        ibv_close_device(res->ctx);
}

/* ========== Rebuild QP (Error Recovery) ========== */
static int rebuild_qp(struct alignment_ctx *res, const struct rdma_endpoint *remote_ep)
{
    /* Destroy old QP that entered Error state */
    ibv_destroy_qp(res->qp);

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
    CHECK_NULL(res->qp, "Failed to rebuild QP");

    /* Reconnect (connect to peer) */
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    int ret = qp_full_connect(res->qp, remote_ep, res->port, res->is_roce, access);
    CHECK_ERRNO(ret, "QP connection failed after rebuild");

    return 0;

cleanup:
    return -1;
}

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s server|client [server_ip]\n", argv[0]);
        fprintf(stderr, "  Terminal 1: %s server\n", argv[0]);
        fprintf(stderr, "  Terminal 2: %s client [server_ip]\n", argv[0]);
        return 1;
    }

    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("========================================================\n");
    printf("  Atomic Operation Alignment Error Demo\n");
    printf("  Role: %s\n", is_server ? "Server" : "Client");
    printf("========================================================\n");
    printf("  Key point: RDMA atomic ops (FAA/CAS) require 8-byte aligned target address\n");
    printf("  Experiment 1: Misaligned address -> triggers error\n");
    printf("  Experiment 2: Correctly aligned  -> executes successfully\n");
    printf("========================================================\n\n");

    /* 1. Initialize RDMA resources */
    struct alignment_ctx res;
    memset(&res, 0, sizeof(res));

    if (init_resources(&res) != 0) {
        fprintf(stderr, "[Error] Failed to initialize RDMA resources\n");
        return 1;
    }

    /* 2. Fill local endpoint info and exchange via TCP */
    struct rdma_endpoint local_ep, remote_ep;
    memset(&local_ep, 0, sizeof(local_ep));
    memset(&remote_ep, 0, sizeof(remote_ep));

    if (fill_local_endpoint(res.ctx, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &local_ep) != 0) {
        fprintf(stderr, "[Error] Failed to fill local endpoint info\n");
        cleanup_resources(&res);
        return 1;
    }

    /*
     * Exchange the misaligned address and rkey with the peer (Experiment 1 target)
     * The peer will attempt to execute FAA on this misaligned address
     */
    uint64_t misaligned_addr = (uint64_t)(res.raw_buf + MISALIGN_OFFSET);
    local_ep.buf_addr = misaligned_addr;
    local_ep.buf_rkey = res.raw_mr->rkey;

    printf("[Info] Local endpoint: QP=%u, LID=%u\n", local_ep.qp_num, local_ep.lid);
    printf("[Info] Misaligned target: addr=0x%lx (raw_buf=%p + %d), rkey=0x%x\n",
           (unsigned long)misaligned_addr, (void *)res.raw_buf,
           MISALIGN_OFFSET, res.raw_mr->rkey);

    printf("\n[Info] TCP info exchange (port %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[Error] TCP info exchange failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] Remote endpoint: QP=%u, buf_addr=0x%lx, rkey=0x%x\n",
           remote_ep.qp_num,
           (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);

    /* 3. QP connection: RESET -> INIT -> RTR -> RTS */
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (qp_full_connect(res.qp, &remote_ep, res.port, res.is_roce, access) != 0) {
        fprintf(stderr, "[Error] QP connection failed\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[Info] QP connection complete (RESET -> INIT -> RTR -> RTS)\n\n");

    /*
     * Only client initiates atomic operations (Experiments 1 and 2).
     * Server acts as the "target", its memory is modified remotely by atomic ops.
     */
    if (!is_server) {
        /* ====== Experiment 1: Execute FAA on misaligned address ====== */
        printf("========================================\n");
        printf("  Experiment 1: FAA on misaligned address\n");
        printf("========================================\n\n");

        printf("  Remote target address: 0x%lx\n", (unsigned long)remote_ep.buf_addr);
        printf("  Address %% 8 = %lu", (unsigned long)(remote_ep.buf_addr % 8));
        if (remote_ep.buf_addr % 8 != 0) {
            printf(" (NOT 8-byte aligned!)\n");
        } else {
            printf(" (aligned)\n");
        }
        printf("\n  Attempting FAA +1 to misaligned address...\n\n");

        int ret1 = do_faa(&res, remote_ep.buf_addr, remote_ep.buf_rkey,
                          1, "Experiment 1 (misaligned)");

        if (ret1 != 0) {
            printf("  As expected: misaligned address caused FAA failure!\n");
            printf("  QP has entered ERROR state, needs rebuild\n\n");

            /* Print current QP state */
            print_qp_state(res.qp);
            printf("\n");

            /* Rebuild QP to continue Experiment 2 */
            printf("  Rebuilding QP and reconnecting...\n");
            if (rebuild_qp(&res, &remote_ep) != 0) {
                fprintf(stderr, "[Error] QP rebuild failed, cannot continue Experiment 2\n");
                cleanup_resources(&res);
                return 1;
            }
            printf("  QP rebuilt and reconnected\n\n");
        } else {
            printf("  (Note: some NICs/drivers may not check alignment errors)\n\n");
        }

        /* ====== Experiment 2: Execute FAA on correctly aligned address ====== */
        printf("========================================\n");
        printf("  Experiment 2: FAA on correctly aligned address\n");
        printf("========================================\n\n");

        /*
         * Memory allocated with posix_memalign(64, sizeof(uint64_t))
         * Need to inform the peer of the aligned address and new rkey...
         * but for simplicity, here we use local loopback: execute FAA on our own aligned_buf
         * (QP is connected to peer, but we can also use our own MR)
         *
         * Cleaner approach: directly use server's aligned_buf
         * But here we use the already registered aligned_mr to demonstrate correct alignment
         */
        uint64_t aligned_addr = (uint64_t)res.aligned_buf;
        printf("  Aligned buffer address: %p\n", (void *)res.aligned_buf);
        printf("  Address %% 8 = %lu", (unsigned long)(aligned_addr % 8));
        if (aligned_addr % 8 == 0) {
            printf(" (8-byte aligned)\n");
        } else {
            printf(" (not aligned)\n");
        }
        printf("  Initial value: %lu\n\n", (unsigned long)*res.aligned_buf);

        /* First FAA: +1 */
        printf("  Attempting FAA +1 to aligned address...\n\n");
        int ret2 = do_faa(&res, aligned_addr, res.aligned_mr->rkey,
                          1, "Experiment 2 (aligned) FAA +1");

        if (ret2 == 0) {
            printf("  Current value: %lu\n\n", (unsigned long)*res.aligned_buf);

            /* Second FAA: +99 */
            printf("  Attempting FAA +99 to aligned address...\n\n");
            do_faa(&res, aligned_addr, res.aligned_mr->rkey,
                   99, "Experiment 2 (aligned) FAA +99");
            printf("  Final value: %lu (expected 100)\n",
                   (unsigned long)*res.aligned_buf);
        }
    } else {
        /* Server: wait for client to complete operations */
        printf("[Server] Waiting for Client to perform atomic operation experiments...\n");
        printf("[Server] (Server memory is modified remotely by atomic ops, no local action needed)\n");
        sleep(10);
        printf("[Server] raw_buf at offset %d value: ", MISALIGN_OFFSET);
        /* Try to read (may read abnormal values due to alignment issues) */
        uint64_t val;
        memcpy(&val, res.raw_buf + MISALIGN_OFFSET, sizeof(uint64_t));
        printf("%lu\n", (unsigned long)val);
    }

    /* Summary */
    printf("\n========================================\n");
    printf("  Summary: Atomic Operation 8-byte Alignment Requirement\n");
    printf("========================================\n");
    printf("  1. RDMA atomic operations (FAA/CAS) operate on uint64_t (8 bytes)\n");
    printf("  2. NIC hardware requires target remote address to be 8-byte aligned\n");
    printf("  3. Recommended allocation methods:\n");
    printf("     - posix_memalign(&ptr, 64, sizeof(uint64_t))\n");
    printf("     - aligned_alloc(8, sizeof(uint64_t))\n");
    printf("     - __attribute__((aligned(8))) uint64_t counter;\n");
    printf("  4. malloc() typically returns 16-byte aligned addresses (meets requirement)\n");
    printf("     but manual offset can break alignment!\n");

    printf("\n[Info] Program finished, cleaning up resources...\n");
    cleanup_resources(&res);
    return 0;
}
