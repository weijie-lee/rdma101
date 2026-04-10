/**
 * Inline Data 优化对比实验
 *
 * 演示 Inline Data 优化技术：
 *   - 查询设备的 max_inline_data 能力
 *   - 对比普通 DMA 发送 vs Inline 发送的性能差异
 *   - Loopback 模式运行，无需两台机器
 *
 * 原理：
 *   普通发送路径: CPU 填写 WQE(含数据地址) → NIC 通过 DMA 读取数据 → NIC 发送
 *   Inline 路径:  CPU 将数据拷贝到 WQE 中   → NIC 直接从 WQE 取数据发送 (省一次 DMA)
 *
 *   对于小消息 (< max_inline_data)，Inline 可以显著降低延迟，因为：
 *   1. 少一次 PCIe DMA 读取往返
 *   2. 不需要 lkey (数据已在 WQE 中)
 *
 * 编译:
 *   gcc -o inline_data inline_data.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * 运行: ./inline_data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */
#define MSG_SIZE        32          /* 消息大小 (字节), 适合 inline */
#define NUM_MESSAGES    1000        /* 每轮测试发送的消息数 */
#define BUFFER_SIZE     4096        /* 缓冲区大小 */
#define CQ_DEPTH        256         /* CQ 深度 */
#define MAX_SEND_WR     128         /* SQ 最大 WR 数 */
#define MAX_RECV_WR     128         /* RQ 最大 WR 数 */

/* ========== 获取高精度时间 (纳秒) ========== */
static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ========== 批量发布 recv WR ========== */
static int post_recvs(struct ibv_qp *qp, struct ibv_mr *mr,
                      char *buf, int count)
{
    struct ibv_recv_wr wr, *bad_wr;
    struct ibv_sge sge;
    int i, ret;

    /* 为每条消息发布一个 recv WR */
    for (i = 0; i < count; i++) {
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)(buf + (i % 4) * MSG_SIZE);
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;

        memset(&wr, 0, sizeof(wr));
        wr.wr_id   = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.next    = NULL;

        ret = ibv_post_recv(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] post_recv #%d 失败: errno=%d\n", i, ret);
            return -1;
        }
    }
    return 0;
}

/* ========== 普通发送 (DMA 路径) ========== */
static int send_normal(struct ibv_qp *qp, struct ibv_cq *cq,
                       struct ibv_mr *mr, char *buf, int count)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        /* 构建 SGE: 指向已注册的内存区域 */
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = mr->lkey;      /* 普通发送需要 lkey */

        /* 构建 Send WR: 不使用 IBV_SEND_INLINE */
        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;  /* 每条都产生 CQE */

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] 普通 post_send #%d 失败: errno=%d\n", i, ret);
            return -1;
        }

        /* 等待发送完成 */
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) return -1;
    }
    return 0;
}

/* ========== Inline 发送 (数据嵌入 WQE) ========== */
static int send_inline(struct ibv_qp *qp, struct ibv_cq *cq,
                       char *buf, int count)
{
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_sge sge;
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        /*
         * Inline 发送: sge 中的数据会被直接拷贝到 WQE
         * 注意: lkey 可以设为 0，因为 NIC 不需要 DMA 读取数据
         *       但实践中设置 lkey 也不影响正确性
         */
        memset(&sge, 0, sizeof(sge));
        sge.addr   = (uint64_t)buf;
        sge.length = MSG_SIZE;
        sge.lkey   = 0;            /* Inline 模式不需要 lkey! */

        /* 构建 Send WR: 使用 IBV_SEND_INLINE 标志 */
        memset(&wr, 0, sizeof(wr));
        wr.wr_id      = i;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;  /* 关键! */

        ret = ibv_post_send(qp, &wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "[错误] inline post_send #%d 失败: errno=%d\n", i, ret);
            return -1;
        }

        /* 等待发送完成 */
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) return -1;
    }
    return 0;
}

/* ========== 消费接收侧 CQ ========== */
static int drain_recv_cq(struct ibv_cq *cq, int count)
{
    struct ibv_wc wc;
    int i, ret;

    for (i = 0; i < count; i++) {
        ret = poll_cq_blocking(cq, &wc);
        if (ret != 0) {
            fprintf(stderr, "[错误] drain_recv_cq #%d 失败\n", i);
            return -1;
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

    printf("=== Inline Data 优化对比实验 ===\n\n");

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

    /* ---- 步骤 2: 查询设备 inline 能力 ---- */
    struct ibv_device_attr dev_attr;
    ret = ibv_query_device(ctx, &dev_attr);
    CHECK_ERRNO(ret, "查询设备属性失败");
    /*
     * 注意: ibv_device_attr 中没有直接的 max_inline_data 字段。
     * max_inline_data 实际由驱动在 QP 创建时决定，
     * 可以在 ibv_create_qp 的 cap 中设置请求值，
     * 创建后 cap 中会返回实际支持的值。
     */
    printf("[2] 设备能力: max_qp_wr=%d, max_sge=%d\n",
           dev_attr.max_qp_wr, dev_attr.max_sge);

    /* ---- 步骤 3: 分配资源 ---- */
    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "分配 PD 失败");

    send_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(send_cq, "创建 send_cq 失败");

    recv_cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(recv_cq, "创建 recv_cq 失败");

    /* 分配缓冲区 */
    send_buf = malloc(BUFFER_SIZE);
    recv_buf = malloc(BUFFER_SIZE);
    CHECK_NULL(send_buf, "分配 send_buf 失败");
    CHECK_NULL(recv_buf, "分配 recv_buf 失败");
    memset(send_buf, 'A', BUFFER_SIZE);
    memset(recv_buf, 0, BUFFER_SIZE);

    /* 注册内存 */
    send_mr = ibv_reg_mr(pd, send_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(send_mr, "注册 send_mr 失败");
    recv_mr = ibv_reg_mr(pd, recv_buf, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(recv_mr, "注册 recv_mr 失败");

    /* ---- 步骤 4: 创建 QP (带 inline data) ---- */
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = send_cq;
    qp_init_attr.recv_cq = recv_cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = MAX_SEND_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_RECV_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = 256;  /* 请求 256 字节 inline */

    qp = ibv_create_qp(pd, &qp_init_attr);
    CHECK_NULL(qp, "创建 QP 失败");

    /*
     * 创建后 qp_init_attr.cap.max_inline_data 会被更新为实际值
     * 不同硬件/驱动支持的 max_inline_data 不同
     */
    printf("[3] 创建 QP #%u\n", qp->qp_num);
    printf("    请求 max_inline_data = 256\n");
    printf("    实际 max_inline_data = %u\n", qp_init_attr.cap.max_inline_data);

    if (qp_init_attr.cap.max_inline_data < MSG_SIZE) {
        fprintf(stderr, "[警告] 设备不支持 %d 字节 inline, 最大 %u 字节\n",
                MSG_SIZE, qp_init_attr.cap.max_inline_data);
        fprintf(stderr, "        将跳过 inline 测试\n");
    }

    /* ---- 步骤 5: Loopback 建连 ---- */
    printf("[4] Loopback 建连...\n");
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, RDMA_DEFAULT_PORT_NUM,
                              RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "填充本地端点信息失败");

    /* Loopback: 远端就是自己 */
    ret = qp_full_connect(qp, &local_ep, RDMA_DEFAULT_PORT_NUM,
                          is_roce, IBV_ACCESS_LOCAL_WRITE);
    CHECK_ERRNO(ret, "QP 建连失败");
    print_qp_state(qp);

    /* ---- 步骤 6: 普通发送测试 ---- */
    printf("\n===== 测试 1: 普通发送 (DMA 路径) =====\n");
    printf("  消息大小: %d 字节, 消息数: %d\n", MSG_SIZE, NUM_MESSAGES);

    /* 先发布所有 recv */
    ret = post_recvs(qp, recv_mr, recv_buf, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    uint64_t t1 = get_time_ns();
    ret = send_normal(qp, send_cq, send_mr, send_buf, NUM_MESSAGES);
    uint64_t t2 = get_time_ns();
    if (ret != 0) goto cleanup;

    /* 消费 recv CQ */
    ret = drain_recv_cq(recv_cq, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    double normal_us = (double)(t2 - t1) / 1000.0;
    double normal_per_msg = normal_us / NUM_MESSAGES;
    printf("  总耗时: %.2f μs\n", normal_us);
    printf("  平均每消息: %.3f μs\n", normal_per_msg);

    /* ---- 步骤 7: Inline 发送测试 ---- */
    printf("\n===== 测试 2: Inline 发送 (数据嵌入 WQE) =====\n");
    printf("  消息大小: %d 字节, 消息数: %d\n", MSG_SIZE, NUM_MESSAGES);
    printf("  IBV_SEND_INLINE 标志: 已启用\n");
    printf("  lkey: 不需要 (数据直接在 WQE 中)\n");

    /* 再次发布 recv */
    ret = post_recvs(qp, recv_mr, recv_buf, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    uint64_t t3 = get_time_ns();
    ret = send_inline(qp, send_cq, send_buf, NUM_MESSAGES);
    uint64_t t4 = get_time_ns();
    if (ret != 0) goto cleanup;

    /* 消费 recv CQ */
    ret = drain_recv_cq(recv_cq, NUM_MESSAGES);
    if (ret != 0) goto cleanup;

    double inline_us = (double)(t4 - t3) / 1000.0;
    double inline_per_msg = inline_us / NUM_MESSAGES;
    printf("  总耗时: %.2f μs\n", inline_us);
    printf("  平均每消息: %.3f μs\n", inline_per_msg);

    /* ---- 结果对比 ---- */
    printf("\n===== 性能对比 =====\n");
    printf("  ┌──────────────┬───────────────┬──────────────┐\n");
    printf("  │ 模式         │ 总耗时 (μs)   │ 每消息 (μs)  │\n");
    printf("  ├──────────────┼───────────────┼──────────────┤\n");
    printf("  │ 普通 (DMA)   │ %13.2f │ %12.3f │\n", normal_us, normal_per_msg);
    printf("  │ Inline       │ %13.2f │ %12.3f │\n", inline_us, inline_per_msg);
    printf("  └──────────────┴───────────────┴──────────────┘\n");

    if (inline_us < normal_us) {
        double speedup = normal_us / inline_us;
        printf("\n  结论: Inline 模式快 %.2fx\n", speedup);
    } else {
        printf("\n  结论: 在当前环境下 Inline 未体现明显优势\n");
        printf("  提示: SoftRoCE 模拟环境可能无法真实反映 Inline 优化效果\n");
        printf("        在真实硬件 (ConnectX 等) 上差异会更明显\n");
    }

    printf("\n===== 原理总结 =====\n");
    printf("  1. Inline Data 将小消息数据直接拷贝到 WQE (Work Queue Element) 中\n");
    printf("  2. NIC 从 WQE 直接获取数据，跳过 DMA 读取步骤\n");
    printf("  3. 省去一次 PCIe 往返，降低小消息延迟\n");
    printf("  4. Inline 发送不需要 lkey (数据不再位于 MR 中)\n");
    printf("  5. 适用于消息大小 < max_inline_data 的场景\n");
    printf("  6. 不适用于大消息 (数据量超过 WQE 容量)\n");

cleanup:
    /* 逆序释放资源 */
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
