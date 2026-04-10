/**
 * Distributed Spinlock - Based on RDMA CAS (Compare-and-Swap)
 *
 * Principle:
 *   In distributed systems, multiple nodes need mutual exclusion to access shared resources.
 *   Traditional approaches rely on a centralized lock server, requiring network round-trips (RTT)
 *   and server CPU involvement for each lock/unlock operation.
 *
 *   A distributed lock based on RDMA atomic operations can completely bypass the server CPU:
 *     - Server only needs to maintain lock variable and shared data memory, without participating in any lock logic
 *     - Client directly operates on the lock variable in server memory via RDMA CAS
 *     - Fully zero-copy, zero CPU intervention (one-sided RDMA)
 *
 *   Lock variable semantics:
 *     uint64_t lock: 0 = unlocked, 1 = locked
 *
 *   Acquire Lock: CAS(lock, expected=0, new=1)
 *     - If remote lock == 0 -> atomically set to 1, returns old value 0 -> lock acquired
 *     - If remote lock != 0 -> no modification, returns old value 1 -> lock held, spin retry
 *
 *   Release Lock: CAS(lock, expected=1, new=0)
 *     - If remote lock == 1 -> atomically set to 0, returns old value 1 -> unlock succeeded
 *     - If remote lock != 1 -> abnormal state (attempting to release an unheld lock)
 *
 * Use cases:
 *   - Distributed KV stores (e.g., FaRM, DrTM)
 *   - Distributed filesystem metadata mutual exclusion
 *   - RDMA-based shared memory concurrency control
 *
 * Usage:
 *   Server: ./02_spinlock server
 *   Client: ./02_spinlock client <server_ip>
 *
 * Build: gcc -o 02_spinlock 02_spinlock.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define TCP_PORT        7780
#define DATA_BUF_SIZE   256         /* Shared data region size */
#define MAX_SPIN_COUNT  1000000     /* Spin limit to prevent deadlock/infinite wait */

/* ========== Extended Connection Info ========== */

/**
 * In addition to the standard rdma_endpoint (QP number, LID, GID, etc.),
 * the distributed lock also needs to exchange the remote addresses/rkeys
 * of the lock variable and data region, since rdma_endpoint only provides
 * one set of buf_addr/buf_rkey.
 */
struct spinlock_conn_info {
    struct rdma_endpoint ep;        /* Standard endpoint info */
    uint64_t    lock_addr;          /* Lock variable remote virtual address */
    uint32_t    lock_rkey;          /* Lock variable MR rkey */
    uint64_t    data_addr;          /* Shared data region remote virtual address */
    uint32_t    data_rkey;          /* Shared data region MR rkey */
};

/* ========== TCP Exchange Extended Connection Info ========== */

/**
 * exchange_spinlock_conn_tcp - Exchange distributed lock connection info via TCP
 *
 * @server_ip: NULL=act as server listening; non-NULL=act as client connecting
 * @local:     Local info (input)
 * @remote:    Peer info (output)
 *
 * Returns: 0 success, -1 failure
 */
static int exchange_spinlock_conn_tcp(const char *server_ip,
                                      const struct spinlock_conn_info *local,
                                      struct spinlock_conn_info *remote)
{
    int sock = -1, conn = -1, ret = -1;
    int reuse = 1;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_PORT);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("[TCP] socket"); return -1; }

    if (server_ip == NULL) {
        /* Server: listen and wait for client connection */
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[TCP] bind"); goto out;
        }
        if (listen(sock, 1) < 0) {
            perror("[TCP] listen"); goto out;
        }
        printf("[TCP] Waiting for client connection (port %d)...\n", TCP_PORT);
        conn = accept(sock, NULL, NULL);
        if (conn < 0) { perror("[TCP] accept"); goto out; }

        /* Server: receive first, then send */
        if (recv(conn, remote, sizeof(*remote), MSG_WAITALL) != (ssize_t)sizeof(*remote)) {
            perror("[TCP] recv"); goto out;
        }
        if (send(conn, local, sizeof(*local), 0) != (ssize_t)sizeof(*local)) {
            perror("[TCP] send"); goto out;
        }
    } else {
        /* Client: connect to server */
        inet_pton(AF_INET, server_ip, &addr.sin_addr);
        printf("[TCP] Connecting to server %s:%d ...\n", server_ip, TCP_PORT);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[TCP] connect"); goto out;
        }
        conn = sock;
        sock = -1;

        /* Client: send first, then receive */
        if (send(conn, local, sizeof(*local), 0) != (ssize_t)sizeof(*local)) {
            perror("[TCP] send"); goto out;
        }
        if (recv(conn, remote, sizeof(*remote), MSG_WAITALL) != (ssize_t)sizeof(*remote)) {
            perror("[TCP] recv"); goto out;
        }
    }
    ret = 0;

out:
    if (conn >= 0) close(conn);
    if (sock >= 0) close(sock);
    return ret;
}

/* ========== CAS Atomic Operation Wrapper ========== */

/**
 * post_cas_and_poll - Execute one CAS atomic operation and wait for completion
 *
 * CAS semantics: atomically compare the value at remote_addr with expected,
 *   if equal then replace with new_val, and write the pre-operation old value into result_buf.
 *
 * @qp:          Queue pair
 * @cq:          Completion queue
 * @result_addr: Local result buffer address (old value written back here)
 * @result_lkey: Result buffer MR lkey
 * @remote_addr: Remote lock variable address
 * @remote_rkey: Remote lock variable MR rkey
 * @expected:    Expected old value
 * @new_val:     New value to write
 *
 * Returns: 0 success, -1 failure
 */
static int post_cas_and_poll(struct ibv_qp *qp, struct ibv_cq *cq,
                             uint64_t result_addr, uint32_t result_lkey,
                             uint64_t remote_addr, uint32_t remote_rkey,
                             uint64_t expected, uint64_t new_val)
{
    struct ibv_sge sge = {
        .addr   = result_addr,
        .length = sizeof(uint64_t),
        .lkey   = result_lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_ATOMIC_CMP_AND_SWP;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = remote_addr;
    wr.wr.atomic.rkey        = remote_rkey;
    wr.wr.atomic.compare_add = expected;   /* Compare value */
    wr.wr.atomic.swap        = new_val;    /* Swap value */

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("[CAS] ibv_post_send failed");
        return -1;
    }

    /* Blocking wait for completion */
    struct ibv_wc wc;
    if (poll_cq_blocking(cq, &wc) != 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[CAS] WC failed: %s\n",
                wc.status != IBV_WC_SUCCESS ? ibv_wc_status_str(wc.status) : "poll error");
        return -1;
    }
    return 0;
}

/* ========== RDMA Write Wrapper ========== */

/**
 * post_write_and_poll - Execute one RDMA Write and wait for completion
 *
 * Writes local data unilaterally to remote memory without notifying remote CPU (one-sided).
 */
static int post_write_and_poll(struct ibv_qp *qp, struct ibv_cq *cq,
                               uint64_t local_addr, uint32_t local_lkey,
                               uint32_t length,
                               uint64_t remote_addr, uint32_t remote_rkey)
{
    struct ibv_sge sge = {
        .addr   = local_addr,
        .length = length,
        .lkey   = local_lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 1;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("[WRITE] ibv_post_send failed");
        return -1;
    }

    struct ibv_wc wc;
    if (poll_cq_blocking(cq, &wc) != 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[WRITE] WC failed: %s\n",
                wc.status != IBV_WC_SUCCESS ? ibv_wc_status_str(wc.status) : "poll error");
        return -1;
    }
    return 0;
}

/* ========== Main Program ========== */

int main(int argc, char *argv[])
{
    /* --- Argument parsing --- */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s server|client [server_ip]\n", argv[0]);
        return 1;
    }
    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("=== RDMA Distributed Spinlock (CAS) ===\n");
    printf("Role: %s\n\n", is_server ? "Server (lock/data holder)" : "Client (lock requester)");

    /* --- RDMA resource declarations --- */
    struct ibv_device     **dev_list  = NULL;
    struct ibv_context     *ctx       = NULL;
    struct ibv_pd          *pd        = NULL;
    struct ibv_cq          *cq        = NULL;
    struct ibv_qp          *qp        = NULL;

    /* Server: lock variable and shared data (independent posix_memalign, each with its own MR) */
    uint64_t  *lock_var     = NULL;     /* Lock: 0=unlocked, 1=locked */
    uint64_t  *shared_data  = NULL;     /* Shared data region */
    struct ibv_mr *lock_mr  = NULL;
    struct ibv_mr *data_mr  = NULL;

    /* Client: CAS result buffer + RDMA Write source data buffer */
    uint64_t  *result_buf   = NULL;     /* CAS returns old value written here */
    char      *write_buf    = NULL;     /* RDMA Write source data */
    struct ibv_mr *result_mr = NULL;
    struct ibv_mr *write_mr  = NULL;

    int ret = 1;
    int num_devices;

    /* ========== Step 1: Open RDMA device ========== */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }
    printf("[Step 1] Device: %s (total %d)\n",
           ibv_get_device_name(dev_list[0]), num_devices);

    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "Failed to open device");

    /* Auto-detect IB/RoCE transport layer type */
    enum rdma_transport transport = detect_transport(ctx, RDMA_DEFAULT_PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  Transport layer: %s\n\n", transport_str(transport));

    /* ========== Step 2: Create PD / CQ / QP ========== */
    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "Failed to allocate PD");

    cq = ibv_create_cq(ctx, 128, NULL, NULL, 0);
    CHECK_NULL(cq, "Failed to create CQ");

    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = 64;
    qp_init_attr.cap.max_recv_wr  = 64;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(pd, &qp_init_attr);
    CHECK_NULL(qp, "Failed to create QP");
    printf("[Step 2] PD/CQ/QP created (QP num=%u)\n", qp->qp_num);

    /* ========== Step 3: Allocate memory + Register MR ========== */
    /*
     * Lock variable and shared data each use posix_memalign to 64-byte boundary:
     *   - Atomic operations require target address to be at least 8-byte aligned
     *   - 64-byte alignment avoids false sharing (cache line = 64B)
     *
     * MR registration must enable:
     *   REMOTE_WRITE  - Allow client RDMA Write to write data
     *   REMOTE_READ   - Allow client RDMA Read to read data (optional)
     *   REMOTE_ATOMIC - Allow client CAS/FAA atomic operations to modify lock
     *   LOCAL_WRITE   - Allow RDMA hardware to write to local memory (required on receive side)
     */
    int access_flags = IBV_ACCESS_LOCAL_WRITE  | IBV_ACCESS_REMOTE_WRITE |
                       IBV_ACCESS_REMOTE_READ  | IBV_ACCESS_REMOTE_ATOMIC;

    /* Lock variable: uint64_t, 64-byte aligned */
    if (posix_memalign((void **)&lock_var, 64, sizeof(uint64_t)) != 0) {
        perror("posix_memalign(lock_var)");
        goto cleanup;
    }
    *lock_var = 0;  /* Initial: unlocked */

    lock_mr = ibv_reg_mr(pd, lock_var, sizeof(uint64_t), access_flags);
    CHECK_NULL(lock_mr, "Failed to register lock MR");

    /* Shared data region: posix_memalign to 64 bytes */
    if (posix_memalign((void **)&shared_data, 64, DATA_BUF_SIZE) != 0) {
        perror("posix_memalign(shared_data)");
        goto cleanup;
    }
    memset(shared_data, 0, DATA_BUF_SIZE);

    data_mr = ibv_reg_mr(pd, shared_data, DATA_BUF_SIZE, access_flags);
    CHECK_NULL(data_mr, "Failed to register data MR");

    /* CAS result buffer (used by client, also allocated on server for code simplicity) */
    if (posix_memalign((void **)&result_buf, 64, sizeof(uint64_t)) != 0) {
        perror("posix_memalign(result_buf)");
        goto cleanup;
    }
    *result_buf = 0;

    result_mr = ibv_reg_mr(pd, result_buf, sizeof(uint64_t), IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(result_mr, "Failed to register result MR");

    /* RDMA Write source data buffer (used by client) */
    if (posix_memalign((void **)&write_buf, 64, DATA_BUF_SIZE) != 0) {
        perror("posix_memalign(write_buf)");
        goto cleanup;
    }
    memset(write_buf, 0, DATA_BUF_SIZE);

    write_mr = ibv_reg_mr(pd, write_buf, DATA_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(write_mr, "Failed to register write MR");

    printf("[Step 3] Memory allocation + MR registration complete\n");
    printf("  lock_var:    addr=%p, rkey=0x%x (initial value=%lu)\n",
           (void *)lock_var, lock_mr->rkey, (unsigned long)*lock_var);
    printf("  shared_data: addr=%p, rkey=0x%x\n",
           (void *)shared_data, data_mr->rkey);
    printf("  result_buf:  addr=%p, lkey=0x%x\n",
           (void *)result_buf, result_mr->lkey);
    printf("  write_buf:   addr=%p, lkey=0x%x\n\n",
           (void *)write_buf, write_mr->lkey);

    /* ========== Step 4: Exchange connection info (TCP out-of-band) ========== */
    struct spinlock_conn_info local_info, remote_info;
    memset(&local_info, 0, sizeof(local_info));
    memset(&remote_info, 0, sizeof(remote_info));

    /* Fill standard endpoint info (QP number, LID, GID, etc.) */
    int fill_ret = fill_local_endpoint(ctx, qp, RDMA_DEFAULT_PORT_NUM,
                                       RDMA_DEFAULT_GID_INDEX, &local_info.ep);
    CHECK_ERRNO(fill_ret, "Failed to fill endpoint info");

    /* Fill lock and data remote addresses/rkeys */
    local_info.lock_addr = (uint64_t)(uintptr_t)lock_var;
    local_info.lock_rkey = lock_mr->rkey;
    local_info.data_addr = (uint64_t)(uintptr_t)shared_data;
    local_info.data_rkey = data_mr->rkey;

    printf("[Step 4] TCP info exchange (port %d)...\n", TCP_PORT);
    if (exchange_spinlock_conn_tcp(server_ip, &local_info, &remote_info) != 0) {
        fprintf(stderr, "[Error] TCP info exchange failed\n");
        goto cleanup;
    }

    printf("  Local: QP=%u, lock=0x%lx, data=0x%lx\n",
           local_info.ep.qp_num,
           (unsigned long)local_info.lock_addr,
           (unsigned long)local_info.data_addr);
    printf("  Remote: QP=%u, lock=0x%lx (rkey=0x%x), data=0x%lx (rkey=0x%x)\n",
           remote_info.ep.qp_num,
           (unsigned long)remote_info.lock_addr, remote_info.lock_rkey,
           (unsigned long)remote_info.data_addr, remote_info.data_rkey);

    /* QP state transition: RESET -> INIT -> RTR -> RTS */
    int conn_ret = qp_full_connect(qp, &remote_info.ep,
                                   RDMA_DEFAULT_PORT_NUM, is_roce,
                                   access_flags);
    CHECK_ERRNO(conn_ret, "QP connection failed");
    printf("  QP connection ready (RESET->INIT->RTR->RTS)\n\n");

    /* ========== Step 5: Distributed lock operations ========== */

    if (is_server) {
        /* ======= Server: passive wait, only maintains memory ======= */
        /*
         * Core feature of distributed lock: server CPU does not participate in lock operations.
         * Client directly modifies the lock_var in server memory via RDMA CAS,
         * and directly writes to shared_data in server memory via RDMA Write.
         * Server only needs to keep memory accessible (truly one-sided RDMA).
         */
        printf("=== Server: Waiting for client operations ===\n");
        printf("  lock_var address: %p, rkey=0x%x\n", (void *)lock_var, lock_mr->rkey);
        printf("  shared_data address: %p, rkey=0x%x\n", (void *)shared_data, data_mr->rkey);
        printf("  Current lock = %lu (0=unlocked)\n\n",
               (unsigned long)*lock_var);

        /* Periodically print lock and data status to observe client's remote operations */
        printf("  Waiting for client to complete operations (printing status every second)...\n");
        for (int i = 0; i < 12; i++) {
            sleep(1);
            printf("  [t=%2ds] lock=%lu (%s), shared_data=\"%s\"\n",
                   i + 1,
                   (unsigned long)*lock_var,
                   *lock_var == 0 ? "unlocked" : "locked",
                   (char *)shared_data);
        }

        /* Final status print */
        printf("\n=== Server: Final state ===\n");
        printf("  lock_var    = %lu (%s)\n",
               (unsigned long)*lock_var,
               *lock_var == 0 ? "unlocked - client correctly released the lock" :
                                "locked - abnormal! lock not released");
        printf("  shared_data = \"%s\"\n", (char *)shared_data);

    } else {
        /* ======= Client: acquire lock -> write data -> release lock ======= */
        int spin_count;
        printf("=== Client: Distributed lock operation flow ===\n\n");

        sleep(1);  /* Wait for server to be ready */

        /* --- Phase 1: Acquire Lock --- */
        /*
         * Spin acquire algorithm:
         *   do {
         *       old_val = CAS(remote_lock, expected=0, new=1);
         *   } while (old_val != 0);
         *
         * result_buf stores the old value before CAS:
         *   old value == 0: was unlocked -> successfully set to 1 -> lock acquired
         *   old value == 1: lock held -> not modified -> continue spinning
         */
        printf("[Phase 1] Acquiring distributed lock (CAS: 0->1 spin)\n");
        printf("  Target: remote lock_addr=0x%lx, lock_rkey=0x%x\n\n",
               (unsigned long)remote_info.lock_addr, remote_info.lock_rkey);

        spin_count = 0;
        while (spin_count < MAX_SPIN_COUNT) {
            spin_count++;

            /* CAS: if remote lock==0 then set to 1 */
            if (post_cas_and_poll(qp, cq,
                                  (uint64_t)(uintptr_t)result_buf, result_mr->lkey,
                                  remote_info.lock_addr, remote_info.lock_rkey,
                                  0,    /* expected: unlocked */
                                  1     /* new: locked */
                                 ) != 0) {
                fprintf(stderr, "[Error] CAS lock acquire operation failed\n");
                goto cleanup;
            }

            /* Check returned old value */
            if (*result_buf == 0) {
                /* old value=0 -> lock acquired! */
                printf("  Lock acquired! (attempt #%d, returned old value=%lu)\n\n",
                       spin_count, (unsigned long)*result_buf);
                break;
            }

            /* old value!=0 -> lock held, continue spinning */
            if (spin_count <= 3 || spin_count % 1000 == 0) {
                printf("  [Spin] Attempt #%d: lock held (old value=%lu), retrying...\n",
                       spin_count, (unsigned long)*result_buf);
            }
        }

        if (spin_count >= MAX_SPIN_COUNT) {
            fprintf(stderr, "[Error] Spun %d times without acquiring lock, suspected deadlock\n", MAX_SPIN_COUNT);
            goto cleanup;
        }

        /* --- Phase 2: Critical section - write shared data via RDMA Write --- */
        /*
         * At this point the client holds the lock (remote lock_var == 1).
         * Under lock protection, write data to server's shared_data via RDMA Write.
         * This simulates modifying shared resources within a distributed critical section.
         *
         * Note: RDMA Write is a one-sided operation:
         *   - Data is DMA'd directly from client memory to server memory
         *   - Server CPU is completely unaware of this write
         *   - No interrupts or context switches
         */
        printf("[Phase 2] Critical section: RDMA Write to shared data\n");
        const char *msg = "DATA_FROM_CLIENT";
        strncpy(write_buf, msg, DATA_BUF_SIZE - 1);
        printf("  Writing: \"%s\" (%zu bytes)\n", msg, strlen(msg) + 1);

        if (post_write_and_poll(qp, cq,
                                (uint64_t)(uintptr_t)write_buf, write_mr->lkey,
                                (uint32_t)(strlen(msg) + 1),
                                remote_info.data_addr, remote_info.data_rkey
                               ) != 0) {
            fprintf(stderr, "[Error] RDMA Write failed (still need to release lock)\n");
            /* Must release lock even if write fails, otherwise deadlock */
        } else {
            printf("  RDMA Write complete\n\n");
        }

        /* --- Phase 3: Release Lock --- */
        /*
         * CAS(lock, expected=1, new=0):
         *   old value == 1: was locked -> successfully set to 0 -> unlock succeeded
         *   old value != 1: abnormal (should not happen - no one can modify it while we hold the lock)
         */
        printf("[Phase 3] Releasing distributed lock (CAS: 1->0)\n");

        if (post_cas_and_poll(qp, cq,
                              (uint64_t)(uintptr_t)result_buf, result_mr->lkey,
                              remote_info.lock_addr, remote_info.lock_rkey,
                              1,    /* expected: locked */
                              0     /* new: unlocked */
                             ) != 0) {
            fprintf(stderr, "[Error] CAS unlock operation failed\n");
            goto cleanup;
        }

        if (*result_buf == 1) {
            printf("  Lock released! (old value=%lu -> new value=0)\n\n",
                   (unsigned long)*result_buf);
        } else {
            fprintf(stderr, "  [Warning] Unlock anomaly: returned old value=%lu (expected=1)\n\n",
                    (unsigned long)*result_buf);
        }

        /* --- Operation summary --- */
        printf("=== Client: Operation complete ===\n");
        printf("  1. Acquire lock (CAS 0->1)           Done\n");
        printf("  2. Write shared data (RDMA Write)     Done\n");
        printf("  3. Release lock (CAS 1->0)            Done\n");
    }

    ret = 0;

cleanup:
    /* ========== Resource cleanup (reverse order) ========== */
    printf("\n[Cleanup] Releasing RDMA resources...\n");
    if (write_mr)   ibv_dereg_mr(write_mr);
    if (result_mr)  ibv_dereg_mr(result_mr);
    if (data_mr)    ibv_dereg_mr(data_mr);
    if (lock_mr)    ibv_dereg_mr(lock_mr);
    if (qp)         ibv_destroy_qp(qp);
    if (cq)         ibv_destroy_cq(cq);
    if (pd)         ibv_dealloc_pd(pd);
    if (ctx)        ibv_close_device(ctx);
    if (dev_list)   ibv_free_device_list(dev_list);
    if (write_buf)  free(write_buf);
    if (result_buf) free(result_buf);
    if (shared_data) free(shared_data);
    if (lock_var)   free(lock_var);
    printf("  Cleanup complete\n");

    return ret;
}
