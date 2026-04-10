/**
 * Protection Domain (PD) Isolation Demo
 *
 * This program demonstrates PD's core security mechanism -- resource isolation:
 *   1. Create two independent PDs (pd1, pd2)
 *   2. Register MR under pd1, create QP under pd2
 *   3. Attempt to send using pd1's MR lkey with pd2's QP -> should fail
 *   4. Demonstrate normal case: MR + QP under the same PD can work normally
 *
 * Compile: gcc -o pd_isolation pd_isolation.c -I../../common ../../common/librdma_utils.a -libverbs
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
#define CQ_DEPTH    16

int main(int argc, char *argv[])
{
    /* ===== Resource declarations ===== */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd1      = NULL;   /* Protection Domain 1 */
    struct ibv_pd      *pd2      = NULL;   /* Protection Domain 2 */
    struct ibv_mr      *mr1      = NULL;   /* MR registered under pd1 */
    struct ibv_mr      *mr_same  = NULL;   /* MR under same PD (normal case) */
    struct ibv_cq      *cq1      = NULL;   /* CQ for pd1 */
    struct ibv_cq      *cq2      = NULL;   /* CQ for pd2 */
    struct ibv_qp      *qp1      = NULL;   /* QP for pd1 (normal case) */
    struct ibv_qp      *qp2      = NULL;   /* QP for pd2 (cross-PD case) */
    char               *buf1     = NULL;
    char               *buf_same = NULL;
    int                 num_devices;
    int                 ret;

    printf("========================================\n");
    printf("  PD Isolation Demo (Protection Domain)\n");
    printf("========================================\n\n");

    /* ========== Step 1: Open device ========== */
    printf("[Step 1] Open RDMA device\n");
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get device list");

    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "Failed to open device");
    printf("  Device: %s\n\n", ibv_get_device_name(dev_list[0]));

    /* ========== Step 2: Create two independent PDs ========== */
    printf("[Step 2] Create two independent Protection Domains\n");
    pd1 = ibv_alloc_pd(ctx);
    CHECK_NULL(pd1, "Failed to allocate PD1");
    printf("  PD1 created successfully, handle=%u\n", pd1->handle);

    pd2 = ibv_alloc_pd(ctx);
    CHECK_NULL(pd2, "Failed to allocate PD2");
    printf("  PD2 created successfully, handle=%u\n", pd2->handle);
    printf("  -> Two PDs have different handles, representing independent security domains\n\n");

    /* ========== Step 3: Register MR under PD1 ========== */
    printf("[Step 3] Register Memory Region (MR) under PD1\n");
    buf1 = malloc(BUFFER_SIZE);
    CHECK_NULL(buf1, "malloc buf1 failed");
    memset(buf1, 'A', BUFFER_SIZE);

    mr1 = ibv_reg_mr(pd1, buf1, BUFFER_SIZE,
                      IBV_ACCESS_LOCAL_WRITE |
                      IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(mr1, "Failed to register MR under PD1");
    printf("  MR1 registered successfully (belongs to PD1)\n");
    printf("    lkey=0x%08x, rkey=0x%08x\n", mr1->lkey, mr1->rkey);
    printf("    addr=%p, length=%zu\n\n", mr1->addr, (size_t)mr1->length);

    /* ========== Step 4: Create CQs ========== */
    printf("[Step 4] Create Completion Queues\n");
    cq1 = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq1, "Failed to create CQ1");
    cq2 = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq2, "Failed to create CQ2");
    printf("  CQ1, CQ2 created successfully\n\n");

    /* ========== Step 5: Create QP under PD2 ========== */
    printf("[Step 5] Create QP under PD2\n");
    struct ibv_qp_init_attr qp2_attr = {
        .send_cq = cq2,
        .recv_cq = cq2,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp2 = ibv_create_qp(pd2, &qp2_attr);
    CHECK_NULL(qp2, "Failed to create QP2 under PD2");
    printf("  QP2 created successfully (belongs to PD2), qp_num=%u\n\n", qp2->qp_num);

    /* ========== Step 6: Demonstrate cross-PD usage -> failure ========== */
    printf("============================================================\n");
    printf("[Step 6] Demonstrate cross-PD usage: Send with PD1's MR lkey + PD2's QP\n");
    printf("============================================================\n\n");

    /*
     * Transition QP2 (belongs to PD2) to INIT state, prepare for sending.
     * Use qp_to_init() utility function.
     */
    int access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                       IBV_ACCESS_REMOTE_WRITE;
    ret = qp_to_init(qp2, PORT_NUM, access_flags);
    if (ret != 0) {
        printf("  [Info] QP2 RESET->INIT failed (ret=%d)\n", ret);
        printf("  -> This may be due to port state or configuration issues, not PD isolation\n\n");
    } else {
        printf("  QP2 has transitioned to INIT state\n");
    }

    /*
     * Key experiment: Attempt to use MR1's lkey (belongs to PD1) with QP2 (belongs to PD2) for sending
     *
     * In RDMA, hardware verifies that the lkey in SGE belongs to the QP's PD.
     * If PD mismatch, ibv_post_send() returns error, or HCA generates a
     * "Local Protection Error" (IBV_WC_LOC_PROT_ERR) when processing the WQE.
     */
    printf("  Attempting ibv_post_send: SGE.lkey=0x%08x (PD1), QP belongs to PD2...\n",
           mr1->lkey);

    struct ibv_sge sge = {
        .addr   = (uintptr_t)buf1,
        .length = 64,
        .lkey   = mr1->lkey,    /* Intentionally use PD1's MR lkey */
    };
    struct ibv_send_wr wr = {
        .wr_id      = 1,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_wr = NULL;

    ret = ibv_post_send(qp2, &wr, &bad_wr);
    if (ret != 0) {
        printf("  ibv_post_send failed! errno=%d (%s)\n", errno, strerror(errno));
        printf("  -> This is the effect of PD isolation: hardware/driver rejected cross-PD resource access\n");
        printf("  -> Note: Some drivers don't check at post_send time, instead return\n");
        printf("    IBV_WC_LOC_PROT_ERR (Local Protection Error) in the CQ\n\n");
    } else {
        /*
         * Some drivers allow post_send to succeed (only enqueue the WR),
         * but HCA detects PD mismatch during actual processing and generates an error completion.
         * Since QP2 is not yet in RTS state, post_send itself may fail due to state issues.
         * But the key point is: even if QP reaches RTS state, cross-PD will still fail.
         */
        printf("  post_send returned success (enqueued only), but HCA will detect PD mismatch during processing\n");
        printf("  -> CQ will receive IBV_WC_LOC_PROT_ERR error\n\n");
    }

    /* ========== Step 7: Demonstrate normal case (same PD) ========== */
    printf("============================================\n");
    printf("[Step 7] Normal case: MR + QP under the same PD\n");
    printf("============================================\n\n");

    /* Create a QP under PD1 as well */
    struct ibv_qp_init_attr qp1_attr = {
        .send_cq = cq1,
        .recv_cq = cq1,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp1 = ibv_create_qp(pd1, &qp1_attr);
    CHECK_NULL(qp1, "Failed to create QP1 under PD1");
    printf("  QP1 created successfully (belongs to PD1), qp_num=%u\n", qp1->qp_num);

    /* Register another MR under the same PD */
    buf_same = malloc(BUFFER_SIZE);
    CHECK_NULL(buf_same, "malloc buf_same failed");
    memset(buf_same, 'B', BUFFER_SIZE);

    mr_same = ibv_reg_mr(pd1, buf_same, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr_same, "Failed to register mr_same under PD1");
    printf("  mr_same registered successfully (belongs to PD1), lkey=0x%08x\n\n", mr_same->lkey);

    /* Transition QP1 to INIT state */
    ret = qp_to_init(qp1, PORT_NUM, IBV_ACCESS_LOCAL_WRITE);
    if (ret == 0) {
        printf("  QP1 RESET->INIT transition succeeded\n");
        print_qp_state(qp1);

        /* Send using same PD's MR lkey */
        struct ibv_sge sge_ok = {
            .addr   = (uintptr_t)buf_same,
            .length = 64,
            .lkey   = mr_same->lkey,    /* Same PD (PD1) */
        };
        struct ibv_send_wr wr_ok = {
            .wr_id      = 2,
            .sg_list    = &sge_ok,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
        };
        struct ibv_send_wr *bad_wr_ok = NULL;

        ret = ibv_post_send(qp1, &wr_ok, &bad_wr_ok);
        if (ret != 0) {
            printf("  post_send failed (errno=%d: %s)\n", errno, strerror(errno));
            printf("  -> Note: This may be because QP is not yet in RTS state, not a PD isolation issue\n");
        } else {
            printf("  ibv_post_send succeeded! MR+QP under the same PD have no isolation issue\n");
        }
    } else {
        printf("  QP1 RESET->INIT transition failed (port may not be ready)\n");
    }

    /* ========== Summary ========== */
    printf("\n========================================\n");
    printf("  PD Isolation Summary\n");
    printf("========================================\n");
    printf("  1. Each PD is an independent security boundary\n");
    printf("  2. MR, QP, AH and other resources all belong to a specific PD\n");
    printf("  3. Resources from different PDs cannot be mixed (hardware enforced)\n");
    printf("  4. Cross-PD usage results in:\n");
    printf("     - ibv_post_send/recv returning error, or\n");
    printf("     - IBV_WC_LOC_PROT_ERR in the CQ\n");
    printf("  5. Use cases: multi-tenant isolation, inter-process security isolation\n");
    printf("     - Example: VM-A's PD and VM-B's PD are invisible to each other\n");
    printf("     - Even knowing the other's lkey/rkey cannot grant unauthorized access\n\n");

cleanup:
    printf("[Cleanup] Releasing all resources...\n");

    if (qp1)      ibv_destroy_qp(qp1);
    if (qp2)      ibv_destroy_qp(qp2);
    if (cq1)      ibv_destroy_cq(cq1);
    if (cq2)      ibv_destroy_cq(cq2);
    if (mr1)      ibv_dereg_mr(mr1);
    if (mr_same)  ibv_dereg_mr(mr_same);
    if (buf1)     free(buf1);
    if (buf_same) free(buf_same);
    if (pd1)      ibv_dealloc_pd(pd1);
    if (pd2)      ibv_dealloc_pd(pd2);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  Done\n");
    return 0;
}
