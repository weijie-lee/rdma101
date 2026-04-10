/**
 * MR Access Flags Demo
 *
 * This program demonstrates the effects of different access flags in ibv_reg_mr():
 *   1. Register MRs with various flag combinations, print lkey/rkey
 *   2. Show: LOCAL_WRITE only, LOCAL_WRITE+REMOTE_READ, +REMOTE_WRITE, +REMOTE_ATOMIC
 *   3. Use loopback QP to test: attempt RDMA Write to read-only MR -> produces protection error
 *   4. Print system memory locking limit info (ulimit -l, nr_hugepages)
 *
 * Compile: gcc -o mr_access_flags mr_access_flags.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE 4096
#define PORT_NUM    RDMA_DEFAULT_PORT_NUM
#define CQ_DEPTH    32

/**
 * Helper function: convert MR access flags to readable string
 */
static void print_access_flags(int flags)
{
    printf("    Flag combination: ");
    if (flags & IBV_ACCESS_LOCAL_WRITE)   printf("LOCAL_WRITE ");
    if (flags & IBV_ACCESS_REMOTE_WRITE)  printf("REMOTE_WRITE ");
    if (flags & IBV_ACCESS_REMOTE_READ)   printf("REMOTE_READ ");
    if (flags & IBV_ACCESS_REMOTE_ATOMIC) printf("REMOTE_ATOMIC ");
    if (flags == 0)                        printf("(none -- local read-only)");
    printf("\n");
}

/**
 * Helper function: print system memory locking limits
 */
static void print_memory_limits(void)
{
    FILE *fp;
    char line[256];

    printf("\n=== System Memory Locking Info ===\n");

    /* ulimit -l (read via /proc/self/limits) */
    fp = fopen("/proc/self/limits", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "Max locked memory")) {
                printf("  %s", line);
                break;
            }
        }
        fclose(fp);
    }

    /* /proc/sys/vm/nr_hugepages */
    fp = fopen("/proc/sys/vm/nr_hugepages", "r");
    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            printf("  /proc/sys/vm/nr_hugepages = %s", line);
        }
        fclose(fp);
    }

    printf("  Note: MR registration requires pinned physical memory\n");
    printf("  If ulimit -l is too small, ibv_reg_mr will fail (errno=ENOMEM)\n");
    printf("  Fix: ulimit -l unlimited or modify /etc/security/limits.conf\n");
    printf("============================\n\n");
}

int main(int argc, char *argv[])
{
    /* Resource declarations */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_cq      *cq       = NULL;
    struct ibv_qp      *qp       = NULL;
    struct ibv_mr      *mr_arr[4] = {NULL};  /* 4 MRs with different flags */
    char               *buf_arr[4] = {NULL};
    struct ibv_mr      *mr_src   = NULL;     /* Source MR for loopback test */
    char               *buf_src  = NULL;
    int                 num_devices;
    int                 ret;
    int                 i;

    printf("============================================\n");
    printf("  MR Access Flags Demo\n");
    printf("============================================\n\n");

    /* Print system memory limits */
    print_memory_limits();

    /* ========== Open device, create PD ========== */
    printf("[Step 1] Open device and create PD\n");
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
    printf("  PD allocated successfully\n\n");

    /* ========== Step 2: Register MRs with different flag combinations ========== */
    printf("[Step 2] Register MRs with different access flags\n");
    printf("========================================\n");

    /* Define 4 flag combinations */
    int flag_combos[] = {
        /* Combo 1: Local write only (minimal permission, for locally-used buffers) */
        IBV_ACCESS_LOCAL_WRITE,
        /* Combo 2: Local write + remote read (allow peer to RDMA Read this MR) */
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ,
        /* Combo 3: Local write + remote write (allow peer to RDMA Write this MR) */
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE,
        /* Combo 4: Full permissions (including remote atomic operations) */
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
        IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC,
    };
    const char *combo_desc[] = {
        "Local write only (LOCAL_WRITE)",
        "Local write + remote read (LOCAL_WRITE | REMOTE_READ)",
        "Local write + remote write (LOCAL_WRITE | REMOTE_WRITE)",
        "Full permissions (LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE | REMOTE_ATOMIC)",
    };

    for (i = 0; i < 4; i++) {
        printf("\n  --- MR[%d]: %s ---\n", i, combo_desc[i]);
        print_access_flags(flag_combos[i]);

        buf_arr[i] = malloc(BUFFER_SIZE);
        CHECK_NULL(buf_arr[i], "malloc failed");
        memset(buf_arr[i], 'A' + i, BUFFER_SIZE);

        mr_arr[i] = ibv_reg_mr(pd, buf_arr[i], BUFFER_SIZE, flag_combos[i]);
        CHECK_NULL(mr_arr[i], "ibv_reg_mr failed");

        printf("    lkey = 0x%08x (local access key)\n", mr_arr[i]->lkey);
        printf("    rkey = 0x%08x (remote access key)\n", mr_arr[i]->rkey);
        printf("    addr = %p, length = %zu\n",
               mr_arr[i]->addr, (size_t)mr_arr[i]->length);
    }

    printf("\n  Key observations: Each MR has different lkey/rkey values\n");
    printf("  lkey is used in SGE for local WRs, rkey must be shared with peer for RDMA Read/Write\n");
    printf("  REMOTE_* flags control whether the peer can RDMA Read/Write/Atomic this region\n\n");

    /* ========== Step 3: Create loopback QP to test RDMA Write ========== */
    printf("[Step 3] Create loopback QP, test RDMA Write permission check\n");
    printf("========================================\n\n");

    cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq, "Failed to create CQ");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
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

    /* Prepare source MR (source data for RDMA Write) */
    buf_src = malloc(BUFFER_SIZE);
    CHECK_NULL(buf_src, "malloc buf_src failed");
    memset(buf_src, 'S', BUFFER_SIZE);
    mr_src = ibv_reg_mr(pd, buf_src, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr_src, "Failed to register source MR");

    /* Prepare loopback endpoint info */
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "Failed to fill endpoint info");

    /* Detect transport type */
    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  Transport type: %s\n", transport_str(transport));

    /* Connect QP with full permissions (loopback) */
    int full_access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, full_access);
    if (ret != 0) {
        printf("  QP connection failed (loopback), skipping RDMA Write test\n");
        printf("  -> Hint: Ensure port state is ACTIVE\n");
        goto skip_rdma_test;
    }
    printf("  QP connected (loopback, RESET->INIT->RTR->RTS)\n\n");

    /*
     * Test: RDMA Write to MR[0] (LOCAL_WRITE only, no REMOTE_WRITE permission)
     * Expected: HCA produces IBV_WC_REM_ACCESS_ERR (Remote Access Error)
     */
    printf("  Test: RDMA Write -> MR[0] (LOCAL_WRITE only, no REMOTE_WRITE)\n");
    printf("  Target rkey=0x%08x, expecting Remote Access Error...\n", mr_arr[0]->rkey);

    struct ibv_sge sge_test = {
        .addr   = (uintptr_t)buf_src,
        .length = 64,
        .lkey   = mr_src->lkey,
    };
    struct ibv_send_wr wr_test = {
        .wr_id      = 100,
        .sg_list    = &sge_test,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = (uintptr_t)buf_arr[0],
            .rkey        = mr_arr[0]->rkey,  /* Target MR has no REMOTE_WRITE */
        },
    };
    struct ibv_send_wr *bad_wr = NULL;

    ret = ibv_post_send(qp, &wr_test, &bad_wr);
    if (ret != 0) {
        printf("  ibv_post_send failed: %s\n", strerror(errno));
    } else {
        /* Poll CQ for result */
        struct ibv_wc wc;
        int ne;
        int poll_count = 0;
        while (poll_count < 1000000) {
            ne = ibv_poll_cq(cq, 1, &wc);
            if (ne > 0) {
                printf("  CQ result: status=%s (%d)\n",
                       ibv_wc_status_str(wc.status), wc.status);
                if (wc.status != IBV_WC_SUCCESS) {
                    printf("  Expected permission error! RDMA Write rejected (MR has no REMOTE_WRITE)\n");
                }
                break;
            }
            if (ne < 0) {
                printf("  ibv_poll_cq error\n");
                break;
            }
            poll_count++;
        }
        if (poll_count >= 1000000) {
            printf("  Polling timed out, no completion event received\n");
        }
    }

skip_rdma_test:
    /* ========== Summary ========== */
    printf("\n============================================\n");
    printf("  MR Access Flags Summary\n");
    printf("============================================\n");
    printf("  IBV_ACCESS_LOCAL_WRITE:    Allow local QP to write to this MR\n");
    printf("  IBV_ACCESS_REMOTE_READ:    Allow peer to RDMA Read this MR\n");
    printf("  IBV_ACCESS_REMOTE_WRITE:   Allow peer to RDMA Write this MR\n");
    printf("  IBV_ACCESS_REMOTE_ATOMIC:  Allow peer to perform atomic operations on this MR\n");
    printf("  Least privilege principle: Only enable flags that are actually needed\n");
    printf("  REMOTE_WRITE/REMOTE_ATOMIC both implicitly require LOCAL_WRITE\n");
    printf("  Registering MR pins physical pages, watch ulimit -l limits\n\n");

cleanup:
    printf("[Cleanup] Releasing resources...\n");
    if (qp)     ibv_destroy_qp(qp);
    if (cq)     ibv_destroy_cq(cq);
    if (mr_src) ibv_dereg_mr(mr_src);
    if (buf_src) free(buf_src);
    for (i = 3; i >= 0; i--) {
        if (mr_arr[i])  ibv_dereg_mr(mr_arr[i]);
        if (buf_arr[i]) free(buf_arr[i]);
    }
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  Done\n");
    return 0;
}
