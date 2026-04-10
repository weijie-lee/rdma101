/**
 * Scatter-Gather Element (SGE) 演示 - Loopback 模式
 *
 * 演示 RDMA 的 Scatter/Gather 机制:
 *   - 发送端: 使用 3 个 SGE 指向 3 块不连续的内存区域 (header, payload, trailer)
 *   - NIC 会自动将 3 块数据"聚合"(gather) 成一个连续的网络包发送
 *   - 接收端: 使用 1 个大 SGE 接收，数据被"分散"(scatter) 到接收缓冲区
 *   - 验证接收到的数据 == header + payload + trailer 的拼接
 *
 * 工作原理:
 *   Gather (发送端): NIC 按 SGE 顺序从多块内存中读取数据，拼接成一个 RDMA 消息
 *   Scatter (接收端): NIC 按 SGE 顺序将收到的数据写入多块内存
 *
 * 本程序使用 Loopback (QP 连接自己)，单进程即可运行。
 *
 * 编译: gcc -Wall -O2 -g -o 02_sge_demo 02_sge_demo.c -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* 三块不连续的数据区域 (按规格: header=16, payload=32, trailer=8) */
#define HEADER_SIZE     16
#define PAYLOAD_SIZE    32
#define TRAILER_SIZE    8
#define TOTAL_SIZE      (HEADER_SIZE + PAYLOAD_SIZE + TRAILER_SIZE)

/* 接收缓冲区大小 */
#define RECV_BUF_SIZE   512

/* ========== RDMA 资源 ========== */
struct sge_demo_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;

    /* 发送端: 3 块不连续内存 */
    char *header;
    char *payload;
    char *trailer;
    struct ibv_mr *header_mr;
    struct ibv_mr *payload_mr;
    struct ibv_mr *trailer_mr;

    /* 接收端: 1 块连续内存 */
    char *recv_buf;
    struct ibv_mr *recv_mr;

    uint8_t port;
    int is_roce;
};

/* ========== 初始化 RDMA 资源 ========== */
static int init_resources(struct sge_demo_ctx *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices = 0;
    int ret = -1;

    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 没有 RDMA 设备\n");
        goto cleanup;
    }

    res->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(res->ctx, "打开设备失败");
    printf("[信息] 打开设备: %s\n", ibv_get_device_name(dev_list[0]));

    /* 检测传输层类型 */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[信息] 传输层: %s\n", transport_str(transport));

    /* PD */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配 PD 失败");

    /* CQ: 需要足够容纳 send + recv 的完成事件 */
    res->cq = ibv_create_cq(res->ctx, 64, NULL, NULL, 0);
    CHECK_NULL(res->cq, "创建 CQ 失败");

    /* QP: max_send_sge=4, max_recv_sge=4 以支持多 SGE */
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 4;  /* 发送端最多 4 个 SGE */
    qp_attr.cap.max_recv_sge = 4;  /* 接收端最多 4 个 SGE */

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "创建 QP 失败");
    printf("[信息] QP num=%u, max_send_sge=%d, max_recv_sge=%d\n",
           res->qp->qp_num, qp_attr.cap.max_send_sge, qp_attr.cap.max_recv_sge);

    /* 分配 3 块不连续的发送内存 */
    res->header  = (char *)malloc(HEADER_SIZE);
    res->payload = (char *)malloc(PAYLOAD_SIZE);
    res->trailer = (char *)malloc(TRAILER_SIZE);
    CHECK_NULL(res->header, "分配 header 失败");
    CHECK_NULL(res->payload, "分配 payload 失败");
    CHECK_NULL(res->trailer, "分配 trailer 失败");

    /* 分配接收缓冲区 */
    res->recv_buf = (char *)malloc(RECV_BUF_SIZE);
    CHECK_NULL(res->recv_buf, "分配 recv_buf 失败");
    memset(res->recv_buf, 0, RECV_BUF_SIZE);

    /* 为每块内存注册独立的 MR */
    res->header_mr = ibv_reg_mr(res->pd, res->header, HEADER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->header_mr, "注册 header MR 失败");

    res->payload_mr = ibv_reg_mr(res->pd, res->payload, PAYLOAD_SIZE,
                                 IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->payload_mr, "注册 payload MR 失败");

    res->trailer_mr = ibv_reg_mr(res->pd, res->trailer, TRAILER_SIZE,
                                 IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->trailer_mr, "注册 trailer MR 失败");

    res->recv_mr = ibv_reg_mr(res->pd, res->recv_buf, RECV_BUF_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->recv_mr, "注册 recv MR 失败");

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Loopback 建连 (QP 连接自己) ========== */
static int setup_loopback(struct sge_demo_ctx *res)
{
    /* 填充本地端点 (对端也是自己) */
    struct rdma_endpoint self_ep;
    memset(&self_ep, 0, sizeof(self_ep));
    if (fill_local_endpoint(res->ctx, res->qp, res->port,
                            RDMA_DEFAULT_GID_INDEX, &self_ep) != 0) {
        fprintf(stderr, "[错误] 填充端点信息失败\n");
        return -1;
    }

    /* QP 状态转换: RESET -> INIT -> RTR -> RTS */
    int access = IBV_ACCESS_LOCAL_WRITE;
    int ret = qp_full_connect(res->qp, &self_ep, res->port, res->is_roce, access);
    if (ret != 0) {
        fprintf(stderr, "[错误] Loopback QP 建连失败\n");
        return -1;
    }
    printf("[信息] QP Loopback 建连完成 (RESET->INIT->RTR->RTS)\n");
    return 0;
}

/* ========== 填充测试数据 ========== */
static void fill_test_data(struct sge_demo_ctx *res)
{
    /* Header: 16 字节, 以 "HDR:" 开头 */
    memset(res->header, 0, HEADER_SIZE);
    snprintf(res->header, HEADER_SIZE, "HDR:HELLO_RDMA!");

    /* Payload: 32 字节, 以 "PAYLOAD" 开头 */
    memset(res->payload, 0, PAYLOAD_SIZE);
    snprintf(res->payload, PAYLOAD_SIZE, "PAYLOAD:SGE-GATHER-DEMO-DATA123");

    /* Trailer: 8 字节, 以 "END." 开头 */
    memset(res->trailer, 0, TRAILER_SIZE);
    snprintf(res->trailer, TRAILER_SIZE, "END.OK!");

    printf("\n[信息] === 发送端数据准备 ===\n");
    printf("  Header  (%3d 字节): addr=%p, 内容=\"%s\"\n",
           HEADER_SIZE, (void *)res->header, res->header);
    printf("  Payload (%3d 字节): addr=%p, 内容=\"%s\"\n",
           PAYLOAD_SIZE, (void *)res->payload, res->payload);
    printf("  Trailer (%3d 字节): addr=%p, 内容=\"%s\"\n",
           TRAILER_SIZE, (void *)res->trailer, res->trailer);
    printf("  总计: %d 字节 (3 块不连续内存)\n\n", TOTAL_SIZE);
}

/* ========== 执行多 SGE Send/Recv ========== */
static int do_sge_send_recv(struct sge_demo_ctx *res)
{
    int ret;

    /* ---- 步骤 1: 接收端 post recv (1 个大 SGE) ---- */
    printf("[步骤1] 接收端: post recv (1 个 SGE, %d 字节缓冲区)\n", RECV_BUF_SIZE);

    struct ibv_sge recv_sge = {
        .addr   = (uint64_t)res->recv_buf,
        .length = RECV_BUF_SIZE,
        .lkey   = res->recv_mr->lkey,
    };

    struct ibv_recv_wr recv_wr;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 100;
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;

    struct ibv_recv_wr *bad_recv_wr = NULL;
    ret = ibv_post_recv(res->qp, &recv_wr, &bad_recv_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] ibv_post_recv 失败: %d\n", ret);
        return -1;
    }

    /* ---- 步骤 2: 发送端 post send (3 个 SGE) ---- */
    printf("[步骤2] 发送端: post send (3 个 SGE，gather 模式)\n");

    /*
     * 构建 3 个 SGE，指向 3 块不连续内存。
     * NIC 会按顺序从这 3 块内存中读取数据 (gather)，
     * 拼接成一个 RDMA 消息发送出去。
     */
    struct ibv_sge send_sges[3];

    /* SGE 0: Header */
    send_sges[0].addr   = (uint64_t)res->header;
    send_sges[0].length = (uint32_t)strlen(res->header);  /* 只发送实际数据 */
    send_sges[0].lkey   = res->header_mr->lkey;

    /* SGE 1: Payload */
    send_sges[1].addr   = (uint64_t)res->payload;
    send_sges[1].length = (uint32_t)strlen(res->payload);
    send_sges[1].lkey   = res->payload_mr->lkey;

    /* SGE 2: Trailer */
    send_sges[2].addr   = (uint64_t)res->trailer;
    send_sges[2].length = (uint32_t)strlen(res->trailer);
    send_sges[2].lkey   = res->trailer_mr->lkey;

    /* 打印每个 SGE 的详情 */
    uint32_t total_send_bytes = 0;
    for (int i = 0; i < 3; i++) {
        printf("  SGE[%d]: addr=%p, length=%u, lkey=0x%x\n",
               i, (void *)send_sges[i].addr,
               send_sges[i].length, send_sges[i].lkey);
        total_send_bytes += send_sges[i].length;
    }
    printf("  发送总字节: %u\n\n", total_send_bytes);

    struct ibv_send_wr send_wr;
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 200;
    send_wr.sg_list    = send_sges;
    send_wr.num_sge    = 3;         /* 3 个 SGE! */
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad_send_wr = NULL;
    ret = ibv_post_send(res->qp, &send_wr, &bad_send_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] ibv_post_send 失败: %d\n", ret);
        return -1;
    }

    /* ---- 步骤 3: 等待发送完成 ---- */
    printf("[步骤3] 等待发送完成...\n");
    struct ibv_wc wc;
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;
    printf("  发送端 WC:\n");
    print_wc_detail(&wc);
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[错误] 发送失败!\n");
        return -1;
    }

    /* ---- 步骤 4: 等待接收完成 ---- */
    printf("\n[步骤4] 等待接收完成...\n");
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;
    printf("  接收端 WC:\n");
    print_wc_detail(&wc);
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[错误] 接收失败!\n");
        return -1;
    }

    printf("\n[信息] 接收到 %u 字节数据\n", wc.byte_len);

    return (int)wc.byte_len;
}

/* ========== 验证数据完整性 ========== */
static int verify_data(struct sge_demo_ctx *res, int recv_len)
{
    /* 构建期望的拼接结果 */
    char expected[RECV_BUF_SIZE];
    memset(expected, 0, sizeof(expected));

    int hdr_len = (int)strlen(res->header);
    int pay_len = (int)strlen(res->payload);
    int trl_len = (int)strlen(res->trailer);
    int expect_total = hdr_len + pay_len + trl_len;

    memcpy(expected, res->header, hdr_len);
    memcpy(expected + hdr_len, res->payload, pay_len);
    memcpy(expected + hdr_len + pay_len, res->trailer, trl_len);

    printf("\n[验证] === 数据完整性检查 ===\n");
    printf("  期望长度: %d 字节\n", expect_total);
    printf("  实际长度: %d 字节\n", recv_len);

    if (recv_len != expect_total) {
        printf("  [失败] 长度不匹配!\n");
        return -1;
    }

    if (memcmp(res->recv_buf, expected, expect_total) == 0) {
        printf("  [成功] 数据完全匹配! NIC 正确地将 3 块不连续内存 gather 成一个消息\n");
    } else {
        printf("  [失败] 数据不匹配!\n");
        printf("  期望: \"%.*s\"\n", expect_total, expected);
        printf("  实际: \"%.*s\"\n", recv_len, res->recv_buf);
        return -1;
    }

    /* 分段展示接收到的数据 */
    printf("\n  接收缓冲区内容拆解:\n");
    printf("    [0..%d)    Header 部分:  \"%.*s\"\n",
           hdr_len, hdr_len, res->recv_buf);
    printf("    [%d..%d)  Payload 部分: \"%.*s\"\n",
           hdr_len, hdr_len + pay_len, pay_len, res->recv_buf + hdr_len);
    printf("    [%d..%d) Trailer 部分: \"%.*s\"\n",
           hdr_len + pay_len, expect_total, trl_len,
           res->recv_buf + hdr_len + pay_len);

    return 0;
}

/* ========== 清理 ========== */
static void cleanup_resources(struct sge_demo_ctx *res)
{
    if (res->recv_mr)    ibv_dereg_mr(res->recv_mr);
    if (res->trailer_mr) ibv_dereg_mr(res->trailer_mr);
    if (res->payload_mr) ibv_dereg_mr(res->payload_mr);
    if (res->header_mr)  ibv_dereg_mr(res->header_mr);
    if (res->recv_buf)   free(res->recv_buf);
    if (res->trailer)    free(res->trailer);
    if (res->payload)    free(res->payload);
    if (res->header)     free(res->header);
    if (res->qp)         ibv_destroy_qp(res->qp);
    if (res->cq)         ibv_destroy_cq(res->cq);
    if (res->pd)         ibv_dealloc_pd(res->pd);
    if (res->ctx)        ibv_close_device(res->ctx);
}

/* ========== 主函数 ========== */
int main(void)
{
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   Scatter-Gather Element (SGE) 演示 - Loopback  ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ 发送端: 3 个 SGE → NIC gather → 1 个 RDMA 消息  ║\n");
    printf("║ 接收端: 1 个 SGE → NIC scatter → 接收缓冲区     ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    struct sge_demo_ctx res;
    memset(&res, 0, sizeof(res));

    /* 1. 初始化资源 */
    if (init_resources(&res) != 0) {
        fprintf(stderr, "[错误] 初始化资源失败\n");
        return 1;
    }

    /* 2. Loopback 建连 */
    if (setup_loopback(&res) != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 3. 填充测试数据 */
    fill_test_data(&res);

    /* 4. 执行 SGE Send/Recv */
    int recv_len = do_sge_send_recv(&res);
    if (recv_len < 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 5. 验证数据 */
    verify_data(&res, recv_len);

    /* 6. 清理 */
    printf("\n[信息] 程序结束\n");
    cleanup_resources(&res);
    return 0;
}
