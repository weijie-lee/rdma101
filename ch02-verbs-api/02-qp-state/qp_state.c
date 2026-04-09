/**
 * QP 状态转换示例
 * 演示如何将 QP 从 RESET -> INIT -> RTR -> RTS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define PORT_NUM 1

/* 资源结构 */
struct rdma_dev {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
};

/* 获取本地QP信息 */
void print_local_qp_info(struct rdma_dev *dev)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    
    ibv_query_qp(dev->qp, &attr, IBV_QP_STATE, &init_attr);
    
    printf("=== Local QP Info ===\n");
    printf("QP Number: %u\n", dev->qp->qp_num);
    printf("State: %d\n", attr.qp_state);
    printf("Port: %d\n", attr.port_num);
    printf("PKey Index: %d\n", attr.pkey_index);
    printf("======================\n");
}

/* 获取端口信息 */
void print_port_info(struct ibv_context *context, uint8_t port)
{
    struct ibv_port_attr attr;
    
    if (ibv_query_port(context, port, &attr) == 0) {
        printf("=== Port %d Info ===\n", port);
        printf("Link Layer: %s\n", 
               attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "IB" : "Ethernet");
        printf("State: %s\n", 
               attr.state == IBV_PORT_ACTIVE ? "ACTIVE" : "DOWN");
        printf("LID: %u\n", attr.lid);
        printf("SM LID: %u\n", attr.sm_lid);
        printf("===================\n");
    }
}

/* RESET -> INIT */
int qp_reset_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = PORT_NUM,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE,
    };
    
    int ret = ibv_modify_qp(qp, &attr,
                             IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                             IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        perror("QP RESET->INIT failed");
    }
    return ret;
}

/* INIT -> RTR */
int qp_init_to_rtr(struct ibv_qp *qp, uint32_t remote_qp_num, 
                   uint16_t remote_lid)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_256,
        .dest_qp_num = remote_qp_num,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .dlid = remote_lid,
            .sl = 0,
            .port_num = PORT_NUM,
        },
    };
    
    int ret = ibv_modify_qp(qp, &attr, 
                             IBV_QP_STATE | IBV_QP_PATH_MTU |
                             IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                             IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                             IBV_QP_AV);
    if (ret) {
        perror("QP INIT->RTR failed");
    }
    return ret;
}

/* RTR -> RTS */
int qp_rtr_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = 0,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .max_rd_atomic = 1,
    };
    
    int ret = ibv_modify_qp(qp, &attr,
                             IBV_QP_STATE | IBV_QP_SQ_PSN |
                             IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                             IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        perror("QP RTR->RTS failed");
    }
    return ret;
}

int main(int argc, char *argv[])
{
    struct rdma_dev dev = {0};
    struct ibv_device **list;
    int num;
    
    printf("QP State Transition Example\n");
    printf("============================\n\n");
    
    /* 获取设备 */
    list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "No devices found\n");
        return 1;
    }
    
    /* 打开设备 */
    dev.context = ibv_open_device(list[0]);
    if (!dev.context) {
        perror("open device");
        return 1;
    }
    
    /* 打印端口信息 */
    print_port_info(dev.context, PORT_NUM);
    
    /* 分配PD */
    dev.pd = ibv_alloc_pd(dev.context);
    if (!dev.pd) {
        perror("alloc pd");
        return 1;
    }
    
    /* 创建CQ */
    dev.cq = ibv_create_cq(dev.context, 128, NULL, NULL, 0);
    if (!dev.cq) {
        perror("create cq");
        return 1;
    }
    
    /* 创建QP */
    struct ibv_qp_init_attr init_attr = {
        .send_cq = dev.cq,
        .recv_cq = dev.cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 128,
            .max_recv_wr = 128,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    dev.qp = ibv_create_qp(dev.pd, &init_attr);
    if (!dev.qp) {
        perror("create qp");
        return 1;
    }
    
    /* 打印初始QP状态 */
    print_local_qp_info(&dev);

    /* 执行状态转换: RESET -> INIT */
    printf("Step 1: RESET -> INIT\n");
    if (qp_reset_to_init(dev.qp)) {
        return 1;
    }
    print_local_qp_info(&dev);

    /*
     * 演示完整状态转换：INIT -> RTR -> RTS
     *
     * 对于 loopback 测试（单机验证），可以用本机 QP num 和 LID
     * 作为"远端"参数，将 QP 转到 RTR/RTS，完整演示状态机。
     *
     * 实际生产中，需要通过 TCP socket 或 RDMA CM 与对端交换：
     *   - remote_qp_num：对端 QP 编号
     *   - remote_lid：对端端口 LID
     */

    /* 查询本机 LID（loopback 演示用） */
    struct ibv_port_attr port_attr;
    if (ibv_query_port(dev.context, PORT_NUM, &port_attr) != 0) {
        perror("query port for loopback");
        return 1;
    }
    uint16_t local_lid = port_attr.lid;
    uint32_t local_qp_num = dev.qp->qp_num;

    printf("\nStep 2: INIT -> RTR (loopback: remote_qp=%u, remote_lid=%u)\n",
           local_qp_num, local_lid);
    if (qp_init_to_rtr(dev.qp, local_qp_num, local_lid)) {
        return 1;
    }
    print_local_qp_info(&dev);

    printf("Step 3: RTR -> RTS\n");
    if (qp_rtr_to_rts(dev.qp)) {
        return 1;
    }
    print_local_qp_info(&dev);

    printf("\n=== QP state machine complete: RESET -> INIT -> RTR -> RTS ===\n");
    printf("QP is now ready for data transfer.\n\n");

    /* 清理 */
    ibv_destroy_qp(dev.qp);
    ibv_destroy_cq(dev.cq);
    ibv_dealloc_pd(dev.pd);
    ibv_close_device(dev.context);
    ibv_free_device_list(list);
    
    return 0;
}
