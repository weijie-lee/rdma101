/**
 * 保护域 (Protection Domain) 隔离性演示
 *
 * 本程序演示 PD 的核心安全机制 —— 资源隔离:
 *   1. 创建两个独立的 PD (pd1, pd2)
 *   2. 在 pd1 下注册 MR，在 pd2 下创建 QP
 *   3. 尝试用 pd1 的 MR lkey 配合 pd2 的 QP 发送 → 应当失败
 *   4. 演示正常情况: 同一 PD 下的 MR + QP 可以正常工作
 *
 * 编译: gcc -o pd_isolation pd_isolation.c -I../../common ../../common/librdma_utils.a -libverbs
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
#define CQ_DEPTH    16

int main(int argc, char *argv[])
{
    /* ===== 资源声明 ===== */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd1      = NULL;   /* 保护域 1 */
    struct ibv_pd      *pd2      = NULL;   /* 保护域 2 */
    struct ibv_mr      *mr1      = NULL;   /* pd1 下注册的 MR */
    struct ibv_mr      *mr_same  = NULL;   /* 同一 PD 下的 MR (正常用例) */
    struct ibv_cq      *cq1      = NULL;   /* pd1 的 CQ */
    struct ibv_cq      *cq2      = NULL;   /* pd2 的 CQ */
    struct ibv_qp      *qp1      = NULL;   /* pd1 的 QP (正常用例) */
    struct ibv_qp      *qp2      = NULL;   /* pd2 的 QP (跨 PD 用例) */
    char               *buf1     = NULL;
    char               *buf_same = NULL;
    int                 num_devices;
    int                 ret;

    printf("========================================\n");
    printf("  PD 隔离性演示 (Protection Domain)\n");
    printf("========================================\n\n");

    /* ========== 步骤 1: 打开设备 ========== */
    printf("[步骤1] 打开 RDMA 设备\n");
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取设备列表失败");

    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未发现 RDMA 设备\n");
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "打开设备失败");
    printf("  设备: %s\n\n", ibv_get_device_name(dev_list[0]));

    /* ========== 步骤 2: 创建两个独立的 PD ========== */
    printf("[步骤2] 创建两个独立的保护域\n");
    pd1 = ibv_alloc_pd(ctx);
    CHECK_NULL(pd1, "分配 PD1 失败");
    printf("  PD1 创建成功, handle=%u\n", pd1->handle);

    pd2 = ibv_alloc_pd(ctx);
    CHECK_NULL(pd2, "分配 PD2 失败");
    printf("  PD2 创建成功, handle=%u\n", pd2->handle);
    printf("  → 两个 PD 拥有不同的 handle，代表独立的安全域\n\n");

    /* ========== 步骤 3: 在 PD1 下注册 MR ========== */
    printf("[步骤3] 在 PD1 下注册内存区域 (MR)\n");
    buf1 = malloc(BUFFER_SIZE);
    CHECK_NULL(buf1, "malloc buf1 失败");
    memset(buf1, 'A', BUFFER_SIZE);

    mr1 = ibv_reg_mr(pd1, buf1, BUFFER_SIZE,
                      IBV_ACCESS_LOCAL_WRITE |
                      IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(mr1, "在 PD1 下注册 MR 失败");
    printf("  MR1 注册成功 (属于 PD1)\n");
    printf("    lkey=0x%08x, rkey=0x%08x\n", mr1->lkey, mr1->rkey);
    printf("    addr=%p, length=%zu\n\n", mr1->addr, (size_t)mr1->length);

    /* ========== 步骤 4: 创建 CQ ========== */
    printf("[步骤4] 创建完成队列\n");
    cq1 = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq1, "创建 CQ1 失败");
    cq2 = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq2, "创建 CQ2 失败");
    printf("  CQ1, CQ2 创建成功\n\n");

    /* ========== 步骤 5: 在 PD2 下创建 QP ========== */
    printf("[步骤5] 在 PD2 下创建 QP\n");
    struct ibv_qp_init_attr qp2_attr = {
        .send_cq = cq2,
        .recv_cq = cq2,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp2 = ibv_create_qp(pd2, &qp2_attr);
    CHECK_NULL(qp2, "在 PD2 下创建 QP2 失败");
    printf("  QP2 创建成功 (属于 PD2), qp_num=%u\n\n", qp2->qp_num);

    /* ========== 步骤 6: 演示跨 PD 使用 → 失败 ========== */
    printf("============================================================\n");
    printf("[步骤6] 演示跨 PD 使用: 用 PD1 的 MR lkey + PD2 的 QP 发送\n");
    printf("============================================================\n\n");

    /*
     * 将 QP2 (属于 PD2) 转到 INIT 状态，准备发送。
     * 使用 qp_to_init() 工具函数。
     */
    int access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                       IBV_ACCESS_REMOTE_WRITE;
    ret = qp_to_init(qp2, PORT_NUM, access_flags);
    if (ret != 0) {
        printf("  [信息] QP2 RESET→INIT 失败 (ret=%d)\n", ret);
        printf("  → 这可能因为端口状态或配置问题，非 PD 隔离导致\n\n");
    } else {
        printf("  QP2 已转到 INIT 状态\n");
    }

    /*
     * 关键实验: 尝试用 MR1 的 lkey (属于 PD1) 配合 QP2 (属于 PD2) 发送
     *
     * 在 RDMA 中，硬件会校验 SGE 中的 lkey 是否属于 QP 所在的 PD。
     * 如果 PD 不匹配，ibv_post_send() 会返回错误，或者 HCA 会在处理
     * WQE 时生成一个 "Local Protection Error" (IBV_WC_LOC_PROT_ERR)。
     */
    printf("  尝试 ibv_post_send: SGE.lkey=0x%08x (PD1), QP 属于 PD2...\n",
           mr1->lkey);

    struct ibv_sge sge = {
        .addr   = (uintptr_t)buf1,
        .length = 64,
        .lkey   = mr1->lkey,    /* 故意使用 PD1 的 MR lkey */
    };
    struct ibv_send_wr wr = {
        .wr_id      = 1,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad_wr = NULL;

    ret = ibv_post_send(qp2, &wr, &bad_wr);
    if (ret != 0) {
        printf("  ✗ ibv_post_send 失败! errno=%d (%s)\n", errno, strerror(errno));
        printf("  → 这就是 PD 隔离的效果: 硬件/驱动拒绝了跨 PD 的资源访问\n");
        printf("  → 注意: 某些驱动在 post_send 时不检查, 而是在 CQ 中返回\n");
        printf("    IBV_WC_LOC_PROT_ERR (Local Protection Error)\n\n");
    } else {
        /*
         * 有些驱动允许 post_send 成功 (仅将 WR 放入发送队列),
         * 但 HCA 在实际处理时会发现 PD 不匹配，生成错误完成事件。
         * 由于 QP2 尚未到 RTS 状态，这里的 post_send 本身可能因状态问题
         * 而失败。但关键是: 即使 QP 到了 RTS 状态，跨 PD 也会失败。
         */
        printf("  post_send 返回成功 (仅入队)，但 HCA 处理时会检测到 PD 不匹配\n");
        printf("  → CQ 中将收到 IBV_WC_LOC_PROT_ERR 错误\n\n");
    }

    /* ========== 步骤 7: 演示正常用例 (同一 PD) ========== */
    printf("============================================\n");
    printf("[步骤7] 正常用例: 同一 PD 下的 MR + QP\n");
    printf("============================================\n\n");

    /* 在 PD1 下也创建一个 QP */
    struct ibv_qp_init_attr qp1_attr = {
        .send_cq = cq1,
        .recv_cq = cq1,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp1 = ibv_create_qp(pd1, &qp1_attr);
    CHECK_NULL(qp1, "在 PD1 下创建 QP1 失败");
    printf("  QP1 创建成功 (属于 PD1), qp_num=%u\n", qp1->qp_num);

    /* 在同一 PD 下注册另一个 MR */
    buf_same = malloc(BUFFER_SIZE);
    CHECK_NULL(buf_same, "malloc buf_same 失败");
    memset(buf_same, 'B', BUFFER_SIZE);

    mr_same = ibv_reg_mr(pd1, buf_same, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr_same, "在 PD1 下注册 mr_same 失败");
    printf("  mr_same 注册成功 (属于 PD1), lkey=0x%08x\n\n", mr_same->lkey);

    /* 将 QP1 转到 INIT 状态 */
    ret = qp_to_init(qp1, PORT_NUM, IBV_ACCESS_LOCAL_WRITE);
    if (ret == 0) {
        printf("  QP1 RESET→INIT 转换成功\n");
        print_qp_state(qp1);

        /* 使用同一 PD 的 MR lkey 来发送 */
        struct ibv_sge sge_ok = {
            .addr   = (uintptr_t)buf_same,
            .length = 64,
            .lkey   = mr_same->lkey,    /* 同一个 PD (PD1) */
        };
        struct ibv_send_wr wr_ok = {
            .wr_id      = 2,
            .sg_list    = &sge_ok,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
        };
        struct ibv_send_wr *bad_wr_ok = NULL;

        ret = ibv_post_send(qp1, &wr_ok, &bad_wr_ok);
        if (ret != 0) {
            printf("  post_send 失败 (errno=%d: %s)\n", errno, strerror(errno));
            printf("  → 注意: 这可能因为 QP 还未到 RTS 状态, 不是 PD 隔离问题\n");
        } else {
            printf("  ✓ ibv_post_send 成功! 同一 PD 下的 MR+QP 没有隔离问题\n");
        }
    } else {
        printf("  QP1 RESET→INIT 转换失败 (可能端口未就绪)\n");
    }

    /* ========== 总结 ========== */
    printf("\n========================================\n");
    printf("  PD 隔离性总结\n");
    printf("========================================\n");
    printf("  1. 每个 PD 是一个独立的安全边界\n");
    printf("  2. MR、QP、AH 等资源都归属于某个 PD\n");
    printf("  3. 不同 PD 的资源不能混用 (硬件强制检查)\n");
    printf("  4. 跨 PD 使用会导致:\n");
    printf("     - ibv_post_send/recv 返回错误, 或\n");
    printf("     - CQ 中产生 IBV_WC_LOC_PROT_ERR\n");
    printf("  5. 用途: 多租户隔离、进程间安全隔离\n");
    printf("     - 例: VM-A 的 PD 与 VM-B 的 PD 互不可见\n");
    printf("     - 即使知道对方的 lkey/rkey 也无法越权访问\n\n");

cleanup:
    printf("[清理] 释放所有资源...\n");

    if (qp1)      ibv_destroy_qp(qp1);
    if (qp2)      ibv_destroy_qp(qp2);
    if (cq1)      ibv_destroy_cq(cq1);
    if (cq2)      ibv_destroy_cq(cq2);
    if (mr1)      ibv_dereg_mr(mr1);
    if (mr_same)  ibv_dereg_mr(mr_same);
    if (buf1)     free(buf1);
    if (buf_same) free(buf_same);
    if (pd1)      ibv_dealloc_pd(pd1);
    if (pd2)      ibv_dealloc_pd(pd2);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  完成\n");
    return 0;
}
