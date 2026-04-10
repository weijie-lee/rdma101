/**
 * RDMA 错误触发与恢复演示
 *
 * 故意触发 4 种典型 RDMA 错误，展示：
 *   - 错误的 WC 状态码
 *   - 详细的中文错误诊断信息
 *   - QP 错误恢复流程 (ERROR → RESET → INIT → RTR → RTS)
 *
 * 触发的错误场景：
 *   a) IBV_WC_REM_ACCESS_ERR: 使用错误的 rkey 执行 RDMA Write
 *   b) IBV_WC_LOC_PROT_ERR:  使用不同 PD 的 lkey
 *   c) IBV_WC_WR_FLUSH_ERR:  在 ERROR 状态的 QP 上 post send
 *   d) IBV_WC_RNR_RETRY_EXC_ERR: 不 post recv 就发送 (rnr_retry=0)
 *
 * 编译:
 *   gcc -o trigger_errors trigger_errors.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * 运行: ./trigger_errors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */
#define BUFFER_SIZE     4096
#define CQ_DEPTH        64
#define MAX_SEND_WR     64
#define MAX_RECV_WR     64

/* ========== 全局 RDMA 资源 ========== */
struct test_resources {
    struct ibv_device    **dev_list;
    struct ibv_context   *ctx;
    struct ibv_pd        *pd;
    struct ibv_pd        *pd2;       /* 第二个 PD (用于触发 LOC_PROT_ERR) */
    struct ibv_cq        *send_cq;
    struct ibv_cq        *recv_cq;
    struct ibv_qp        *qp;
    struct ibv_mr        *mr;
    struct ibv_mr        *mr2;       /* 使用 pd2 注册的 MR (用于错误触发) */
    char                 *buf;
    char                 *buf2;
    int                   is_roce;
    struct rdma_endpoint  local_ep;
};

/* ========== 打印错误诊断信息 ========== */
static void print_error_diagnosis(int status)
{
    printf("\n  ┌─ 错误诊断 ────────────────────────────────────────────┐\n");

    switch (status) {
    case IBV_WC_SUCCESS:
        printf("  │ 状态: IBV_WC_SUCCESS (0)                              │\n");
        printf("  │ 含义: 操作成功完成                                    │\n");
        break;

    case IBV_WC_REM_ACCESS_ERR:
        printf("  │ 状态: IBV_WC_REM_ACCESS_ERR (5)                       │\n");
        printf("  │ 含义: 远端访问权限错误                                │\n");
        printf("  │ 原因:                                                 │\n");
        printf("  │   1. rkey 错误或已失效                                │\n");
        printf("  │   2. 目标地址超出 MR 注册范围                          │\n");
        printf("  │   3. MR 没有 REMOTE_WRITE/READ 权限                   │\n");
        printf("  │ 修复:                                                 │\n");
        printf("  │   - 检查 rkey 和远端地址是否正确交换                   │\n");
        printf("  │   - 确认 MR 注册时包含 IBV_ACCESS_REMOTE_WRITE 等      │\n");
        break;

    case IBV_WC_LOC_PROT_ERR:
        printf("  │ 状态: IBV_WC_LOC_PROT_ERR (4)                        │\n");
        printf("  │ 含义: 本地保护域错误                                  │\n");
        printf("  │ 原因:                                                 │\n");
        printf("  │   1. lkey 对应的 MR 与 QP 不在同一个 PD                │\n");
        printf("  │   2. lkey 无效或已被 deregister                        │\n");
        printf("  │   3. SGE 地址超出 MR 注册范围                          │\n");
        printf("  │ 修复:                                                 │\n");
        printf("  │   - 确认 QP 和 MR 属于同一个 PD                        │\n");
        printf("  │   - 检查 lkey 是否正确                                 │\n");
        break;

    case IBV_WC_WR_FLUSH_ERR:
        printf("  │ 状态: IBV_WC_WR_FLUSH_ERR (5)                        │\n");
        printf("  │ 含义: WR 被刷出 (QP 已进入 ERROR 状态)                 │\n");
        printf("  │ 原因:                                                 │\n");
        printf("  │   1. QP 因为之前的错误已进入 ERROR 状态                │\n");
        printf("  │   2. 所有待处理的 WR 都会被标记为 FLUSH_ERR             │\n");
        printf("  │ 修复:                                                 │\n");
        printf("  │   - 先修复导致 QP ERROR 的根本原因                     │\n");
        printf("  │   - 恢复 QP: ERROR → RESET → INIT → RTR → RTS          │\n");
        break;

    case IBV_WC_RNR_RETRY_EXC_ERR:
        printf("  │ 状态: IBV_WC_RNR_RETRY_EXC_ERR (11)                  │\n");
        printf("  │ 含义: RNR (Receiver Not Ready) 重试超限                │\n");
        printf("  │ 原因:                                                 │\n");
        printf("  │   1. 对端没有 post recv 就发来了消息                    │\n");
        printf("  │   2. 对端的 recv WR 已被消耗完                         │\n");
        printf("  │   3. rnr_retry 设置过低 (0=不重试)                     │\n");
        printf("  │ 修复:                                                 │\n");
        printf("  │   - 确保对端先 post_recv 再发送                        │\n");
        printf("  │   - 设置合理的 rnr_retry (推荐 7=无限重试)              │\n");
        break;

    default:
        printf("  │ 状态: %s (%d)\n", ibv_wc_status_str(status), status);
        printf("  │ 含义: 请参考 error_diagnosis 工具查询详情               │\n");
        break;
    }

    printf("  └──────────────────────────────────────────────────────┘\n");
}

/* ========== 初始化资源 ========== */
static int init_resources(struct test_resources *res)
{
    int num_devices, ret;

    memset(res, 0, sizeof(*res));

    /* 打开设备 */
    res->dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(res->dev_list, "获取设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未找到 RDMA 设备\n");
        goto cleanup;
    }

    res->ctx = ibv_open_device(res->dev_list[0]);
    CHECK_NULL(res->ctx, "打开设备失败");
    printf("[初始化] 设备: %s\n", ibv_get_device_name(res->dev_list[0]));

    res->is_roce = (detect_transport(res->ctx, RDMA_DEFAULT_PORT_NUM)
                    == RDMA_TRANSPORT_ROCE);
    printf("[初始化] 传输层: %s\n", res->is_roce ? "RoCE" : "IB");

    /* 分配两个 PD (一个正常, 一个用于触发 LOC_PROT_ERR) */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配 PD 失败");

    res->pd2 = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd2, "分配 PD2 失败");

    /* CQ */
    res->send_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->send_cq, "创建 send_cq 失败");
    res->recv_cq = ibv_create_cq(res->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(res->recv_cq, "创建 recv_cq 失败");

    /* 缓冲区和 MR */
    res->buf = calloc(1, BUFFER_SIZE);
    res->buf2 = calloc(1, BUFFER_SIZE);
    CHECK_NULL(res->buf, "分配 buf 失败");
    CHECK_NULL(res->buf2, "分配 buf2 失败");

    /* mr 属于 pd (与 QP 同一个 PD) */
    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    CHECK_NULL(res->mr, "注册 MR 失败");

    /* mr2 属于 pd2 (不同的 PD, 用于触发保护域错误) */
    res->mr2 = ibv_reg_mr(res->pd2, res->buf2, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->mr2, "注册 MR2 失败");

    printf("[初始化] 资源创建完毕 (PD×2, CQ×2, MR×2)\n");
    return 0;

cleanup:
    return -1;
}

/* ========== 创建并建连 QP (loopback) ========== */
static int create_and_connect_qp(struct test_resources *res, int rnr_retry)
{
    struct ibv_qp_init_attr qp_attr;
    int ret;

    /* 如果已有旧 QP 则销毁 */
    if (res->qp) {
        ibv_destroy_qp(res->qp);
        res->qp = NULL;
    }

    /* 创建 QP */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->send_cq;
    qp_attr.recv_cq = res->recv_cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = MAX_SEND_WR;
    qp_attr.cap.max_recv_wr  = MAX_RECV_WR;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "创建 QP 失败");

    /* 填充端点 */
    ret = fill_local_endpoint(res->ctx, res->qp, RDMA_DEFAULT_PORT_NUM,
                              RDMA_DEFAULT_GID_INDEX, &res->local_ep);
    CHECK_ERRNO(ret, "填充端点失败");

    /* 填充远端 MR 信息 (loopback 用自己的) */
    res->local_ep.buf_addr = (uint64_t)res->buf;
    res->local_ep.buf_rkey = res->mr->rkey;

    /* 手动建连 (用于自定义 rnr_retry) */
    ret = qp_to_init(res->qp, RDMA_DEFAULT_PORT_NUM,
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                     IBV_ACCESS_REMOTE_READ);
    CHECK_ERRNO(ret, "QP → INIT 失败");

    ret = qp_to_rtr(res->qp, &res->local_ep, RDMA_DEFAULT_PORT_NUM,
                    res->is_roce);
    CHECK_ERRNO(ret, "QP → RTR 失败");

    /* 自定义 RTS 参数 (特别是 rnr_retry) */
    {
        struct ibv_qp_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state      = IBV_QPS_RTS;
        attr.sq_psn         = RDMA_DEFAULT_PSN;
        attr.timeout        = 14;
        attr.retry_cnt      = 7;
        attr.rnr_retry      = rnr_retry;   /* 可自定义 */
        attr.max_rd_atomic  = 1;

        ret = ibv_modify_qp(res->qp, &attr,
                            IBV_QP_STATE | IBV_QP_SQ_PSN |
                            IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
        CHECK_ERRNO(ret, "QP → RTS 失败");
    }

    return 0;

cleanup:
    return -1;
}

/* ========== 恢复 QP: ERROR → RESET → INIT → RTR → RTS ========== */
static int recover_qp(struct test_resources *res)
{
    int ret;

    printf("\n  [恢复] QP 状态恢复流程:\n");
    print_qp_state(res->qp);

    printf("  [恢复] ERROR → RESET\n");
    ret = qp_to_reset(res->qp);
    if (ret != 0) {
        fprintf(stderr, "  [恢复] → RESET 失败\n");
        return -1;
    }
    print_qp_state(res->qp);

    printf("  [恢复] RESET → INIT → RTR → RTS\n");
    ret = qp_full_connect(res->qp, &res->local_ep, RDMA_DEFAULT_PORT_NUM,
                          res->is_roce,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
    if (ret != 0) {
        fprintf(stderr, "  [恢复] 重新建连失败\n");
        return -1;
    }
    print_qp_state(res->qp);
    printf("  [恢复] QP 已恢复到 RTS!\n");

    /* 清空 CQ 中的残留 CQE */
    struct ibv_wc wc;
    while (ibv_poll_cq(res->send_cq, 1, &wc) > 0) { /* 清空 */ }
    while (ibv_poll_cq(res->recv_cq, 1, &wc) > 0) { /* 清空 */ }

    return 0;
}

/* ========== 辅助: 非阻塞 poll CQ (带超时) ========== */
static int poll_cq_with_timeout(struct ibv_cq *cq, struct ibv_wc *wc,
                                int timeout_us)
{
    int ne;
    int elapsed = 0;
    int step = 100;  /* 每次 sleep 100μs */

    while (elapsed < timeout_us) {
        ne = ibv_poll_cq(cq, 1, wc);
        if (ne < 0) return -1;
        if (ne > 0) return 0;
        usleep(step);
        elapsed += step;
    }
    return -2;  /* 超时 */
}

/* ========== 场景 A: IBV_WC_REM_ACCESS_ERR ========== */
static void test_rem_access_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ 场景 A: IBV_WC_REM_ACCESS_ERR (远端访问权限错误)    ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  触发方法: 使用错误的 rkey 执行 RDMA Write\n\n");

    if (create_and_connect_qp(res, 7) != 0) {
        fprintf(stderr, "  创建 QP 失败，跳过此测试\n");
        return;
    }

    /* 构建 RDMA Write WR，使用错误的 rkey */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xA001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = (uint64_t)res->buf;
    wr.wr.rdma.rkey        = 0xDEADBEEF;   /* 故意使用错误的 rkey! */

    printf("  发送 RDMA Write (rkey=0xDEADBEEF, 故意错误)...\n");

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  post_send 失败: %s\n", strerror(ret));
        return;
    }

    /* 等待错误 CQE */
    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 2000000);
    if (ret == 0) {
        printf("  收到 WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else {
        printf("  等待 CQE 超时 (远端可能未返回错误)\n");
    }

    /* 恢复 QP */
    recover_qp(res);
}

/* ========== 场景 B: IBV_WC_LOC_PROT_ERR ========== */
static void test_loc_prot_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ 场景 B: IBV_WC_LOC_PROT_ERR (本地保护域错误)       ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  触发方法: 使用不同 PD 中 MR 的 lkey\n");
    printf("  QP 属于 PD1, 但 lkey 来自 PD2 注册的 MR\n\n");

    if (create_and_connect_qp(res, 7) != 0) {
        fprintf(stderr, "  创建 QP 失败，跳过此测试\n");
        return;
    }

    /* 先 post recv (避免 RNR) */
    struct ibv_sge recv_sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };
    struct ibv_recv_wr recv_wr, *bad_recv;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;
    ibv_post_recv(res->qp, &recv_wr, &bad_recv);

    /* 使用 mr2 (属于 pd2) 的 lkey */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf2,
        .length = 64,
        .lkey   = res->mr2->lkey,   /* PD2 的 lkey, 与 QP 的 PD 不匹配! */
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xB001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    printf("  发送 Send (lkey 来自不同 PD, 故意错误)...\n");

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  post_send 失败: %s\n", strerror(ret));
        return;
    }

    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 2000000);
    if (ret == 0) {
        printf("  收到 WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else {
        printf("  等待 CQE 超时\n");
    }

    recover_qp(res);
}

/* ========== 场景 C: IBV_WC_WR_FLUSH_ERR ========== */
static void test_wr_flush_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ 场景 C: IBV_WC_WR_FLUSH_ERR (WR 被刷出)            ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  触发方法: 先让 QP 进入 ERROR 状态，再 post send\n\n");

    if (create_and_connect_qp(res, 7) != 0) {
        fprintf(stderr, "  创建 QP 失败，跳过此测试\n");
        return;
    }

    /* 手动将 QP 转到 ERROR 状态 */
    struct ibv_qp_attr err_attr;
    memset(&err_attr, 0, sizeof(err_attr));
    err_attr.qp_state = IBV_QPS_ERR;
    int ret = ibv_modify_qp(res->qp, &err_attr, IBV_QP_STATE);
    if (ret != 0) {
        fprintf(stderr, "  将 QP 转到 ERROR 失败: %s\n", strerror(errno));
        return;
    }

    printf("  QP 已手动转到 ERROR 状态\n");
    print_qp_state(res->qp);

    /* 在 ERROR 状态下 post send */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xC001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    printf("  在 ERROR 状态下 post_send...\n");

    ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        printf("  post_send 返回错误: %s (某些驱动会直接拒绝)\n", strerror(ret));
    }

    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 1000000);
    if (ret == 0) {
        printf("  收到 WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else if (ret == -2) {
        printf("  CQE 超时 (post_send 可能被驱动直接拒绝)\n");
    }

    recover_qp(res);
}

/* ========== 场景 D: IBV_WC_RNR_RETRY_EXC_ERR ========== */
static void test_rnr_retry_err(struct test_resources *res)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║ 场景 D: IBV_WC_RNR_RETRY_EXC_ERR (RNR 重试超限)   ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  触发方法: 不 post recv 就发送 (rnr_retry=0)\n");
    printf("  rnr_retry=0 表示不重试，立即报错\n\n");

    /* 使用 rnr_retry=0 创建 QP (不重试) */
    if (create_and_connect_qp(res, 0) != 0) {
        fprintf(stderr, "  创建 QP 失败，跳过此测试\n");
        return;
    }

    printf("  QP 创建完毕 (rnr_retry=0)\n");
    printf("  故意不 post recv, 直接发送...\n");

    /* 直接发送，不先 post recv */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = 64,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0xD001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  post_send 失败: %s\n", strerror(ret));
        return;
    }

    /* 等待错误 CQE (RNR 需要等超时) */
    printf("  等待 RNR 超时...\n");
    struct ibv_wc wc;
    ret = poll_cq_with_timeout(res->send_cq, &wc, 5000000);  /* 5 秒超时 */
    if (ret == 0) {
        printf("  收到 WC: status=%s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
        print_wc_detail(&wc);
        print_error_diagnosis(wc.status);
    } else if (ret == -2) {
        printf("  等待 CQE 超时 (RNR 错误可能需要更长时间)\n");
        printf("  提示: SoftRoCE 中 RNR 行为可能与硬件不同\n");
    }

    recover_qp(res);
}

/* ========== 清理资源 ========== */
static void cleanup_resources(struct test_resources *res)
{
    if (res->qp)       ibv_destroy_qp(res->qp);
    if (res->send_cq)  ibv_destroy_cq(res->send_cq);
    if (res->recv_cq)  ibv_destroy_cq(res->recv_cq);
    if (res->mr)       ibv_dereg_mr(res->mr);
    if (res->mr2)      ibv_dereg_mr(res->mr2);
    if (res->pd)       ibv_dealloc_pd(res->pd);
    if (res->pd2)      ibv_dealloc_pd(res->pd2);
    if (res->ctx)      ibv_close_device(res->ctx);
    if (res->dev_list) ibv_free_device_list(res->dev_list);
    free(res->buf);
    free(res->buf2);
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    struct test_resources res;

    printf("=== RDMA 错误触发与恢复演示 ===\n\n");
    printf("本程序将故意触发 4 种典型 RDMA 错误，\n");
    printf("展示错误信息并演示 QP 恢复流程。\n");

    /* 初始化 */
    if (init_resources(&res) != 0) {
        fprintf(stderr, "[错误] 资源初始化失败\n");
        cleanup_resources(&res);
        return 1;
    }

    /* 依次触发 4 种错误 */
    test_rem_access_err(&res);
    test_loc_prot_err(&res);
    test_wr_flush_err(&res);
    test_rnr_retry_err(&res);

    /* 总结 */
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║                      总结                            ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    printf("  RDMA 错误处理要点:\n");
    printf("  1. 始终检查 ibv_poll_cq 返回的 wc.status\n");
    printf("  2. 使用 ibv_wc_status_str() 获取可读的错误描述\n");
    printf("  3. QP 出错会自动进入 ERROR 状态\n");
    printf("  4. 恢复路径: ERROR → RESET → INIT → RTR → RTS\n");
    printf("  5. FLUSH_ERR 是继发错误, 需要找到根本原因\n");
    printf("  6. 设置合理的 rnr_retry (推荐 7=无限重试)\n");
    printf("  7. 生产环境中应注册异步事件处理 (ibv_get_async_event)\n");

    /* 清理 */
    cleanup_resources(&res);
    printf("\n程序结束\n");
    return 0;
}
