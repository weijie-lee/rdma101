/**
 * RDMA Initialization Example
 * Demonstrates basic device discovery, opening, PD/CQ/QP creation
 */

#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main(int argc, char *argv[])
{
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_qp_init_attr qp_init_attr;
    int num_devices;
    int i;

    /* 1. Get device list */
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        perror("Failed to get device list");
        return 1;
    }

    if (num_devices == 0) {
        printf("No RDMA devices found\n");
        return 1;
    }

    printf("Found %d RDMA device(s)\n", num_devices);
    for (i = 0; i < num_devices; i++) {
        printf("  %d: %s\n", i, ibv_get_device_name(device_list[i]));
    }

    /* 2. Select the first device and open it */
    device = device_list[0];
    context = ibv_open_device(device);
    if (!context) {
        perror("Failed to open device");
        return 1;
    }
    printf("Opened device: %s\n", ibv_get_device_name(device));

    /* 3. Allocate Protection Domain */
    pd = ibv_alloc_pd(context);
    if (!pd) {
        perror("Failed to allocate PD");
        return 1;
    }
    printf("Allocated PD\n");

    /* 4. Create Completion Queue */
    cq = ibv_create_cq(context, 128, NULL, NULL, 0);
    if (!cq) {
        perror("Failed to create CQ");
        return 1;
    }
    printf("Created CQ with %d entries\n", 128);

    /* 5. Create Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 128;
    qp_init_attr.cap.max_recv_wr = 128;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        perror("Failed to create QP");
        return 1;
    }
    printf("Created QP with num=%u\n", qp->qp_num);

    /* 6. Query QP state */
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    if (ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr) == 0) {
        printf("QP state: %d (RESET=%d, INIT=%d, RTR=%d, RTS=%d)\n",
               attr.qp_state, IBV_QPS_RESET, IBV_QPS_INIT,
               IBV_QPS_RTR, IBV_QPS_RTS);
    }

    /* 6. Register Memory (Memory Region) - allow RDMA device to access this memory */
    char *mr_buf = malloc(4096);
    if (!mr_buf) {
        perror("Failed to allocate memory for MR");
        return 1;
    }
    memset(mr_buf, 0, 4096);

    struct ibv_mr *mr = ibv_reg_mr(pd, mr_buf, 4096,
                                   IBV_ACCESS_LOCAL_WRITE |
                                   IBV_ACCESS_REMOTE_READ |
                                   IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("Failed to register MR");
        free(mr_buf);
        return 1;
    }
    printf("Registered MR: lkey=0x%x, rkey=0x%x, addr=%p, len=%zu\n",
           mr->lkey, mr->rkey, mr->addr, mr->length);

    /* 7. Cleanup resources */
    if (ibv_dereg_mr(mr)) {
        perror("Failed to deregister MR");
    }
    free(mr_buf);
    if (ibv_destroy_qp(qp)) {
        perror("Failed to destroy QP");
    }
    if (ibv_destroy_cq(cq)) {
        perror("Failed to destroy CQ");
    }
    if (ibv_dealloc_pd(pd)) {
        perror("Failed to deallocate PD");
    }
    if (ibv_close_device(context)) {
        perror("Failed to close device");
    }
    ibv_free_device_list(device_list);

    printf("\nAll resources cleaned up\n");
    return 0;
}
