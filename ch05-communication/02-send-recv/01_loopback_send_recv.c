/**
 * RDMA Send/Recv 完整示例 - Loopback 模式
 *
 * 这是一个 Loopback 示例，同一机器上模拟 client-server 通信。
 * 实际使用时需要两台机器，通过 TCP 交换 QP 信息。
 *
 * 增强功能:
 *   - IB/RoCE 自动检测 (detect_transport)
 *   - 多 SGE 发送示例 (使用 2 个 SGE 发送一条消息)
 *   - 打印详细 WC 字段 (print_wc_detail)
 *   - max_send_sge/max_recv_sge 提升到 4
 *
 * 编译: gcc -Wall -O2 -g -o 01_loopback_send_recv 01_loopback_send_recv.c \
 *        -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "../../common/rdma_utils.h"

#define BUFFER_SIZE 1024

/* RDMA 资源结构 */
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    char *send_buf;
    char *recv_buf;
    int is_roce;        /* IB/RoCE 自动检测结果 */
    uint8_t port;
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

    /* 3. 检测传输层类型 */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->context, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("传输层类型: %s\n", transport_str(transport));

    /* 4. 分配PD */
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        perror("分配PD失败");
        return -1;
    }

    /* 5. 创建CQ */
    res->cq = ibv_create_cq(res->context, 256, NULL, NULL, 0);
    if (!res->cq) {
        perror("创建CQ失败");
        return -1;
    }

    /* 6. 创建QP (max_send_sge=4, max_recv_sge=4 以支持多 SGE) */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 128;
    qp_init_attr.cap.max_recv_wr = 128;
    qp_init_attr.cap.max_send_sge = 4;  /* 提升到 4，支持多 SGE 发送 */
    qp_init_attr.cap.max_recv_sge = 4;  /* 提升到 4，支持多 SGE 接收 */

    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        perror("创建QP失败");
        return -1;
    }

    /* 7. 分配和注册内存 */
    res->send_buf = malloc(BUFFER_SIZE);
    res->recv_buf = malloc(BUFFER_SIZE);

    res->send_mr = ibv_reg_mr(res->pd, res->send_buf, BUFFER_SIZE,
                               IBV_ACCESS_LOCAL_WRITE);
    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, BUFFER_SIZE,
                               IBV_ACCESS_LOCAL_WRITE);

    ibv_free_device_list(device_list);
    return 0;
}

/* 发送数据 - 单 SGE 版本 */
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
 * 多 SGE 发送: 使用 2 个 SGE 将消息分为 "前缀" 和 "正文" 两段发送
 *
 * NIC 会自动将两段数据 gather 成一个 RDMA 消息。
 * 接收端在一个连续缓冲区中收到拼接后的完整消息。
 */
int post_send_multi_sge(struct rdma_resources *res,
                        const char *prefix, int prefix_len,
                        const char *body, int body_len)
{
    /* 将前缀和正文分别放入 send_buf 的不同位置 */
    memcpy(res->send_buf, prefix, prefix_len);
    memcpy(res->send_buf + 512, body, body_len);  /* 偏移 512 放正文 */

    /* 2 个 SGE: 分别指向前缀和正文 */
    struct ibv_sge sges[2];
    sges[0].addr   = (uint64_t)res->send_buf;
    sges[0].length = prefix_len;
    sges[0].lkey   = res->send_mr->lkey;

    sges[1].addr   = (uint64_t)(res->send_buf + 512);
    sges[1].length = body_len;
    sges[1].lkey   = res->send_mr->lkey;

    printf("  多 SGE 发送:\n");
    printf("    SGE[0]: addr=%p, length=%d (前缀)\n",
           (void *)sges[0].addr, sges[0].length);
    printf("    SGE[1]: addr=%p, length=%d (正文)\n",
           (void *)sges[1].addr, sges[1].length);

    struct ibv_send_wr wr = {
        .wr_id = 2,
        .sg_list = sges,
        .num_sge = 2,       /* 2 个 SGE! */
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

    printf("=== RDMA Send/Recv Loopback 示例 (增强版) ===\n\n");

    /* 初始化资源 */
    if (init_rdma_resources(&res) != 0) {
        return 1;
    }
    printf("资源初始化完成\n");
    printf("  QP号: %u\n", res.qp->qp_num);
    printf("  max_send_sge: 4, max_recv_sge: 4\n");

    /* 配置 QP 状态机 - 使用 IB/RoCE 自动检测 */
    printf("\n配置QP状态机 (Loopback)...\n");

    /* 填充本地端点信息 */
    struct rdma_endpoint self_ep;
    memset(&self_ep, 0, sizeof(self_ep));
    if (fill_local_endpoint(res.context, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &self_ep) != 0) {
        fprintf(stderr, "填充端点信息失败\n");
        return 1;
    }
    printf("  LID: %u\n", self_ep.lid);
    if (res.is_roce) {
        char gid_str[46];
        gid_to_str(&self_ep.gid, gid_str, sizeof(gid_str));
        printf("  GID: %s\n", gid_str);
    }

    /* QP 全状态转换: RESET -> INIT -> RTR -> RTS */
    int access = IBV_ACCESS_LOCAL_WRITE;
    if (qp_full_connect(res.qp, &self_ep, res.port, res.is_roce, access) != 0) {
        fprintf(stderr, "QP 建连失败\n");
        return 1;
    }
    printf("  QP状态: RTS (Ready to Send)\n");

    /* ========== 测试 1: 单 SGE Send/Recv ========== */
    printf("\n========================================\n");
    printf("  测试 1: 单 SGE Send/Recv\n");
    printf("========================================\n");

    /* 步骤1: 接收端先 post_recv */
    printf("\n[步骤1] 接收端 post_recv\n");
    memset(res.recv_buf, 0, BUFFER_SIZE);
    post_recv(&res);

    /* 步骤2: 发送端 post_send */
    printf("[步骤2] 发送端 post_send: \"%s\"\n", message);
    memcpy(res.send_buf, message, strlen(message) + 1);
    post_send(&res, res.send_buf, strlen(message) + 1);

    /* 步骤3: 等待发送完成，打印详细 WC */
    printf("[步骤3] 发送端等待 WC\n");
    struct ibv_wc wc;
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  发送 WC 详情:\n");
        print_wc_detail(&wc);
    }

    /* 步骤4: 等待接收完成，打印详细 WC */
    printf("[步骤4] 接收端等待 WC\n");
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  接收 WC 详情:\n");
        print_wc_detail(&wc);
    }

    /* 检查结果 */
    printf("\n[结果] 收到的消息: \"%s\"\n", res.recv_buf);

    /* ========== 测试 2: 多 SGE Send/Recv ========== */
    printf("\n========================================\n");
    printf("  测试 2: 多 SGE Send/Recv (2 个 SGE)\n");
    printf("========================================\n");

    const char *prefix = "[MSG] ";
    const char *body   = "这条消息由 2 个 SGE 拼接而成!";

    /* 接收端先 post recv */
    printf("\n[步骤1] 接收端 post_recv\n");
    memset(res.recv_buf, 0, BUFFER_SIZE);
    post_recv(&res);

    /* 多 SGE 发送 */
    printf("[步骤2] 多 SGE 发送:\n");
    printf("  前缀: \"%s\" (%zu 字节)\n", prefix, strlen(prefix));
    printf("  正文: \"%s\" (%zu 字节)\n", body, strlen(body) + 1);
    post_send_multi_sge(&res, prefix, strlen(prefix),
                        body, strlen(body) + 1);

    /* 等待发送完成 */
    printf("[步骤3] 发送端等待 WC\n");
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  发送 WC 详情:\n");
        print_wc_detail(&wc);
    }

    /* 等待接收完成 */
    printf("[步骤4] 接收端等待 WC\n");
    if (poll_cq_blocking(res.cq, &wc) == 0) {
        printf("  接收 WC 详情:\n");
        print_wc_detail(&wc);
        printf("  接收字节数: %u (前缀 %zu + 正文 %zu = %zu)\n",
               wc.byte_len, strlen(prefix), strlen(body) + 1,
               strlen(prefix) + strlen(body) + 1);
    }

    /* 检查拼接结果 */
    printf("\n[结果] 收到的消息: \"%s\"\n", res.recv_buf);

    /* 验证 */
    char expected[BUFFER_SIZE];
    snprintf(expected, sizeof(expected), "%s%s", prefix, body);
    if (strcmp(res.recv_buf, expected) == 0) {
        printf("[验证] ✓ 多 SGE 拼接正确!\n");
    } else {
        printf("[验证] ✗ 拼接不匹配! 期望: \"%s\"\n", expected);
    }

    /* 清理 */
    if (res.send_mr) ibv_dereg_mr(res.send_mr);
    if (res.recv_mr) ibv_dereg_mr(res.recv_mr);
    if (res.send_buf) free(res.send_buf);
    if (res.recv_buf) free(res.recv_buf);
    ibv_destroy_qp(res.qp);
    ibv_destroy_cq(res.cq);
    ibv_dealloc_pd(res.pd);
    ibv_close_device(res.context);

    printf("\n程序结束\n");
    return 0;
}
