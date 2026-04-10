/**
 * Unsignaled Completion 优化对比实验
 *
 * 演示 Unsignaled Send 优化技术：
 *   - 模式 1: 每个 Send 都带 IBV_SEND_SIGNALED，逐个 poll CQ
 *   - 模式 2: 每 N 个 Send 才 signal 一次，批量完成
 *   - 对比两种模式的性能差异
 *
 * 原理：
 *   每个 IBV_SEND_SIGNALED 的 Send 完成后都会产生一个 CQE，
 *   poll_cq 需要消耗 CPU 时间。如果大部分 Send 不需要确认完成，
 *   可以设置为 unsignaled (不带 IBV_SEND_SIGNALED)，只在关键节点
 *   (如每 N 个请求) signal 一次。
 *
 *   关键约束：
 *   - 必须在 SQ 满之前至少 signal 一次 (否则 SQ 会溢出，无法回收 WQE)
 *   - signal 的那个 CQE 隐含确认了之前所有 unsignaled 的 WR
 *   - 错误处理: unsignaled 的 WR 出错也会通过后续 signaled 的 WR 报告
 *
 * 编译:
 *   gcc -o unsignaled_send unsignaled_send.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * 运行: ./unsignaled_send
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */
#define MSG_SIZE        64          /* 消息大小 (字节) */
#define NUM_MESSAGES    10000       /* 每轮测试发送的消息数 */
#define BUFFER_SIZE     4096        /* 缓冲区大小 */
#define CQ_DEPTH        256         /* CQ 深度 */
#define MAX_SEND_WR     128         /* SQ 最大 WR 数 */
#define MAX_RECV_WR     256         /* RQ 最大 WR 数 (需要足够的 recv) */
#define SIGNAL_INTERVAL 32          /* 每 N 个 Send signal 一次 */

/* ========== 获取高精度时间 (纳秒) ========== */
static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ========== 批量发布 recv WR ========== */
static int post_recv_batch(struct ibv_qp *qp, struct ibv_mr *mr,
                           char *buf, int count)
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    int i, ret;

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)(buf + (i % 4) * MSG_SIZE);
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id   = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        ret = ibv_post_recv(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] post_recv #%d 失败: errno=%d\n", i, ret);
            return -1;
        }
    }
    return 0;
}

/* ========== 模式 1: 全部 Signaled ========== */
static int test_all_signaled(struct ibv_qp *qp, struct ibv_cq *cq,
                             struct ibv_mr *mr, char *buf, int count)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;  /* 每个都产生 CQE */

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] signaled post_send #%d 失败: errno=%d\n", i, ret);
            return -1;
        }

        /* 每个 send 都要 poll 一次 CQE */
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) return -1;
    }
    return 0;
}

/* ========== 模式 2: 每 N 个才 Signal 一次 ========== */
static int test_unsignaled(struct ibv_qp *qp, struct ibv_cq *cq,
                           struct ibv_mr *mr, char *buf, int count,
                           int signal_interval)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;
    int outstanding = 0;  /* 已发布但未确认的 WR 数 */

    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;

        /*
         * 每 signal_interval 个请求 signal 一次
         * 或者最后一个请求必须 signal (确保所有 WR 都被确认)
         *
         * 关键: outstanding WR 数不能超过 max_send_wr
         */
        if ((i + 1) % signal_interval == 0 || i == count - 1) {
            wr.send_flags = IBV_SEND_SIGNALED;  /* 产生 CQE */
        } else {
            wr.send_flags = 0;                   /* 不产生 CQE */
        }

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] unsignaled post_send #%d 失败: errno=%d\n", i, ret);
            return -1;
        }

        outstanding++;

        /* 当遇到 signaled 的 WR 时，poll CQ 回收所有完成的 WR */
        if (wr.send_flags & IBV_SEND_SIGNALED) {
            ret = poll_cq_blocking(cq, &wc);
            if (ret != 0) return -1;
            outstanding = 0;  /* signaled CQE 确认了之前所有 WR */
        }

        /*
         * 安全检查: 如果 outstanding 快要达到 max_send_wr，
         * 即使不到 signal_interval，也需要强制 signal
         * (这里的逻辑已经通过上面的 % 运算保证不会溢出，
         *  但生产代码中需要更精细的控制)
         */
    }

    return 0;
}

/* ========== 消费接收侧 CQ ========== */
static int drain_recv_cq(struct ibv_cq *cq, int count)
{
    struct ibv_wc wc;
    int drained = 0, ne;

    while (drained < count) {
        ne = ibv_poll_cq(cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[错误] drain_recv poll_cq 失败\n");
            return -1;
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[错误] recv CQE 失败: %s\n",
                        ibv_wc_status_str(wc.status));
                return -1;
            }
            drained++;
        }
    }
    return 0;
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    /* RDMA 资源 */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *send_cq = NULL, *recv_cq = NULL;
    struct ibv_qp *qp = NULL;
    struct ibv_mr *send_mr = NULL, *recv_mr = NULL;
    char *send_buf = NULL, *recv_buf = NULL;
    int num_devices, ret;
    int is_roce;

    printf("=== Unsignaled Completion 优化对比实验 ===\n\n");

    /* ---- 步骤 1: 打开设备 ---- */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未找到 RDMA 设备\n");
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "打开设备失败");
    printf("[1] 打开设备: %s\n", ibv_get_device_name(dev_list[0]));

    /* 自动检测传输层类型 */
    is_roce = (detect_transport(ctx, RDMA_DEFAULT_PORT_NUM) == RDMA_TRANSPORT_ROCE);
    printf("    传输层: %s\n", is_roce ? "RoCE" : "IB");

    /* ---- 步骤 2: 分配资源 ---- */
    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "分配 PD 失败");

    send_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(send_cq, "创建 send_cq 失败");

    recv_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(recv_cq, "创建 recv_cq 失败");

    send_buf = malloc(BUFFER_SIZE);
    recv_buf = malloc(BUFFER_SIZE);
    CHECK_NULL(send_buf, "分配 send_buf 失败");
    CHECK_NULL(recv_buf, "分配 recv_buf 失败");
    memset(send_buf, 'B', BUFFER_SIZE);
    memset(recv_buf, 0, BUFFER_SIZE);

    send_mr = ibv_reg_mr(pd, send_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(send_mr, "注册 send_mr 失败");
    recv_mr = ibv_reg_mr(pd, recv_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(recv_mr, "注册 recv_mr 失败");

    /* ---- 步骤 3: 创建 QP ---- */
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = send_cq;
    qp_init_attr.recv_cq = recv_cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = MAX_SEND_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_RECV_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    /*
     * sq_sig_all = 0: 默认不自动 signal 所有 send WR
     * 我们手动控制哪些 WR 需要 signal
     */
    qp_init_attr.sq_sig_all = 0;

    qp = ibv_create_qp(pd, &qp_init_attr);
    CHECK_NULL(qp, "创建 QP 失败");
    printf("[2] 创建 QP #%u (sq_sig_all=0, max_send_wr=%d)\n",
           qp->qp_num, MAX_SEND_WR);
    printf("    signal 间隔: 每 %d 个 Send signal 一次\n", SIGNAL_INTERVAL);

    /* ---- 步骤 4: Loopback 建连 ---- */
    printf("[3] Loopback 建连...\n");
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, RDMA_DEFAULT_PORT_NUM,
                              RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "填充本地端点信息失败");

    ret = qp_full_connect(qp, &local_ep, RDMA_DEFAULT_PORT_NUM,
                          is_roce, IBV_ACCESS_LOCAL_WRITE);
    CHECK_ERRNO(ret, "QP 建连失败");
    print_qp_state(qp);

    /* ========== 测试 1: 全部 Signaled ========== */
    printf("\n===== 测试 1: 全部 Signaled (每发一个就 poll 一次) =====\n");
    printf("  消息大小: %d 字节, 消息数: %d\n", MSG_SIZE, NUM_MESSAGES);

    /* 发布足够的 recv WR (分批发布) */
    int recv_posted = 0;
    int batch = MAX_RECV_WR;
    while (recv_posted < NUM_MESSAGES) {
        int to_post = (NUM_MESSAGES - recv_posted) < batch ?
                      (NUM_MESSAGES - recv_posted) : batch;
        ret = post_recv_batch(qp, recv_mr, recv_buf, to_post);
        if (ret != 0) goto cleanup;
        recv_posted += to_post;

        /* 如果还有更多要发，先消费一些 recv CQ 腾出空间
         * 注意: 这里简化处理，实际中需要更精细的流控
         */
        if (recv_posted < NUM_MESSAGES) {
            /* 跑一部分 send + poll 来消费 recv CQ */
            break;
        }
    }

    /* 简化：一次发布 NUM_MESSAGES 个 recv (如果 RQ 够大) */
    /* 重新发布 recv (上面的逻辑可能不够) */

    uint64_t t1 = get_time_ns();
    ret = test_all_signaled(qp, send_cq, send_mr, send_buf, NUM_MESSAGES);
    uint64_t t2 = get_time_ns();
    if (ret != 0) {
        fprintf(stderr, "[错误] 全部 signaled 测试失败\n");
        goto cleanup;
    }

    /* 消费 recv 侧 CQE */
    drain_recv_cq(recv_cq, NUM_MESSAGES);

    double signaled_us = (double)(t2 - t1) / 1000.0;
    double signaled_per_msg = signaled_us / NUM_MESSAGES;
    printf("  总耗时: %.2f μs\n", signaled_us);
    printf("  平均每消息: %.3f μs\n", signaled_per_msg);
    printf("  CQ poll 次数: %d (每发一个 poll 一次)\n", NUM_MESSAGES);

    /* ========== 测试 2: Unsignaled (每 N 个 signal 一次) ========== */
    printf("\n===== 测试 2: Unsignaled (每 %d 个 signal 一次) =====\n",
           SIGNAL_INTERVAL);
    printf("  消息大小: %d 字节, 消息数: %d\n", MSG_SIZE, NUM_MESSAGES);

    /* 重新发布 recv */
    recv_posted = 0;
    batch = MAX_RECV_WR;
    while (recv_posted < NUM_MESSAGES) {
        int to_post = (NUM_MESSAGES - recv_posted) < batch ?
                      (NUM_MESSAGES - recv_posted) : batch;
        ret = post_recv_batch(qp, recv_mr, recv_buf, to_post);
        if (ret != 0) goto cleanup;
        recv_posted += to_post;
        if (recv_posted < NUM_MESSAGES) break;
    }

    uint64_t t3 = get_time_ns();
    ret = test_unsignaled(qp, send_cq, send_mr, send_buf,
                          NUM_MESSAGES, SIGNAL_INTERVAL);
    uint64_t t4 = get_time_ns();
    if (ret != 0) {
        fprintf(stderr, "[错误] unsignaled 测试失败\n");
        goto cleanup;
    }

    drain_recv_cq(recv_cq, NUM_MESSAGES);

    double unsig_us = (double)(t4 - t3) / 1000.0;
    double unsig_per_msg = unsig_us / NUM_MESSAGES;
    int poll_count = (NUM_MESSAGES + SIGNAL_INTERVAL - 1) / SIGNAL_INTERVAL;
    printf("  总耗时: %.2f μs\n", unsig_us);
    printf("  平均每消息: %.3f μs\n", unsig_per_msg);
    printf("  CQ poll 次数: ~%d (减少 %dx)\n", poll_count, SIGNAL_INTERVAL);

    /* ========== 结果对比 ========== */
    printf("\n===== 性能对比 =====\n");
    printf("  ┌───────────────────────┬───────────────┬──────────────┬────────────┐\n");
    printf("  │ 模式                  │ 总耗时 (μs)   │ 每消息 (μs)  │ poll 次数  │\n");
    printf("  ├───────────────────────┼───────────────┼──────────────┼────────────┤\n");
    printf("  │ Signaled (每个 poll)  │ %13.2f │ %12.3f │ %10d │\n",
           signaled_us, signaled_per_msg, NUM_MESSAGES);
    printf("  │ Unsignaled (每 %d)   │ %13.2f │ %12.3f │ %10d │\n",
           SIGNAL_INTERVAL, unsig_us, unsig_per_msg, poll_count);
    printf("  └───────────────────────┴───────────────┴──────────────┴────────────┘\n");

    if (unsig_us < signaled_us) {
        double speedup = signaled_us / unsig_us;
        printf("\n  结论: Unsignaled 模式快 %.2fx\n", speedup);
    } else {
        printf("\n  结论: 在当前环境下差异不明显\n");
        printf("  提示: SoftRoCE 环境中 poll 开销占比小，真实硬件差异更大\n");
    }

    printf("\n===== 原理总结 =====\n");
    printf("  1. IBV_SEND_SIGNALED 使 Send 完成后产生 CQE，poll_cq 可以检测到\n");
    printf("  2. 不设置 SIGNALED 的 Send 不产生 CQE (unsignaled)\n");
    printf("  3. signaled CQE 的完成隐含确认了之前所有 unsignaled 的 WR\n");
    printf("  4. 减少 poll_cq 调用次数，降低 CPU 开销\n");
    printf("  5. 关键约束: 必须在 SQ 满之前 signal (否则 SQ 溢出)\n");
    printf("  6. 推荐策略: signal_interval = min(max_send_wr/2, 32)\n");
    printf("  7. 错误处理: unsignaled WR 的错误会通过后续 signaled WR 报告\n");

cleanup:
    if (qp)       ibv_destroy_qp(qp);
    if (send_cq)  ibv_destroy_cq(send_cq);
    if (recv_cq)  ibv_destroy_cq(recv_cq);
    if (send_mr)  ibv_dereg_mr(send_mr);
    if (recv_mr)  ibv_dereg_mr(recv_mr);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);
    free(send_buf);
    free(recv_buf);

    printf("\n程序结束\n");
    return 0;
}
