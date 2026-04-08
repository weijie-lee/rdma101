/**
 * RDMA Send/Recv 完整示例
 * 
 * 这是一个Loopback示例，同一机器上模拟client-server通信
 * 实际使用时需要两台机器，通过TCP交换QP信息
 * 
 * 编译: gcc -o 01_loopback_send_recv 01_loopback_send_recv.c -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024

/* RDMA资源结构 */
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    char *send_buf;
    char *recv_buf;
};

/* 初始化RDMA资源 */
int init_rdma_resources(struct rdma_resources *res)
{
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_qp_init_attr qp_init_attr;
    int num;
    
    /* 1. 获取设备 */
    device_list = ibv_get_device_list(&num);
    if (!device_list || num == 0) {
        fprintf(stderr, "没有RDMA设备\n");
        return -1;
    }
    device = device_list[0];
    
    /* 2. 打开设备 */
    res->context = ibv_open_device(device);
    if (!res->context) {
        perror("打开设备失败");
        return -1;
    }
    
    /* 3. 分配PD */
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        perror("分配PD失败");
        return -1;
    }
    
    /* 4. 创建CQ */
    res->cq = ibv_create_cq(res->context, 256, NULL, NULL, 0);
    if (!res->cq) {
        perror("创建CQ失败");
        return -1;
    }
    
    /* 5. 创建QP */
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
        perror("创建QP失败");
        return -1;
    }
    
    /* 6. 分配和注册内存 */
    res->send_buf = malloc(BUFFER_SIZE);
    res->recv_buf = malloc(BUFFER_SIZE);
    
    res->send_mr = ibv_reg_mr(res->pd, res->send_buf, BUFFER_SIZE,
                               IBV_ACCESS_LOCAL_WRITE);
    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, BUFFER_SIZE,
                               IBV_ACCESS_LOCAL_WRITE);
    
    ibv_free_device_list(device_list);
    return 0;
}

/* 配置QP状态 */
int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE,
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

int modify_qp_to_rtr(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_256,
        .dest_qp_num = qp->qp_num,  /* Loopback: 自己的QP号 */
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
    };
    attr.ah_attr.dlid = 1;  /* Loopback */
    attr.ah_attr.port_num = 1;
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

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

/* 发送数据 */
int post_send(struct rdma_resources *res, char *data, int len)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)data,
        .length = len,
        .lkey = res->send_mr->lkey,
    };
    
    struct ibv_send_wr wr = {
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    
    struct ibv_send_wr *bad_wr;
    return ibv_post_send(res->qp, &wr, &bad_wr);
}

/* 接收数据 */
int post_recv(struct rdma_resources *res)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)res->recv_buf,
        .length = BUFFER_SIZE,
        .lkey = res->recv_mr->lkey,
    };
    
    struct ibv_recv_wr wr = {
        .sg_list = &sge,
        .num_sge = 1,
    };
    
    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(res->qp, &wr, &bad_wr);
}

/* 轮询CQ */
int poll_cq(struct ibv_cq *cq)
{
    struct ibv_wc wc;
    int ne;
    
    while (1) {
        ne = ibv_poll_cq(cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "轮询CQ失败\n");
            return -1;
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC失败: %s\n", ibv_wc_status_str(wc.status));
                return -1;
            }
            return wc.byte_len;
        }
        /* ne == 0, 继续轮询 */
    }
}

int main(int argc, char *argv[])
{
    struct rdma_resources res = {0};
    const char *message = "Hello RDMA!";
    
    printf("=== RDMA Send/Recv Loopback 示例 ===\n\n");
    
    /* 初始化资源 */
    if (init_rdma_resources(&res) != 0) {
        return 1;
    }
    printf("资源初始化完成\n");
    printf("  QP号: %u\n", res.qp->qp_num);
    
    /* 配置QP状态机 */
    printf("\n配置QP状态机...\n");
    modify_qp_to_init(res.qp);
    modify_qp_to_rtr(res.qp);
    modify_qp_to_rts(res.qp);
    printf("  QP状态: RTS (Ready to Send)\n");
    
    /* 步骤1: 接收端先post_recv */
    printf("\n[步骤1] 接收端post_recv\n");
    post_recv(&res);
    
    /* 步骤2: 发送端post_send */
    printf("[步骤2] 发送端post_send: \"%s\"\n", message);
    memcpy(res.send_buf, message, strlen(message) + 1);
    post_send(&res, res.send_buf, strlen(message) + 1);
    
    /* 步骤3: 发送端等待完成 */
    printf("[步骤3] 发送端等待WC\n");
    poll_cq(res.cq);
    printf("  发送完成!\n");
    
    /* 步骤4: 接收端等待完成 */
    printf("[步骤4] 接收端等待WC\n");
    poll_cq(res.cq);
    printf("  接收完成!\n");
    
    /* 检查结果 */
    printf("\n[结果] 收到的消息: \"%s\"\n", res.recv_buf);
    
    /* 清理 */
    ibv_destroy_qp(res.qp);
    ibv_destroy_cq(res.cq);
    ibv_dereg_mr(res.send_mr);
    ibv_dereg_mr(res.recv_mr);
    ibv_dealloc_pd(res.pd);
    ibv_close_device(res.context);
    
    printf("\n程序结束\n");
    return 0;
}
