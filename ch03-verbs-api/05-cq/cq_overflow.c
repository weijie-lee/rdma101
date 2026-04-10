/**
 * CQ 溢出 (Overflow) 演示
 *
 * 本程序演示当 CQ 被填满后继续产生完成事件会发生什么:
 *   1. 创建非常小的 CQ (深度=2)
 *   2. 快速提交多个 Send WR, 产生大量完成事件
 *   3. 不及时 poll CQ, 导致 CQ 溢出
 *   4. 捕获并打印溢出错误
 *   5. 解释生产环境中如何避免 (合理 CQ 大小、及时轮询)
 *
 * 编译: gcc -o cq_overflow cq_overflow.c -I../../common ../../common/librdma_utils.a -libverbs
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
#define MSG_SIZE    64

/*
 * 注意: CQ 的最小深度由硬件决定, 有些 HCA 会将很小的值向上取整。
 * 例如请求 cq_depth=2, 实际可能分配 16 或更多。
 * 我们尽量请求最小值, 然后尽力触发溢出。
 */
#define SMALL_CQ_DEPTH  2
#define NUM_WR_TO_POST  32    /* 尝试提交的 WR 数量 (远超 CQ 深度) */

int main(int argc, char *argv[])
{
    /* 资源声明 */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_cq      *cq_small = NULL;   /* 很小的 CQ */
    struct ibv_qp      *qp       = NULL;
    struct ibv_mr      *mr       = NULL;
    char               *buffer   = NULL;
    int                 num_devices;
    int                 ret;
    int                 i;

    printf("============================================\n");
    printf("  CQ 溢出 (Overflow) 演示\n");
    printf("============================================\n\n");

    printf("  实验设计:\n");
    printf("  - 创建非常小的 CQ (请求深度=%d)\n", SMALL_CQ_DEPTH);
    printf("  - 提交 %d 个 Send WR (全部 SIGNALED)\n", NUM_WR_TO_POST);
    printf("  - 故意不 poll CQ, 导致 CQ 溢出\n");
    printf("  - 观察溢出时的错误现象\n\n");

    /* ========== 初始化资源 ========== */
    printf("[步骤1] 初始化 RDMA 资源\n");
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未发现 RDMA 设备\n");
        goto cleanup;
    }
    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "打开设备失败");
    printf("  设备: %s\n", ibv_get_device_name(dev_list[0]));

    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "分配 PD 失败");

    buffer = malloc(BUFFER_SIZE);
    CHECK_NULL(buffer, "malloc 失败");
    memset(buffer, 0, BUFFER_SIZE);

    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(mr, "注册 MR 失败");
    printf("  PD, MR 创建完成\n\n");

    /* ========== 步骤 2: 创建小 CQ ========== */
    printf("[步骤2] 创建极小 CQ (请求深度=%d)\n", SMALL_CQ_DEPTH);

    /*
     * ibv_create_cq() 的 cqe 参数是最小保证值, HCA 可能实际分配更多。
     * 通过 cq->cqe 可以查看实际分配的大小。
     */
    cq_small = ibv_create_cq(ctx, SMALL_CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq_small, "创建小 CQ 失败");
    printf("  请求深度: %d, 实际分配深度: %d\n", SMALL_CQ_DEPTH, cq_small->cqe);
    printf("  → 注意: 实际深度可能大于请求值 (硬件对齐)\n\n");

    /* ========== 步骤 3: 创建 QP 并 loopback 连接 ========== */
    printf("[步骤3] 创建 QP 并建立 loopback 连接\n");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq_small,
        .recv_cq = cq_small,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = NUM_WR_TO_POST,
            .max_recv_wr  = NUM_WR_TO_POST,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp = ibv_create_qp(pd, &qp_init);
    CHECK_NULL(qp, "创建 QP 失败");
    printf("  QP 创建成功, qp_num=%u\n", qp->qp_num);

    /* loopback 连接 */
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "填充端点信息失败");

    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("  QP 连接失败, 跳过溢出测试\n");
        goto cleanup;
    }
    printf("  QP 已连接 (loopback, RTS 状态)\n\n");

    /* ========== 步骤 4: 先 post recv 准备接收 ========== */
    printf("[步骤4] Post Recv 准备接收缓冲区\n");
    for (i = 0; i < NUM_WR_TO_POST; i++) {
        struct ibv_sge recv_sge = {
            .addr   = (uintptr_t)(buffer + (i % 8) * MSG_SIZE),
            .length = MSG_SIZE,
            .lkey   = mr->lkey,
        };
        struct ibv_recv_wr recv_wr = {
            .wr_id   = 5000 + i,
            .sg_list = &recv_sge,
            .num_sge = 1,
        };
        struct ibv_recv_wr *bad_recv = NULL;
        ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
        if (ret != 0) {
            printf("  post_recv[%d] 失败: %s\n", i, strerror(errno));
            break;
        }
    }
    printf("  已提交 %d 个 Recv WR\n\n", i);

    /* ========== 步骤 5: 快速提交大量 Send WR (不 poll CQ!) ========== */
    printf("[步骤5] 快速提交 %d 个 Send WR (全部 SIGNALED, 不 poll CQ)\n", NUM_WR_TO_POST);
    printf("========================================\n");

    int posted = 0;
    for (i = 0; i < NUM_WR_TO_POST; i++) {
        struct ibv_sge sge = {
            .addr   = (uintptr_t)buffer,
            .length = MSG_SIZE,
            .lkey   = mr->lkey,
        };
        struct ibv_send_wr wr = {
            .wr_id      = 1000 + i,
            .sg_list    = &sge,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,  /* 每个都请求完成通知 */
        };
        struct ibv_send_wr *bad_wr = NULL;

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            printf("  post_send[%d] 失败: errno=%d (%s)\n", i, errno, strerror(errno));
            printf("  → 可能因为 Send Queue 已满 (与 CQ 溢出不同)\n");
            break;
        }
        posted++;
    }
    printf("  成功提交 %d 个 Send WR\n", posted);
    printf("  CQ 深度=%d, 但会产生 %d 个完成事件 (send + recv)\n",
           cq_small->cqe, posted * 2);

    /* 等待一小段时间让 HCA 处理 */
    usleep(100000);  /* 100ms */

    /* ========== 步骤 6: 尝试 poll CQ, 观察溢出 ========== */
    printf("\n[步骤6] 尝试 poll CQ, 观察溢出效果\n");
    printf("========================================\n");

    struct ibv_wc wc;
    int ne;
    int success_count = 0;
    int error_count = 0;

    /* 尝试 poll 所有完成事件 */
    while ((ne = ibv_poll_cq(cq_small, 1, &wc)) != 0) {
        if (ne < 0) {
            printf("  ✗ ibv_poll_cq 返回 %d — CQ 可能已溢出!\n", ne);
            printf("  → CQ overflow: 当 CQ 满了，HCA 无法写入新的 CQE\n");
            printf("  → 关联的 QP 会被移到 ERROR 状态\n");
            error_count++;
            break;
        }
        if (wc.status == IBV_WC_SUCCESS) {
            success_count++;
        } else {
            error_count++;
            printf("  WC 错误: wr_id=%lu, status=%s (%d), vendor_err=0x%x\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status), wc.status,
                   wc.vendor_err);

            /*
             * CQ 溢出时, QP 会被移到 ERROR 状态。
             * 后续的 WC 可能显示:
             *   - IBV_WC_WR_FLUSH_ERR: QP 进入 ERROR 后，未完成的 WR 被 flush
             *   - IBV_WC_GENERAL_ERR: 一般错误
             */
            if (wc.status == IBV_WC_WR_FLUSH_ERR) {
                printf("  → WR Flush Error: QP 已进入 ERROR 状态, WR 被冲刷\n");
            }
        }
    }

    printf("\n  结果: 成功完成=%d, 错误完成=%d\n", success_count, error_count);

    /* 检查 QP 当前状态 */
    printf("\n  检查 QP 状态:\n");
    print_qp_state(qp);

    if (error_count > 0) {
        printf("\n  ✓ 观察到 CQ 溢出效果!\n");
        printf("  → 当 CQ 满了, HCA 无法写入新的 CQE\n");
        printf("  → QP 被移到 ERROR 状态, 所有后续 WR 被 flush\n");
    } else if (success_count == posted * 2) {
        printf("\n  所有 WC 都成功了 (实际 CQ 深度足够大: %d)\n", cq_small->cqe);
        printf("  → 硬件实际分配的 CQ 深度 >= %d, 未溢出\n", posted * 2);
        printf("  → 要触发溢出, 需要提交更多 WR 或使用支持更小 CQ 的硬件\n");
    }

    /* ========== 总结 ========== */
    printf("\n============================================\n");
    printf("  CQ 溢出预防指南\n");
    printf("============================================\n");
    printf("  1. CQ 大小规划:\n");
    printf("     - CQ 深度 >= 关联的所有 QP 的 send_wr + recv_wr 总和\n");
    printf("     - 公式: cq_depth >= N_qp * (max_send_wr + max_recv_wr)\n");
    printf("     - 留出余量 (通常 2x)\n\n");
    printf("  2. 及时 Poll CQ:\n");
    printf("     - 忙轮询: 持续调用 ibv_poll_cq()\n");
    printf("     - 事件驱动: 收到通知后立即 poll 到空\n");
    printf("     - 批量 poll: ibv_poll_cq(cq, batch_size, wc_array)\n\n");
    printf("  3. 使用 Shared Receive Queue (SRQ):\n");
    printf("     - 多个 QP 共享接收队列, 减少总 WR 数量\n\n");
    printf("  4. 信号选择 (Selective Signaling):\n");
    printf("     - 并非每个 WR 都需要完成通知\n");
    printf("     - 去掉 IBV_SEND_SIGNALED, 每 N 个 WR 只 signal 一个\n");
    printf("     - 大幅减少 CQ 压力\n\n");

cleanup:
    printf("[清理] 释放资源...\n");
    if (qp)       ibv_destroy_qp(qp);
    if (cq_small) ibv_destroy_cq(cq_small);
    if (mr)       ibv_dereg_mr(mr);
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  完成\n");
    return 0;
}
