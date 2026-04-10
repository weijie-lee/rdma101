/**
 * Shared Receive Queue (SRQ) 演示
 *
 * 演示 SRQ 的核心特性：
 *   - 创建 1 个 SRQ，由 2 个 QP 共享接收缓冲区
 *   - 向 SRQ 发布 recv WR (而不是向每个 QP 单独发布)
 *   - 2 个 QP 互相发送消息，SRQ 自动分配 recv buffer
 *
 * 原理：
 *   没有 SRQ 时，每个 QP 需要独立维护 recv buffer：
 *     N 个 QP × M 个 buffer = N×M 个 recv WR
 *
 *   使用 SRQ 后，所有 QP 共享一个 recv pool：
 *     1 个 SRQ × M 个 buffer = M 个 recv WR
 *
 *   这在大量连接 (数千个 QP) 的场景下能节省大量内存。
 *   例如：1000 个 QP，每个需要 128 个 recv buffer
 *     - 没有 SRQ: 1000 × 128 = 128000 个 recv WR
 *     - 使用 SRQ: 1 × 1000  = 1000 个 recv WR (足够应付并发)
 *
 * 编译:
 *   gcc -o srq_demo srq_demo.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * 运行: ./srq_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */
#define NUM_QPS         2           /* QP 数量 */
#define MSG_SIZE        128         /* 消息大小 */
#define BUFFER_SIZE     4096        /* 总缓冲区大小 */
#define CQ_DEPTH        64          /* CQ 深度 */
#define SRQ_MAX_WR      32          /* SRQ 最大 WR 数 */
#define MAX_SEND_WR     32          /* 每个 QP 的 SQ 最大 WR 数 */

/* ========== SRQ 演示的资源结构 ========== */
struct srq_resources {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_srq      *srq;           /* 共享接收队列 */
    struct ibv_cq       *send_cq;       /* 发送完成队列 */
    struct ibv_cq       *recv_cq;       /* 接收完成队列 (共享) */
    struct ibv_qp       *qp[NUM_QPS];   /* 2 个 QP，共享同一个 SRQ */
    struct ibv_mr       *send_mr;
    struct ibv_mr       *recv_mr;
    char                *send_buf;
    char                *recv_buf;
    int                  is_roce;
};

/* ========== 初始化所有资源 ========== */
static int init_resources(struct srq_resources *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices;
    int ret, i;

    memset(res, 0, sizeof(*res));

    /* 1. 打开设备 */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "[错误] 未找到 RDMA 设备\n");
        return -1;
    }

    res->ctx = ibv_open_device(dev_list[0]);
    if (!res->ctx) {
        fprintf(stderr, "[错误] 打开设备失败\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("[1] 打开设备: %s\n", ibv_get_device_name(dev_list[0]));
    ibv_free_device_list(dev_list);

    /* 自动检测传输层 */
    res->is_roce = (detect_transport(res->ctx, RDMA_DEFAULT_PORT_NUM)
                    == RDMA_TRANSPORT_ROCE);
    printf("    传输层: %s\n", res->is_roce ? "RoCE" : "IB");

    /* 2. 分配 PD */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配 PD 失败");
    printf("[2] 分配 PD\n");

    /* 3. 创建 CQ */
    res->send_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->send_cq, "创建 send_cq 失败");

    res->recv_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->recv_cq, "创建 recv_cq 失败");
    printf("[3] 创建 CQ (send_cq + recv_cq)\n");

    /* 4. 创建 SRQ (关键步骤!) */
    struct ibv_srq_init_attr srq_attr;
    memset(&srq_attr, 0, sizeof(srq_attr));
    srq_attr.attr.max_wr  = SRQ_MAX_WR;    /* SRQ 中最多容纳多少个 recv WR */
    srq_attr.attr.max_sge = 1;              /* 每个 recv WR 的 SGE 数 */

    res->srq = ibv_create_srq(res->pd, &srq_attr);
    CHECK_NULL(res->srq, "创建 SRQ 失败");
    printf("[4] 创建 SRQ (max_wr=%d, max_sge=%d)\n",
           SRQ_MAX_WR, 1);
    printf("    所有 QP 将共享这一个接收队列\n");

    /* 5. 分配和注册内存 */
    res->send_buf = malloc(BUFFER_SIZE);
    res->recv_buf = malloc(BUFFER_SIZE);
    CHECK_NULL(res->send_buf, "分配 send_buf 失败");
    CHECK_NULL(res->recv_buf, "分配 recv_buf 失败");
    memset(res->send_buf, 0, BUFFER_SIZE);
    memset(res->recv_buf, 0, BUFFER_SIZE);

    res->send_mr = ibv_reg_mr(res->pd, res->send_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->send_mr, "注册 send_mr 失败");

    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->recv_mr, "注册 recv_mr 失败");
    printf("[5] 分配并注册内存 (send + recv)\n");

    /* 6. 创建 2 个 QP，都使用同一个 SRQ */
    for (i = 0; i < NUM_QPS; i++) {
        struct ibv_qp_init_attr qp_attr;
        memset(&qp_attr, 0, sizeof(qp_attr));
        qp_attr.send_cq = res->send_cq;
        qp_attr.recv_cq = res->recv_cq;
        qp_attr.srq      = res->srq;       /* 关键: 绑定 SRQ */
        qp_attr.qp_type  = IBV_QPT_RC;
        qp_attr.cap.max_send_wr  = MAX_SEND_WR;
        qp_attr.cap.max_send_sge = 1;
        /*
         * 使用 SRQ 时，QP 自己的 RQ 不再使用。
         * max_recv_wr 和 max_recv_sge 可以设为 0。
         * recv WR 统一提交到 SRQ。
         */
        qp_attr.cap.max_recv_wr  = 0;
        qp_attr.cap.max_recv_sge = 0;

        res->qp[i] = ibv_create_qp(res->pd, &qp_attr);
        if (!res->qp[i]) {
            fprintf(stderr, "[错误] 创建 QP[%d] 失败: %s\n", i, strerror(errno));
            goto cleanup;
        }
        printf("[6] 创建 QP[%d] #%u (使用 SRQ, max_recv_wr=0)\n",
               i, res->qp[i]->qp_num);
    }

    return 0;

cleanup:
    return -1;
}

/* ========== Loopback 建连: QP[0] <--> QP[1] ========== */
static int connect_qps(struct srq_resources *res)
{
    struct rdma_endpoint ep[NUM_QPS];
    int ret, i;

    printf("\n[7] Loopback 建连: QP[0] <--> QP[1]\n");

    /* 填充两个 QP 的端点信息 */
    for (i = 0; i < NUM_QPS; i++) {
        ret = fill_local_endpoint(res->ctx, res->qp[i],
                                  RDMA_DEFAULT_PORT_NUM,
                                  RDMA_DEFAULT_GID_INDEX, &ep[i]);
        if (ret != 0) {
            fprintf(stderr, "[错误] 填充 QP[%d] 端点信息失败\n", i);
            return -1;
        }
    }

    /* QP[0] 连接到 QP[1] 的端点, QP[1] 连接到 QP[0] 的端点 */
    ret = qp_full_connect(res->qp[0], &ep[1], RDMA_DEFAULT_PORT_NUM,
                          res->is_roce, IBV_ACCESS_LOCAL_WRITE);
    if (ret != 0) {
        fprintf(stderr, "[错误] QP[0] 建连失败\n");
        return -1;
    }

    ret = qp_full_connect(res->qp[1], &ep[0], RDMA_DEFAULT_PORT_NUM,
                          res->is_roce, IBV_ACCESS_LOCAL_WRITE);
    if (ret != 0) {
        fprintf(stderr, "[错误] QP[1] 建连失败\n");
        return -1;
    }

    for (i = 0; i < NUM_QPS; i++) {
        printf("    QP[%d] #%u: ", i, res->qp[i]->qp_num);
        print_qp_state(res->qp[i]);
    }

    return 0;
}

/* ========== 向 SRQ 发布 recv WR ========== */
static int post_srq_recvs(struct srq_resources *res, int count)
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    int i, ret;

    printf("\n[8] 向 SRQ 发布 %d 个 recv WR\n", count);
    printf("    注意: recv WR 发布到 SRQ，而不是任何单独的 QP\n");

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)(res->recv_buf + i * MSG_SIZE);
        sge.length = MSG_SIZE;
        sge.lkey   = res->recv_mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id   = i;        /* 用 wr_id 标记 recv buffer 索引 */
        wr.sg_list = &sge;
        wr.num_sge = 1;

        /*
         * ibv_post_srq_recv(): 向 SRQ 发布 recv WR
         * 与 ibv_post_recv(qp, ...) 不同，这里参数是 srq
         * 任何绑定了此 SRQ 的 QP 收到消息时，都会从 SRQ 消费一个 recv WR
         */
        ret = ibv_post_srq_recv(res->srq, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] ibv_post_srq_recv #%d 失败: %s\n",
                    i, strerror(ret));
            return -1;
        }
    }

    printf("    已发布 %d 个 recv WR 到 SRQ\n", count);
    return 0;
}

/* ========== 通过指定 QP 发送消息 ========== */
static int send_message(struct srq_resources *res, int qp_index,
                        const char *msg)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int ret;

    /* 将消息拷贝到发送缓冲区 (不同 QP 用不同偏移避免覆盖) */
    int offset = qp_index * MSG_SIZE;
    snprintf(res->send_buf + offset, MSG_SIZE, "%s", msg);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)(res->send_buf + offset);
    sge.length = strlen(msg) + 1;
    sge.lkey   = res->send_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 1000 + qp_index;   /* 用 wr_id 标识发送端 */
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(res->qp[qp_index], &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] QP[%d] post_send 失败: %s\n",
                qp_index, strerror(ret));
        return -1;
    }

    /* 等待发送完成 */
    ret = poll_cq_blocking(res->send_cq, &wc);
    if (ret != 0) {
        fprintf(stderr, "[错误] QP[%d] send CQE 失败\n", qp_index);
        return -1;
    }

    printf("    QP[%d] 发送: \"%s\" (len=%zu)\n", qp_index, msg, strlen(msg) + 1);
    return 0;
}

/* ========== 从 recv CQ 接收消息 ========== */
static int recv_message(struct srq_resources *res, int expected_count)
{
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < expected_count; i++) {
        ret = poll_cq_blocking(res->recv_cq, &wc);
        if (ret != 0) {
            fprintf(stderr, "[错误] recv poll_cq 失败\n");
            return -1;
        }

        /* 从 WC 中获取信息 */
        int buf_index = wc.wr_id;   /* 对应 SRQ 中的 recv WR 索引 */
        char *data = res->recv_buf + buf_index * MSG_SIZE;

        printf("    SRQ 收到消息: \"%s\" (wr_id=%lu, qp_num=%u, len=%u)\n",
               data, (unsigned long)wc.wr_id, wc.qp_num, wc.byte_len);

        /*
         * 注意: wc.qp_num 表示哪个 QP 收到了这条消息
         * 由于使用 SRQ，消息可以从任何 QP 到达
         * SRQ 自动将空闲的 recv buffer 分配给需要的 QP
         */
    }

    return 0;
}

/* ========== 清理资源 ========== */
static void cleanup_resources(struct srq_resources *res)
{
    int i;

    for (i = 0; i < NUM_QPS; i++) {
        if (res->qp[i]) ibv_destroy_qp(res->qp[i]);
    }
    if (res->srq)     ibv_destroy_srq(res->srq);
    if (res->send_cq) ibv_destroy_cq(res->send_cq);
    if (res->recv_cq) ibv_destroy_cq(res->recv_cq);
    if (res->send_mr) ibv_dereg_mr(res->send_mr);
    if (res->recv_mr) ibv_dereg_mr(res->recv_mr);
    if (res->pd)      ibv_dealloc_pd(res->pd);
    if (res->ctx)     ibv_close_device(res->ctx);
    free(res->send_buf);
    free(res->recv_buf);
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    struct srq_resources res;
    int ret;

    printf("=== Shared Receive Queue (SRQ) 演示 ===\n\n");
    printf("演示目标:\n");
    printf("  - 创建 1 个 SRQ, 被 2 个 QP 共享\n");
    printf("  - recv WR 发布到 SRQ, 而非单独的 QP\n");
    printf("  - 两个 QP 互相发消息, SRQ 自动分配 recv buffer\n\n");

    /* 初始化资源 */
    ret = init_resources(&res);
    if (ret != 0) {
        fprintf(stderr, "[错误] 资源初始化失败\n");
        cleanup_resources(&res);
        return 1;
    }

    /* 建连 */
    ret = connect_qps(&res);
    if (ret != 0) {
        fprintf(stderr, "[错误] QP 建连失败\n");
        cleanup_resources(&res);
        return 1;
    }

    /* 向 SRQ 发布 recv WR */
    ret = post_srq_recvs(&res, 4);
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* QP[0] 发送消息给 QP[1] */
    printf("\n[9] QP[0] → QP[1] 发送消息\n");
    ret = send_message(&res, 0, "Hello from QP[0]!");
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* QP[1] 发送消息给 QP[0] */
    printf("\n[10] QP[1] → QP[0] 发送消息\n");
    ret = send_message(&res, 1, "Hello from QP[1]!");
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 接收消息 (2 条) */
    printf("\n[11] 从 SRQ (recv CQ) 接收消息\n");
    ret = recv_message(&res, 2);
    if (ret != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* ---- 总结 ---- */
    printf("\n===== SRQ 原理总结 =====\n");
    printf("  1. ibv_create_srq(pd, &attr) 创建共享接收队列\n");
    printf("  2. 创建 QP 时设置 qp_attr.srq = srq, max_recv_wr=0\n");
    printf("  3. 所有 recv WR 通过 ibv_post_srq_recv() 提交到 SRQ\n");
    printf("  4. 任何绑定该 SRQ 的 QP 收到消息时，自动从 SRQ 消费 recv WR\n");
    printf("  5. recv CQ 中的 wc.qp_num 标识消息来自哪个 QP\n");
    printf("\n===== SRQ 使用场景 =====\n");
    printf("  - 数据库: 数千个客户端连接，每个 QP 不确定何时有消息到达\n");
    printf("  - 存储系统: 大量节点间的 RDMA 连接\n");
    printf("  - 关键优势: 将 N×M 个 recv buffer 减少到 M 个\n");
    printf("  - 内存节省: QP 数越多，SRQ 的节省越明显\n");
    printf("  - 例: 1000 QP × 128 recv = 128000 → SRQ 仅需 1000 个\n");

    /* 清理 */
    cleanup_resources(&res);
    printf("\n程序结束\n");
    return 0;
}
