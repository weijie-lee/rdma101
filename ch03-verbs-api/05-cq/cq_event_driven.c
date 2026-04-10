/**
 * 事件驱动型 CQ 完成通知演示 (Event-Driven Completion)
 *
 * 本程序演示基于 Completion Channel 的事件驱动 CQ 通知机制:
 *   1. 创建 ibv_comp_channel (完成通知通道)
 *   2. 创建 CQ 并关联到 channel
 *   3. 使用 ibv_req_notify_cq() 注册通知 (arm CQ)
 *   4. 使用 ibv_get_cq_event() 阻塞等待完成事件
 *   5. 使用 ibv_ack_cq_events() 确认事件
 *   6. 通过 loopback Send/Recv 触发真实完成事件
 *   7. 与忙轮询 (busy polling) 方式做对比说明
 *
 * 编译: gcc -o cq_event_driven cq_event_driven.c -I../../common ../../common/librdma_utils.a -libverbs -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE 4096
#define PORT_NUM    RDMA_DEFAULT_PORT_NUM
#define CQ_DEPTH    32
#define MSG_SIZE    64

int main(int argc, char *argv[])
{
    /* 资源声明 */
    struct ibv_device      **dev_list = NULL;
    struct ibv_context      *ctx      = NULL;
    struct ibv_pd           *pd       = NULL;
    struct ibv_comp_channel *channel  = NULL;  /* 完成通知通道 */
    struct ibv_cq           *cq_event = NULL;  /* 事件驱动 CQ */
    struct ibv_cq           *cq_poll  = NULL;  /* 忙轮询 CQ (对比用) */
    struct ibv_qp           *qp       = NULL;
    struct ibv_mr           *mr       = NULL;
    char                    *buffer   = NULL;
    int                      num_devices;
    int                      ret;
    unsigned int             events_completed = 0;  /* 已确认的事件计数 */

    printf("============================================\n");
    printf("  事件驱动型 CQ 完成通知演示\n");
    printf("============================================\n\n");

    /*
     * 背景知识: CQ 完成通知的两种模式
     *
     * 模式 1: 忙轮询 (Busy Polling)
     *   while (1) { ne = ibv_poll_cq(cq, 1, &wc); if (ne > 0) break; }
     *   + 优点: 延迟最低 (纳秒级)
     *   - 缺点: CPU 100% 占用, 浪费 CPU 资源
     *
     * 模式 2: 事件驱动 (Event-Driven)
     *   ibv_req_notify_cq(cq, 0);        // 注册通知
     *   ibv_get_cq_event(channel, ...);   // 阻塞等待
     *   ibv_poll_cq(cq, ...);             // 收到通知后再轮询
     *   ibv_ack_cq_events(cq, 1);         // 确认事件
     *   + 优点: CPU 友好, 适合低频场景
     *   - 缺点: 有额外延迟 (微秒级), 需要内核参与
     *
     * 实践中常用混合模式: 先忙轮询一段时间, 无事件则切换到事件等待。
     */

    /* ========== 步骤 1: 初始化基础资源 ========== */
    printf("[步骤1] 初始化基础 RDMA 资源\n");
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

    /* ========== 步骤 2: 创建完成通知通道 ========== */
    printf("[步骤2] 创建完成通知通道 (Completion Channel)\n");

    /*
     * ibv_create_comp_channel() 创建一个 fd (文件描述符),
     * 用于 select/poll/epoll 等 I/O 多路复用。
     * CQ 事件到达时, 这个 fd 变为可读。
     */
    channel = ibv_create_comp_channel(ctx);
    CHECK_NULL(channel, "创建 Completion Channel 失败");
    printf("  Channel 创建成功, fd=%d\n", channel->fd);
    printf("  → 可用于 select/poll/epoll 多路复用\n\n");

    /* ========== 步骤 3: 创建事件驱动 CQ ========== */
    printf("[步骤3] 创建事件驱动 CQ (关联 Channel)\n");

    /*
     * ibv_create_cq() 的第 4 个参数是 comp_channel:
     *   - NULL: 不关联通道 (仅支持忙轮询)
     *   - channel: 关联通道 (支持事件通知)
     *
     * 第 5 个参数 comp_vector: 用于选择中断向量 (多核负载均衡)
     */
    cq_event = ibv_create_cq(ctx, CQ_DEPTH, NULL, channel, 0);
    CHECK_NULL(cq_event, "创建事件驱动 CQ 失败");
    printf("  事件驱动 CQ 创建成功 (深度=%d, 关联 channel fd=%d)\n",
           CQ_DEPTH, channel->fd);

    /* 同时创建一个普通 CQ (用于对比) */
    cq_poll = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq_poll, "创建轮询 CQ 失败");
    printf("  轮询 CQ 创建成功 (无 channel, 仅支持忙轮询)\n\n");

    /* ========== 步骤 4: 创建 QP 并连接 (loopback) ========== */
    printf("[步骤4] 创建 QP 并建立 loopback 连接\n");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq_event,   /* 发送完成走事件驱动 CQ */
        .recv_cq = cq_event,   /* 接收完成也走事件驱动 CQ */
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
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
    printf("  传输类型: %s\n", transport_str(transport));

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("  QP 连接失败, 跳过事件通知测试\n");
        printf("  → 提示: 确保端口状态为 ACTIVE\n");
        goto cleanup;
    }
    printf("  QP 已连接 (loopback, RTS 状态)\n\n");

    /* ========== 步骤 5: 事件驱动完成通知流程 ========== */
    printf("========================================\n");
    printf("[步骤5] 事件驱动完成通知流程\n");
    printf("========================================\n\n");

    /*
     * 步骤 5a: 先 Post Recv (接收端准备好)
     */
    printf("  5a. Post Recv (准备接收缓冲区)\n");
    struct ibv_sge recv_sge = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr recv_wr = {
        .wr_id   = 1000,
        .sg_list = &recv_sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv = NULL;
    ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
    CHECK_ERRNO(ret, "ibv_post_recv 失败");
    printf("      Recv WR 已提交 (wr_id=1000)\n");

    /*
     * 步骤 5b: Arm CQ — 注册完成通知
     *
     * ibv_req_notify_cq(cq, solicited_only):
     *   - solicited_only=0: 所有完成事件都通知
     *   - solicited_only=1: 仅 Send with Solicited 标志的完成才通知
     *
     * 注意: 必须在 ibv_get_cq_event() 之前调用!
     *        arm 是一次性的, 每次 get_event 后必须重新 arm。
     */
    printf("  5b. Arm CQ (ibv_req_notify_cq)\n");
    ret = ibv_req_notify_cq(cq_event, 0);
    CHECK_ERRNO(ret, "ibv_req_notify_cq 失败");
    printf("      CQ 通知已注册 (solicited_only=0)\n");

    /*
     * 步骤 5c: Post Send (触发完成事件)
     */
    printf("  5c. Post Send (触发 loopback Send/Recv)\n");
    snprintf(buffer + MSG_SIZE, MSG_SIZE, "Hello RDMA Event-Driven!");
    struct ibv_sge send_sge = {
        .addr   = (uintptr_t)(buffer + MSG_SIZE),
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id      = 2000,
        .sg_list    = &send_sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send = NULL;
    ret = ibv_post_send(qp, &send_wr, &bad_send);
    CHECK_ERRNO(ret, "ibv_post_send 失败");
    printf("      Send WR 已提交 (wr_id=2000)\n");

    /*
     * 步骤 5d: 阻塞等待 CQ 事件
     *
     * ibv_get_cq_event() 会阻塞直到:
     *   - CQ 上有新的完成事件到达
     *   - 底层 fd 变为可读
     *
     * 实际生产中, 常用 poll()/epoll() 来监控 channel->fd,
     * 配合超时机制, 避免永久阻塞。
     */
    printf("  5d. 等待 CQ 事件 (ibv_get_cq_event, 阻塞)...\n");

    struct ibv_cq *ev_cq = NULL;
    void *ev_ctx = NULL;
    ret = ibv_get_cq_event(channel, &ev_cq, &ev_ctx);
    CHECK_ERRNO(ret, "ibv_get_cq_event 失败");
    events_completed++;
    printf("      ✓ 收到 CQ 事件通知! (ev_cq=%p)\n", (void *)ev_cq);

    /*
     * 步骤 5e: 收到通知后, 轮询 CQ 获取所有完成事件
     *
     * 重要: 一次 CQ 事件通知可能对应多个 WC (完成事件合并)。
     * 必须循环 poll 直到返回 0, 确保取出所有完成事件。
     */
    printf("  5e. 轮询 CQ 获取完成事件\n");
    struct ibv_wc wc;
    int ne, total = 0;
    while ((ne = ibv_poll_cq(cq_event, 1, &wc)) > 0) {
        total++;
        printf("      WC[%d]: wr_id=%lu, status=%s, opcode=%s\n",
               total, (unsigned long)wc.wr_id,
               ibv_wc_status_str(wc.status),
               wc_opcode_str(wc.opcode));
    }
    printf("      共收到 %d 个完成事件\n", total);

    /*
     * 步骤 5f: 确认 CQ 事件 (必须!)
     *
     * ibv_ack_cq_events() 必须在 ibv_destroy_cq() 之前被调用,
     * 确认所有收到的事件。如果不确认, ibv_destroy_cq() 会阻塞。
     *
     * 可以批量确认: 累积 N 次 get_event, 然后 ack_events(cq, N)。
     */
    printf("  5f. 确认 CQ 事件 (ibv_ack_cq_events)\n");
    ibv_ack_cq_events(cq_event, events_completed);
    printf("      已确认 %u 个事件\n", events_completed);
    events_completed = 0;

    /*
     * 步骤 5g: 重新 Arm CQ (如需继续监听)
     *
     * arm 是一次性的! 每次 get_event 后都要重新 arm。
     * 推荐在 poll CQ 完成后立即 re-arm, 减少丢失通知的窗口。
     */
    printf("  5g. 重新 Arm CQ (如需继续监听)\n");
    ret = ibv_req_notify_cq(cq_event, 0);
    CHECK_ERRNO(ret, "re-arm ibv_req_notify_cq 失败");
    printf("      CQ 已重新 arm, 可继续等待下一轮事件\n");

    /* ========== 总结 ========== */
    printf("\n============================================\n");
    printf("  事件驱动 CQ 总结\n");
    printf("============================================\n");
    printf("  完整流程:\n");
    printf("    1. ibv_create_comp_channel()      → 创建通道\n");
    printf("    2. ibv_create_cq(..., channel, 0)  → CQ 关联通道\n");
    printf("    3. ibv_req_notify_cq(cq, 0)        → Arm CQ\n");
    printf("    4. ibv_post_send/recv(...)          → 提交 WR\n");
    printf("    5. ibv_get_cq_event(channel, ...)   → 阻塞等待\n");
    printf("    6. ibv_poll_cq() 循环              → 取出所有 WC\n");
    printf("    7. ibv_ack_cq_events(cq, N)        → 确认事件\n");
    printf("    8. 回到步骤 3 (re-arm)\n\n");
    printf("  忙轮询 vs 事件驱动:\n");
    printf("    忙轮询: 延迟 ~100ns, CPU 100%%\n");
    printf("    事件驱动: 延迟 ~5-10us, CPU 几乎为 0\n");
    printf("    混合模式: 先轮询 N 次, 无果则切换事件等待\n\n");

cleanup:
    printf("[清理] 释放资源...\n");

    /* 如有未确认事件, 必须先确认 */
    if (events_completed > 0 && cq_event) {
        ibv_ack_cq_events(cq_event, events_completed);
    }

    if (qp)       ibv_destroy_qp(qp);
    if (cq_event) ibv_destroy_cq(cq_event);
    if (cq_poll)  ibv_destroy_cq(cq_poll);
    if (channel)  ibv_destroy_comp_channel(channel);
    if (mr)       ibv_dereg_mr(mr);
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  完成\n");
    return 0;
}
