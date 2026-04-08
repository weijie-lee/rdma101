/**
 * RDMA Write 示例 - 客户端推送数据到服务器
 * 
 * 流程：
 * 1. Server: 启动、创建资源、等待连接
 * 2. Client: 连接Server、交换MR信息、执行Write
 * 3. Server: 验证数据
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024
#define MSG_SIZE 256

/* RDMA 资源结构 */
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buffer;
};

/* 初始化RDMA资源 */
int init_rdma_resources(struct rdma_resources *res)
{
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_qp_init_attr qp_init_attr;
    int num_devices;

    /* 获取设备 */
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return -1;
    }
    device = device_list[0];

    /* 打开设备 */
    res->context = ibv_open_device(device);
    if (!res->context) {
        fprintf(stderr, "Failed to open device\n");
        return -1;
    }

    /* 分配PD */
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        return -1;
    }

    /* 创建CQ */
    res->cq = ibv_create_cq(res->context, 128, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        return -1;
    }

    /* 创建QP */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 128;
    qp_init_attr.cap.max_recv_wr = 128;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        fprintf(stderr, "Failed to create QP\n");
        return -1;
    }

    /* 注册内存 */
    res->buffer = malloc(BUFFER_SIZE);
    memset(res->buffer, 0, BUFFER_SIZE);
    
    res->mr = ibv_reg_mr(res->pd, res->buffer, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
    if (!res->mr) {
        fprintf(stderr, "Failed to register MR\n");
        return -1;
    }

    ibv_free_device_list(device_list);
    return 0;
}

/* 修改QP到RTR状态 */
int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qp_num, 
                     uint16_t remote_lid, uint8_t port)
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
            .port_num = port,
        },
    };

    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

/* 修改QP到RTS状态 */
int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = 0,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .max_rd_atomic = 1,
    };

    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

/* 执行RDMA Write */
int rdma_write(struct rdma_resources *res, uint64_t remote_addr, 
               uint32_t remote_rkey, void *local_buf, size_t size)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)local_buf,
        .length = size,
        .lkey = res->mr->lkey,
    };

    struct ibv_send_wr wr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = remote_addr,
            .rkey = remote_rkey,
        },
    };

    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(res->qp, &wr, &bad_wr)) {
        fprintf(stderr, "RDMA Write failed\n");
        return -1;
    }

    /* 等待完成 */
    struct ibv_wc wc;
    while (1) {
        int ne = ibv_poll_cq(res->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Poll CQ failed\n");
            return -1;
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC status: %d\n", wc.status);
                return -1;
            }
            break;
        }
    }

    return 0;
}

/* 清理资源 */
void cleanup_rdma_resources(struct rdma_resources *res)
{
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->buffer) free(res->buffer);
    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->context) ibv_close_device(res->context);
}

int main(int argc, char *argv[])
{
    struct rdma_resources res = {0};
    int is_server = (argc > 1 && strcmp(argv[1], "server") == 0);

    printf("Starting as %s\n", is_server ? "server" : "client");

    /* 初始化 */
    if (init_rdma_resources(&res)) {
        return 1;
    }

    /* 这里省略连接建立和QP状态转换代码 */
    /* 实际需要通过Send/Recv或RDMA CM交换QP信息 */

    if (is_server) {
        printf("Server ready. QP num: %u, MR lkey: %u\n", 
               res.qp->qp_num, res.mr->lkey);
        printf("MR addr: %lu, rkey: %u\n", 
               (unsigned long)res.buffer, res.mr->rkey);
        printf("Waiting for data...\n");
        
        /* 等待客户端写入 */
        sleep(10);
        
        /* 检查数据 */
        printf("Received: %.50s\n", res.buffer);
    } else {
        printf("Client ready. QP num: %u\n", res.qp->qp_num);
        
        /* 这里需要配置对端的QP信息 */
        /* 实际需要先获取server的QP信息 */
        
        /* 模拟写入 */
        /* rdma_write(&res, remote_addr, remote_rkey, "Hello RDMA!", 11); */
    }

    cleanup_rdma_resources(&res);
    return 0;
}
