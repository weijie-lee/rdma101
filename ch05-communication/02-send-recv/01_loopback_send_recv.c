/**
 * RDMA Send/Recv Complete Example - Loopback Mode
 *
 * This is a Loopback example, simulating client-server communication on the same machine.
 * In actual use, two machines are needed, exchanging QP info via TCP.
 *
 * Enhanced features:
 *   - IB/RoCE auto-detection (detect_transport)
 *   - Multi-SGE send example (using 2 SGEs to send one message)
 *   - Detailed WC field printing (print_wc_detail)
 *   - max_send_sge/max_recv_sge increased to 4
 *
 * Build: gcc -Wall -O2 -g -o 01_loopback_send_recv 01_loopback_send_recv.c \
 *        -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "../../common/rdma_utils.h"

#define BUFFER_SIZE 1024

/* RDMA resource structure */
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    char *send_buf;
    char *recv_buf;
    int is_roce;        /* IB/RoCE auto-detection result */
    uint8_t port;
};

/* Initialize RDMA resources */
int init_rdma_resources(struct rdma_resources *res)
{
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_qp_init_attr qp_init_attr;
    int num;

    /* 1. Get device */
    device_list = ibv_get_device_list(&num);
    if (!device_list || num == 0) {
        fprintf(stderr, "No RDMA devices\n");
        return -1;
    }
    device = device_list[0];

    /* 2. Open device */
    res->context = ibv_open_device(device);
    if (!res->context) {
        perror("Failed to open device");
        return -1;
    }

    /* 3. Detect transport layer type */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->context, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("Transport layer type: %s\n", transport_str(transport));

    /* 4. Allocate PD */
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        perror("Failed to allocate PD");
        return -1;
    }

    /* 5. Create CQ */
    res->cq = ibv_create_cq(res->context, 256, NULL, NULL, 0);
    if (!res->cq) {
        perror("Failed to create CQ");
        return -1;
    }

    /* 6. Create QP (max_send_sge=4, max_recv_sge=4 to support multi-SGE) */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 128;
    qp_init_attr.cap.max_recv_wr = 128;
    qp_init_attr.cap.max_send_sge = 4;  /* Increased to 4, supports multi-SGE send */
    qp_init_attr.cap.max_recv_sge = 4;  /* Increased to 4, supports multi-SGE recv */

    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        perror("Failed to create QP");
        return -1;
    }

    /* 7. Allocate and register memory */
    res->send_buf = malloc(BUFFER_SIZE);
    res->recv_buf = malloc(BUFFER_SIZE);

    res->send_mr = ibv_reg_mr(res->pd, res->send_buf, BUFFER_SIZE,
                               IBV_ACCESS_LOCAL_WRITE);
    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, BUFFER_SIZE,
                               IBV_ACCESS_LOCAL_WRITE);

    ibv_free_device_list(device_list);
    return 0;
}

/* Send data - single SGE version */
int post_send(struct rdma_resources *res, char *data, int len)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)data,
        .length = len,
        .lkey = res->send_mr->lkey,
    };

    struct ibv_send_wr wr = {
        .wr_id = 1,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };

    struct ibv_send_wr *bad_wr;
    return ibv_post_send(res->qp, &wr, &bad_wr);
}

/**
 * Multi-SGE send: uses 2 SGEs to split the message into "prefix" and "body" segments
 *
 * The NIC will automatically gather the two data segments into a single RDMA message.
 * The receiver gets the concatenated complete message in one contiguous buffer.
 */
int post_send_multi_sge(struct rdma_resources *res,
                        const char *prefix, int prefix_len,
                        const char *body, int body_len)
{
    /* Place prefix and body at different positions in send_buf */
    memcpy(res->send_buf, prefix, prefix_len);
    memcpy(res->send_buf + 512, body, body_len);  /* Offset 512 for body */

    /* 2 SGEs: pointing to prefix and body respectively */
    struct ibv_sge sges[2];
    sges[0].addr   = (uint64_t)res->send_buf;
    sges[0].length = prefix_len;
    sges[0].lkey   = res->send_mr->lkey;

    sges[1].addr   = (uint64_t)(res->send_buf + 512);
    sges[1].length = body_len;
    sges[1].lkey   = res->send_mr->lkey;

    printf("  Multi-SGE send:\n");
    printf("    SGE[0]: addr=%p, length=%d (prefix)\n",
           (void *)sges[0].addr, sges[0].length);
    printf("    SGE[1]: addr=%p, length=%d (body)\n",
           (void *)sges[1].addr, sges[1].length);

    struct ibv_send_wr wr = {
        .wr_id = 2,
        .sg_list = sges,
        .num_sge = 2,       /* 2 SGEs! */
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };

    struct ibv_send_wr *bad_wr;
    return ibv_post_send(res->qp, &wr, &bad_wr);
}

/* Receive data */
int post_recv(struct rdma_resources *res)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)res->recv_buf,
        .length = BUFFER_SIZE,
        .lkey = res->recv_mr->lkey,
    };

    struct ibv_recv_wr wr = {
        .wr_id = 100,
        .sg_list = &sge,
        .num_sge = 1,
    };

    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(res->qp, &wr, &bad_wr);
}

int main(int argc, char *argv[])
{
    struct rdma_resources res = {0};
    const char *message = "Hello RDMA!";

    (void)argc;
    (void)argv;

    printf("=== RDMA Send/Recv Loopback Example (Enhanced) ===\n\n");

    /* Initialize resources */
    if (init_rdma_resources(&res) != 0) {
        return 1;
    }
    printf("Resource initialization complete\n");
    printf("  QP number: %u\n", res.qp->qp_num);
    printf("  max_send_sge: 4, max_recv_sge: 4\n");

    /* Configure QP state machine - using IB/RoCE auto-detection */
    printf("\nConfiguring QP state machine (Loopback)...\n");

    /* Fill local endpoint info */
    struct rdma_endpoint self_ep;
    memset(&self_ep, 0, sizeof(self_ep));
    if (fill_local_endpoint(res.context, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &self_ep) != 0) {
        fprintf(stderr, "Failed to fill endpoint info\n");
        return 1;
    }
    printf("  LID: %u\n", self_ep.lid);
    if (res.is_roce) {
        char gid_str[46];
        gid_to_str(&self_ep.gid, gid_str, sizeof(gid_str));
        printf("  GID: %s\n", gid_str);
    }

    /* QP full state transition: RESET -> INIT -> RTR -> RTS */
    int access = IBV_ACCESS_LOCAL_WRITE;
    if (qp_full_connect(res.qp, &self_ep, res.port, res.is_roce, access) != 0) {
        fprintf(stderr, "QP connection failed\n");
        return 1;
    }
    printf("  QP state: RTS (Ready to Send)\n");

    /* ========== Test 1: Single SGE Send/Recv ========== */
    printf("\n========================================\n");
    printf("  Test 1: Single SGE Send/Recv\n");
    printf("========================================\n");

    /* Step 1: Receiver posts recv first */
    printf("\n[Step 1] Receiver post_recv\n");
    memset(res.recv_buf, 0, BUFFER_SIZE);
    post_recv(&res);

    /* Step 2: Sender posts send */
    printf("[Step 2] Sender post_send: \"%s\"\n", message);
    memcpy(res.send_buf, message, strlen(message) + 1);
    post_send(&res, res.send_buf, strlen(message) + 1);

    /* Step 3: Wait for send completion, print detailed WC */
    printf("[Step 3] Sender waiting for WC\n");
    struct ibv_wc wc;
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  Send WC details:\n");
        print_wc_detail(&wc);
    }

    /* Step 4: Wait for recv completion, print detailed WC */
    printf("[Step 4] Receiver waiting for WC\n");
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  Recv WC details:\n");
        print_wc_detail(&wc);
    }

    /* Check result */
    printf("\n[Result] Received message: \"%s\"\n", res.recv_buf);

    /* ========== Test 2: Multi-SGE Send/Recv ========== */
    printf("\n========================================\n");
    printf("  Test 2: Multi-SGE Send/Recv (2 SGEs)\n");
    printf("========================================\n");

    const char *prefix = "[MSG] ";
    const char *body   = "This message is assembled from 2 SGEs!";

    /* Receiver posts recv first */
    printf("\n[Step 1] Receiver post_recv\n");
    memset(res.recv_buf, 0, BUFFER_SIZE);
    post_recv(&res);

    /* Multi-SGE send */
    printf("[Step 2] Multi-SGE send:\n");
    printf("  Prefix: \"%s\" (%zu bytes)\n", prefix, strlen(prefix));
    printf("  Body: \"%s\" (%zu bytes)\n", body, strlen(body) + 1);
    post_send_multi_sge(&res, prefix, strlen(prefix),
                        body, strlen(body) + 1);

    /* Wait for send completion */
    printf("[Step 3] Sender waiting for WC\n");
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  Send WC details:\n");
        print_wc_detail(&wc);
    }

    /* Wait for recv completion */
    printf("[Step 4] Receiver waiting for WC\n");
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  Recv WC details:\n");
        print_wc_detail(&wc);
        printf("  Received bytes: %u (prefix %zu + body %zu = %zu)\n",
               wc.byte_len, strlen(prefix), strlen(body) + 1,
               strlen(prefix) + strlen(body) + 1);
    }

    /* Check concatenation result */
    printf("\n[Result] Received message: \"%s\"\n", res.recv_buf);

    /* Verify */
    char expected[BUFFER_SIZE];
    snprintf(expected, sizeof(expected), "%s%s", prefix, body);
    if (strcmp(res.recv_buf, expected) == 0) {
        printf("[Verify] Multi-SGE concatenation correct!\n");
    } else {
        printf("[Verify] Concatenation mismatch! Expected: \"%s\"\n", expected);
    }

    /* Cleanup */
    if (res.send_mr) ibv_dereg_mr(res.send_mr);
    if (res.recv_mr) ibv_dereg_mr(res.recv_mr);
    if (res.send_buf) free(res.send_buf);
    if (res.recv_buf) free(res.recv_buf);
    ibv_destroy_qp(res.qp);
    ibv_destroy_cq(res.cq);
    ibv_dealloc_pd(res.pd);
    ibv_close_device(res.context);

    printf("\nProgram finished\n");
    return 0;
}
