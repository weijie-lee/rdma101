/**
 * RDMA Hello World - Minimal Complete Send/Recv Example
 *
 * This is a single-machine Loopback program: creates two QPs within the
 * same process, one sends "Hello RDMA!", the other receives and prints it.
 *
 * Features:
 *   - Automatically detects IB/RoCE mode
 *   - Uses common/rdma_utils.h utility library
 *   - Every line has English comments
 *   - Can be directly copied as a starting template for new projects
 *
 * Build:
 *   gcc -o hello_rdma hello_rdma.c \
 *       -I../common ../common/librdma_utils.a -libverbs
 *
 * Run:
 *   ./hello_rdma
 */

#include <stdio.h>          /* printf, fprintf */
#include <stdlib.h>         /* malloc, free, exit */
#include <string.h>         /* memset, strcpy, strlen */
#include <unistd.h>         /* close */
#include <infiniband/verbs.h>   /* All libibverbs APIs */
#include "rdma_utils.h"     /* Common utility library */

/* ========== Constant Definitions ========== */
#define BUF_SIZE    1024    /* Send/recv buffer size (bytes) */
#define CQ_SIZE     16      /* Completion Queue capacity */
#define PORT_NUM    1       /* Port number to use */
#define GID_INDEX   0       /* GID index (RoCE v2 may need 1 or 3) */
#define MSG         "Hello RDMA!"   /* Message to send */

/* ========== Main Function ========== */
int main(int argc, char *argv[])
{
    (void)argc;     /* Unused */
    (void)argv;     /* Unused */

    /* ---- Initialize all resource pointers to NULL (for unified cleanup) ---- */
    struct ibv_device **dev_list    = NULL;  /* Device list */
    struct ibv_context *ctx         = NULL;  /* Device context */
    struct ibv_pd      *pd          = NULL;  /* Protection Domain */
    struct ibv_cq      *cq          = NULL;  /* Completion Queue (shared by both QPs) */
    struct ibv_qp      *sender_qp   = NULL;  /* Sender QP */
    struct ibv_qp      *recver_qp   = NULL;  /* Receiver QP */
    struct ibv_mr      *send_mr     = NULL;  /* Send buffer MR */
    struct ibv_mr      *recv_mr     = NULL;  /* Recv buffer MR */
    char               *send_buf    = NULL;  /* Send buffer */
    char               *recv_buf    = NULL;  /* Recv buffer */
    int                 ret         = 0;     /* Return value */
    int                 num_devices = 0;     /* Number of devices */

    printf("=== RDMA Hello World (Loopback Send/Recv) ===\n\n");

    /* ================================================================
     * Step 1: Open RDMA device
     * ================================================================ */
    printf("[Step 1] Opening RDMA device...\n");

    dev_list = ibv_get_device_list(&num_devices);   /* Get all RDMA devices */
    CHECK_NULL(dev_list, "Failed to get device list");  /* Check success */

    if (num_devices == 0) {                          /* No devices */
        fprintf(stderr, "[Error] No RDMA devices found, please check the driver\n");
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);              /* Open first device */
    CHECK_NULL(ctx, "Failed to open device");

    printf("  Device name: %s\n", ibv_get_device_name(dev_list[0]));

    /* ---- Auto-detect transport layer type (IB or RoCE) ---- */
    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);  /* Is it RoCE? */
    printf("  Transport: %s\n", transport_str(transport));

    /* ================================================================
     * Step 2: Allocate Protection Domain (PD)
     * ================================================================ */
    printf("[Step 2] Allocating Protection Domain (PD)...\n");

    pd = ibv_alloc_pd(ctx);                          /* PD is a container for all resources */
    CHECK_NULL(pd, "Failed to allocate PD");

    printf("  PD allocated successfully\n");

    /* ================================================================
     * Step 3: Create Completion Queue (CQ)
     * ================================================================ */
    printf("[Step 3] Creating Completion Queue (CQ)...\n");

    cq = ibv_create_cq(ctx, CQ_SIZE, NULL, NULL, 0); /* Both QPs share one CQ */
    CHECK_NULL(cq, "Failed to create CQ");

    printf("  CQ created successfully (capacity=%d)\n", CQ_SIZE);

    /* ================================================================
     * Step 4: Create two QPs (sender and receiver)
     * ================================================================ */
    printf("[Step 4] Creating QPs...\n");

    struct ibv_qp_init_attr qp_init_attr;            /* QP initialization attributes */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));  /* Zero out */
    qp_init_attr.send_cq = cq;                       /* Send Completion Queue */
    qp_init_attr.recv_cq = cq;                       /* Recv Completion Queue */
    qp_init_attr.qp_type = IBV_QPT_RC;               /* Reliable Connection (RC) type */
    qp_init_attr.cap.max_send_wr  = 4;               /* Max send WR count */
    qp_init_attr.cap.max_recv_wr  = 4;               /* Max recv WR count */
    qp_init_attr.cap.max_send_sge = 1;               /* Max SGE per WR */
    qp_init_attr.cap.max_recv_sge = 1;               /* Max SGE per WR */

    sender_qp = ibv_create_qp(pd, &qp_init_attr);    /* Create sender QP */
    CHECK_NULL(sender_qp, "Failed to create sender QP");

    recver_qp = ibv_create_qp(pd, &qp_init_attr);    /* Create receiver QP */
    CHECK_NULL(recver_qp, "Failed to create receiver QP");

    printf("  Sender QP number: %u\n", sender_qp->qp_num);
    printf("  Receiver QP number: %u\n", recver_qp->qp_num);

    /* ================================================================
     * Step 5: Register Memory Regions (MR)
     * ================================================================ */
    printf("[Step 5] Registering Memory Regions (MR)...\n");

    send_buf = (char *)malloc(BUF_SIZE);              /* Allocate send buffer */
    CHECK_NULL(send_buf, "Failed to allocate send buffer");
    memset(send_buf, 0, BUF_SIZE);                    /* Zero out */
    strcpy(send_buf, MSG);                            /* Write message to send */

    recv_buf = (char *)malloc(BUF_SIZE);              /* Allocate recv buffer */
    CHECK_NULL(recv_buf, "Failed to allocate recv buffer");
    memset(recv_buf, 0, BUF_SIZE);                    /* Zero out */

    /* Register send MR (local write permission is sufficient) */
    send_mr = ibv_reg_mr(pd, send_buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(send_mr, "Failed to register send MR");

    /* Register recv MR (local write permission, because NIC needs to write data) */
    recv_mr = ibv_reg_mr(pd, recv_buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(recv_mr, "Failed to register recv MR");

    printf("  Send MR: lkey=0x%x\n", send_mr->lkey);
    printf("  Recv MR: lkey=0x%x\n", recv_mr->lkey);

    /* ================================================================
     * Step 6: QP State Transitions (RESET -> INIT -> RTR -> RTS)
     * ================================================================ */
    printf("[Step 6] QP state transitions...\n");

    /* ---- Fill endpoint info (Loopback: each QP's remote is the other QP) ---- */
    struct rdma_endpoint sender_ep, recver_ep;

    /* Fill sender endpoint info */
    ret = fill_local_endpoint(ctx, sender_qp, PORT_NUM, GID_INDEX, &sender_ep);
    CHECK_ERRNO(ret, "Failed to fill sender endpoint");

    /* Fill receiver endpoint info */
    ret = fill_local_endpoint(ctx, recver_qp, PORT_NUM, GID_INDEX, &recver_ep);
    CHECK_ERRNO(ret, "Failed to fill receiver endpoint");

    /* ---- Sender QP connects to receiver (remote = recver) ---- */
    int access = IBV_ACCESS_LOCAL_WRITE;              /* Send/Recv doesn't need REMOTE permissions */
    ret = qp_full_connect(sender_qp, &recver_ep, PORT_NUM, is_roce, access);
    CHECK_ERRNO(ret, "Sender QP state transition failed");

    /* ---- Receiver QP connects to sender (remote = sender) ---- */
    ret = qp_full_connect(recver_qp, &sender_ep, PORT_NUM, is_roce, access);
    CHECK_ERRNO(ret, "Receiver QP state transition failed");

    printf("  Both QPs connected to each other (RESET -> INIT -> RTR -> RTS)\n");

    /* ================================================================
     * Step 7: Post Recv (must be done before Send!)
     * ================================================================ */
    printf("[Step 7] Posting Recv on receiver...\n");

    struct ibv_sge recv_sge;                          /* Recv SGE */
    memset(&recv_sge, 0, sizeof(recv_sge));
    recv_sge.addr   = (uint64_t)recv_buf;             /* Recv buffer address */
    recv_sge.length = BUF_SIZE;                       /* Buffer size */
    recv_sge.lkey   = recv_mr->lkey;                  /* MR's lkey */

    struct ibv_recv_wr recv_wr, *bad_recv_wr;         /* Recv WR */
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;                              /* Custom ID: 1 = receive */
    recv_wr.sg_list = &recv_sge;                      /* Points to SGE */
    recv_wr.num_sge = 1;                              /* 1 SGE */
    recv_wr.next    = NULL;                           /* No next WR */

    ret = ibv_post_recv(recver_qp, &recv_wr, &bad_recv_wr); /* Submit recv request */
    CHECK_ERRNO(ret, "Post Recv failed");

    printf("  Recv request submitted (waiting for data)\n");

    /* ================================================================
     * Step 8: Post Send (send "Hello RDMA!")
     * ================================================================ */
    printf("[Step 8] Posting Send on sender...\n");

    struct ibv_sge send_sge;                          /* Send SGE */
    memset(&send_sge, 0, sizeof(send_sge));
    send_sge.addr   = (uint64_t)send_buf;             /* Send buffer address */
    send_sge.length = strlen(MSG) + 1;                /* Data length (including \0) */
    send_sge.lkey   = send_mr->lkey;                  /* MR's lkey */

    struct ibv_send_wr send_wr, *bad_send_wr;         /* Send WR */
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;                           /* Custom ID: 2 = send */
    send_wr.sg_list    = &send_sge;                   /* Points to SGE */
    send_wr.num_sge    = 1;                           /* 1 SGE */
    send_wr.opcode     = IBV_WR_SEND;                 /* Operation type: Send */
    send_wr.send_flags = IBV_SEND_SIGNALED;           /* Generate CQE on completion */
    send_wr.next       = NULL;                        /* No next WR */

    ret = ibv_post_send(sender_qp, &send_wr, &bad_send_wr); /* Submit send request */
    CHECK_ERRNO(ret, "Post Send failed");

    printf("  Sent: \"%s\" (%zu bytes)\n", MSG, strlen(MSG) + 1);

    /* ================================================================
     * Step 9: Poll CQ for completions
     * ================================================================ */
    printf("[Step 9] Polling CQ for completion events...\n");

    /* ---- Wait for send completion ---- */
    struct ibv_wc wc;                                 /* Completion event */
    ret = poll_cq_blocking(cq, &wc);                  /* Blocking poll */
    CHECK_ERRNO(ret, "Polling send completion failed");

    if (wc.status != IBV_WC_SUCCESS) {                /* Check status */
        fprintf(stderr, "[Error] Send failed: %s\n", ibv_wc_status_str(wc.status));
        goto cleanup;
    }
    printf("  Send complete ✓ (wr_id=%lu)\n", wc.wr_id);
    print_wc_detail(&wc);                             /* Print completion event details */

    /* ---- Wait for recv completion ---- */
    ret = poll_cq_blocking(cq, &wc);                  /* Blocking poll */
    CHECK_ERRNO(ret, "Polling recv completion failed");

    if (wc.status != IBV_WC_SUCCESS) {                /* Check status */
        fprintf(stderr, "[Error] Recv failed: %s\n", ibv_wc_status_str(wc.status));
        goto cleanup;
    }
    printf("  Recv complete ✓ (wr_id=%lu, byte_len=%u)\n", wc.wr_id, wc.byte_len);
    print_wc_detail(&wc);                             /* Print completion event details */

    /* ================================================================
     * Step 10: Verify received data
     * ================================================================ */
    printf("\n[Step 10] Verifying received data...\n");
    printf("  Received: \"%s\"\n", recv_buf);          /* Print received message */

    if (strcmp(send_buf, recv_buf) == 0) {             /* Compare send and recv data */
        printf("\n  ★ Success! RDMA Send/Recv verification passed ★\n");
    } else {
        fprintf(stderr, "\n  ✗ Failed! Send and recv data mismatch\n");
    }

    printf("\n=== Program finished ===\n");

    /* ================================================================
     * Cleanup: Release all resources in reverse order
     * ================================================================ */
cleanup:
    printf("\n[Cleanup] Releasing resources...\n");

    if (sender_qp) ibv_destroy_qp(sender_qp);        /* Destroy sender QP */
    if (recver_qp) ibv_destroy_qp(recver_qp);        /* Destroy receiver QP */
    if (send_mr)   ibv_dereg_mr(send_mr);             /* Deregister send MR */
    if (recv_mr)   ibv_dereg_mr(recv_mr);             /* Deregister recv MR */
    if (cq)        ibv_destroy_cq(cq);                /* Destroy CQ */
    if (pd)        ibv_dealloc_pd(pd);                /* Deallocate PD */
    if (ctx)       ibv_close_device(ctx);             /* Close device */
    if (dev_list)  ibv_free_device_list(dev_list);    /* Free device list */
    if (send_buf)  free(send_buf);                    /* Free send buffer */
    if (recv_buf)  free(recv_buf);                    /* Free recv buffer */

    printf("  Resources released\n");
    return ret;
}
