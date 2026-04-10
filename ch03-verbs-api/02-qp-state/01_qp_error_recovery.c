/**
 * QP 错误恢复演示 (Error Recovery)
 *
 * 本程序演示 QP 从 ERROR 状态恢复的完整流程:
 *   1. 创建 QP 并转到 RTS 状态 (loopback)
 *   2. 人为触发 QP 进入 ERROR 状态 (提交错误参数的 WR)
 *   3. 通过 ibv_query_qp() 确认 QP 处于 ERROR 状态
 *   4. 执行恢复: ERROR → RESET → INIT → RTR → RTS
 *   5. 验证 QP 恢复后可以正常工作
 *
 * 编译: gcc -o 01_qp_error_recovery 01_qp_error_recovery.c -I../../common ../../common/librdma_utils.a -libverbs
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
#define CQ_DEPTH    32
#define MSG_SIZE    64

/**
 * 查询并打印 QP 的详细属性
 */
static void query_and_print_qp_detail(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;
    int mask = IBV_QP_STATE | IBV_QP_CUR_STATE | IBV_QP_PKEY_INDEX |
               IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PATH_MTU |
               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN |
               IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
               IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_MAX_DEST_RD_ATOMIC;

    memset(&attr, 0, sizeof(attr));
    memset(&init_attr, 0, sizeof(init_attr));

    if (ibv_query_qp(qp, &attr, mask, &init_attr) != 0) {
        fprintf(stderr, "  ibv_query_qp 失败: %s\n", strerror(errno));
        return;
    }

    printf("  --- QP #%u 详细属性 ---\n", qp->qp_num);
    printf("    状态 (qp_state):       %s (%d)\n",
           qp_state_str(attr.qp_state), attr.qp_state);
    printf("    当前状态 (cur_qp_state): %s (%d)\n",
           qp_state_str(attr.cur_qp_state), attr.cur_qp_state);
    printf("    端口号:                %d\n", attr.port_num);
    printf("    PKey 索引:             %d\n", attr.pkey_index);

    /* 仅在非 RESET 状态打印更多信息 */
    if (attr.qp_state >= IBV_QPS_INIT) {
        printf("    访问标志:              0x%x", attr.qp_access_flags);
        if (attr.qp_access_flags & IBV_ACCESS_LOCAL_WRITE)   printf(" LOCAL_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_READ)   printf(" REMOTE_READ");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_WRITE)  printf(" REMOTE_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_ATOMIC) printf(" REMOTE_ATOMIC");
        printf("\n");
    }
    if (attr.qp_state >= IBV_QPS_RTR) {
        printf("    Path MTU:              %d\n", attr.path_mtu);
        printf("    目标 QP 号:            %u\n", attr.dest_qp_num);
        printf("    RQ PSN:                %u\n", attr.rq_psn);
        printf("    最大目标 RD 原子:      %d\n", attr.max_dest_rd_atomic);
    }
    if (attr.qp_state >= IBV_QPS_RTS) {
        printf("    SQ PSN:                %u\n", attr.sq_psn);
        printf("    超时:                  %d\n", attr.timeout);
        printf("    重试次数:              %d\n", attr.retry_cnt);
        printf("    RNR 重试:              %d\n", attr.rnr_retry);
        printf("    最大 RD 原子:          %d\n", attr.max_rd_atomic);
    }
    printf("  -------------------------\n");
}

int main(int argc, char *argv[])
{
    /* 资源声明 */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_cq      *cq       = NULL;
    struct ibv_qp      *qp       = NULL;
    struct ibv_mr      *mr       = NULL;
    char               *buffer   = NULL;
    int                 num_devices;
    int                 ret;

    printf("============================================\n");
    printf("  QP 错误恢复演示 (Error Recovery)\n");
    printf("============================================\n\n");

    /*
     * QP 状态机中的 ERROR 状态:
     *
     *   RESET → INIT → RTR → RTS → (正常工作)
     *                                  ↓ (错误发生)
     *                                ERROR
     *                                  ↓ (恢复)
     *                                RESET → INIT → RTR → RTS
     *
     * QP 进入 ERROR 状态的常见原因:
     *   1. 远端 QP 被销毁或进入 ERROR
     *   2. CQ 溢出 (overflow)
     *   3. 保护错误 (Protection Error: PD 不匹配, lkey/rkey 错误)
     *   4. 超时重传耗尽 (retry_cnt 超过限制)
     *   5. RNR 重试耗尽
     */

    /* ========== 步骤 1: 初始化所有资源 ========== */
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

    cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq, "创建 CQ 失败");

    buffer = malloc(BUFFER_SIZE);
    CHECK_NULL(buffer, "malloc 失败");
    memset(buffer, 0, BUFFER_SIZE);

    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                     IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(mr, "注册 MR 失败");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
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
    printf("  所有基础资源已就绪\n\n");

    /* ========== 步骤 2: 将 QP 转到 RTS 状态 (loopback) ========== */
    printf("[步骤2] 将 QP 转到 RTS 状态 (loopback 连接)\n");

    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "填充端点信息失败");

    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  传输类型: %s\n", transport_str(transport));

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("  QP 连接失败, 无法继续测试\n");
        goto cleanup;
    }
    printf("  QP 已到达 RTS 状态\n");
    query_and_print_qp_detail(qp);
    printf("\n");

    /* ========== 步骤 3: 验证 QP 正常工作 ========== */
    printf("[步骤3] 验证 QP 正常工作 (Send/Recv loopback)\n");

    /* 先 Post Recv */
    struct ibv_sge recv_sge = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr recv_wr = {
        .wr_id   = 100,
        .sg_list = &recv_sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv = NULL;
    ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
    CHECK_ERRNO(ret, "post_recv 失败");

    /* Post Send */
    snprintf(buffer + MSG_SIZE, MSG_SIZE, "Before Error");
    struct ibv_sge send_sge = {
        .addr   = (uintptr_t)(buffer + MSG_SIZE),
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr send_wr = {
        .wr_id      = 200,
        .sg_list    = &send_sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send = NULL;
    ret = ibv_post_send(qp, &send_wr, &bad_send);
    CHECK_ERRNO(ret, "post_send 失败");

    /* Poll 两个完成事件 (send + recv) */
    struct ibv_wc wc;
    int completed = 0;
    int poll_attempts = 0;
    while (completed < 2 && poll_attempts < 1000000) {
        int ne = ibv_poll_cq(cq, 1, &wc);
        if (ne > 0) {
            printf("  完成事件: wr_id=%lu, status=%s, opcode=%s\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status),
                   wc_opcode_str(wc.opcode));
            completed++;
        }
        poll_attempts++;
    }
    printf("  ✓ QP 正常工作确认 (完成 %d 个操作)\n\n", completed);

    /* ========== 步骤 4: 人为触发 QP 进入 ERROR 状态 ========== */
    printf("[步骤4] 人为触发 QP 进入 ERROR 状态\n");
    printf("========================================\n");

    /*
     * 方法: 使用无效的 rkey 发起 RDMA Write → 产生 Remote Access Error
     * HCA 会将 QP 移到 ERROR 状态。
     *
     * 也可以通过 ibv_modify_qp 直接将 QP 移到 ERROR:
     *   attr.qp_state = IBV_QPS_ERR;
     *   ibv_modify_qp(qp, &attr, IBV_QP_STATE);
     */
    printf("  方法: 发送 RDMA Write 使用无效 rkey (0xDEADBEEF)\n");

    struct ibv_sge bad_sge = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr bad_wr_s = {
        .wr_id      = 999,
        .sg_list    = &bad_sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = (uintptr_t)buffer,
            .rkey        = 0xDEADBEEF,   /* 无效的 rkey! */
        },
    };
    struct ibv_send_wr *bad_wr_ptr = NULL;
    ret = ibv_post_send(qp, &bad_wr_s, &bad_wr_ptr);
    if (ret != 0) {
        printf("  post_send 就失败了: %s\n", strerror(errno));
        printf("  → 尝试直接用 ibv_modify_qp 将 QP 移到 ERROR\n");
        struct ibv_qp_attr err_attr = { .qp_state = IBV_QPS_ERR };
        ret = ibv_modify_qp(qp, &err_attr, IBV_QP_STATE);
        if (ret != 0) {
            printf("  modify_qp 到 ERROR 也失败: %s\n", strerror(errno));
            goto cleanup;
        }
    } else {
        /* 等待错误完成事件 */
        printf("  等待 HCA 处理并报告错误...\n");
        usleep(100000);  /* 100ms */
        while (ibv_poll_cq(cq, 1, &wc) > 0) {
            printf("  CQ: wr_id=%lu, status=%s (%d)\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status), wc.status);
        }
    }

    /* 确认 QP 现在处于 ERROR 状态 */
    printf("\n  查询 QP 状态 (应为 ERROR):\n");
    query_and_print_qp_detail(qp);

    /* ========== 步骤 5: 执行错误恢复 ========== */
    printf("\n[步骤5] 执行 QP 错误恢复: ERROR → RESET → INIT → RTR → RTS\n");
    printf("========================================\n\n");

    /*
     * 恢复步骤:
     * 1. ERROR → RESET: 清除所有状态和未完成的 WR
     * 2. RESET → INIT:  重新设置端口和访问权限
     * 3. INIT → RTR:    重新设置路径参数
     * 4. RTR → RTS:     重新设置发送参数
     *
     * 注意: 重置后, QP 的 qp_num 不变, 但所有之前的 WR 都被丢弃。
     * 如果使用 SRQ, 重置 QP 不会影响 SRQ 中的 WR。
     */

    /* 步骤 5a: ERROR → RESET */
    printf("  5a. ERROR → RESET\n");
    ret = qp_to_reset(qp);
    CHECK_ERRNO(ret, "QP ERROR→RESET 失败");
    printf("      成功!\n");
    query_and_print_qp_detail(qp);

    /* 需要 drain CQ 中所有残留的 WC */
    printf("  清理 CQ 中的残留事件...\n");
    while (ibv_poll_cq(cq, 1, &wc) > 0) {
        printf("      清理: wr_id=%lu, status=%s\n",
               (unsigned long)wc.wr_id, ibv_wc_status_str(wc.status));
    }
    printf("      CQ 已清空\n\n");

    /* 步骤 5b: RESET → INIT → RTR → RTS */
    printf("  5b. RESET → INIT → RTR → RTS (使用 qp_full_connect)\n");

    /* 重新获取端点信息 (qp_num 不变) */
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "重新填充端点信息失败");

    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, access);
    if (ret != 0) {
        printf("      QP 重新连接失败!\n");
        goto cleanup;
    }
    printf("      成功! QP 已恢复到 RTS 状态\n");
    query_and_print_qp_detail(qp);
    printf("\n");

    /* ========== 步骤 6: 验证恢复后 QP 可正常工作 ========== */
    printf("[步骤6] 验证恢复后 QP 可正常工作\n");
    printf("========================================\n");

    /* 再次执行 Send/Recv */
    struct ibv_sge recv_sge2 = {
        .addr   = (uintptr_t)buffer,
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_recv_wr recv_wr2 = {
        .wr_id   = 300,
        .sg_list = &recv_sge2,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_recv2 = NULL;
    ret = ibv_post_recv(qp, &recv_wr2, &bad_recv2);
    CHECK_ERRNO(ret, "恢复后 post_recv 失败");

    snprintf(buffer + MSG_SIZE, MSG_SIZE, "After Recovery!");
    struct ibv_sge send_sge2 = {
        .addr   = (uintptr_t)(buffer + MSG_SIZE),
        .length = MSG_SIZE,
        .lkey   = mr->lkey,
    };
    struct ibv_send_wr send_wr2 = {
        .wr_id      = 400,
        .sg_list    = &send_sge2,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_send2 = NULL;
    ret = ibv_post_send(qp, &send_wr2, &bad_send2);
    CHECK_ERRNO(ret, "恢复后 post_send 失败");

    /* Poll 完成事件 */
    completed = 0;
    poll_attempts = 0;
    while (completed < 2 && poll_attempts < 1000000) {
        int ne = ibv_poll_cq(cq, 1, &wc);
        if (ne > 0) {
            printf("  完成事件: wr_id=%lu, status=%s, opcode=%s\n",
                   (unsigned long)wc.wr_id,
                   ibv_wc_status_str(wc.status),
                   wc_opcode_str(wc.opcode));
            if (wc.status == IBV_WC_SUCCESS) completed++;
        }
        poll_attempts++;
    }

    if (completed >= 2) {
        printf("  ✓ QP 恢复成功! 可以正常 Send/Recv\n");
        printf("  接收到的数据: \"%s\"\n", buffer);
    } else {
        printf("  ✗ 恢复后操作未完成 (completed=%d)\n", completed);
    }

    /* ========== 总结 ========== */
    printf("\n============================================\n");
    printf("  QP 错误恢复总结\n");
    printf("============================================\n");
    printf("  恢复路径: ERROR → RESET → INIT → RTR → RTS\n\n");
    printf("  注意事项:\n");
    printf("  1. 重置前需清空 CQ 中所有残留的 WC\n");
    printf("  2. QP number 保持不变, 但对端可能需要通知\n");
    printf("  3. 所有未完成的 WR 在 RESET 时被丢弃\n");
    printf("  4. 对端 QP 可能也进入了 ERROR (需同时恢复)\n");
    printf("  5. 最佳实践: 双端协商后同时重建连接\n");
    printf("  6. 某些错误 (如硬件故障) 可能无法通过重置恢复\n\n");

cleanup:
    printf("[清理] 释放资源...\n");
    if (qp)       ibv_destroy_qp(qp);
    if (cq)       ibv_destroy_cq(cq);
    if (mr)       ibv_dereg_mr(mr);
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  完成\n");
    return 0;
}
