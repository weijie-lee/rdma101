/**
 * 手动 TCP 交换建连示例 —— RDMA RC 模式双进程连接
 *
 * 功能：
 *   - 服务器/客户端双模式，通过命令行参数选择
 *   - 自动检测 InfiniBand / RoCE 传输类型
 *   - 通过 TCP Socket 交换 RDMA 端点信息 (QPN, LID, GID, PSN)
 *   - 使用 qp_full_connect() 完成 QP 状态转换 (RESET → INIT → RTR → RTS)
 *   - 建连后执行双向 Send/Recv 测试验证连接
 *
 * 用法：
 *   服务器: ./manual_connect -s [tcp_port]
 *   客户端: ./manual_connect -c <server_ip> [tcp_port]
 *
 * 编译：
 *   gcc -o manual_connect manual_connect.c -I../../common ../../common/librdma_utils.a -libverbs
 *
 * 运行示例：
 *   终端1: ./manual_connect -s 7471
 *   终端2: ./manual_connect -c 192.168.1.100 7471
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */

#define BUFFER_SIZE     1024            /* 发送/接收缓冲区大小 */
#define DEFAULT_PORT    7471            /* 默认 TCP 端口 */
#define IB_PORT         1               /* RDMA 端口号 */
#define GID_INDEX       0               /* GID 表索引 (RoCE v2 通常用 1 或 3) */
#define CQ_DEPTH        16              /* CQ 深度 */
#define MAX_WR          16              /* QP 最大 WR 数 */

/* 服务器发送的消息 */
#define SERVER_MSG      "Hello from server!"
/* 客户端发送的消息 */
#define CLIENT_MSG      "Hello from client!"

/* ========== RDMA 资源结构体 ========== */

struct rdma_context {
    /* 设备与保护域 */
    struct ibv_context      *ctx;           /* 设备上下文 */
    struct ibv_pd           *pd;            /* 保护域 */
    struct ibv_cq           *cq;            /* 完成队列 */
    struct ibv_qp           *qp;            /* 队列对 (RC 类型) */

    /* 内存区域 */
    struct ibv_mr           *send_mr;       /* 发送缓冲区 MR */
    struct ibv_mr           *recv_mr;       /* 接收缓冲区 MR */
    char                    *send_buf;      /* 发送缓冲区 */
    char                    *recv_buf;      /* 接收缓冲区 */

    /* 连接信息 */
    struct rdma_endpoint    local_ep;       /* 本地端点信息 */
    struct rdma_endpoint    remote_ep;      /* 远端端点信息 */

    /* 传输层类型 */
    enum rdma_transport     transport;      /* IB / RoCE */
    int                     is_roce;        /* 是否为 RoCE 模式 */

    /* 运行参数 */
    int                     is_server;      /* 是否为服务器模式 */
    const char              *server_ip;     /* 服务器 IP (客户端模式) */
    int                     tcp_port;       /* TCP 端口号 */
};

/* ========== 函数声明 ========== */

static void print_usage(const char *prog);
static int  parse_args(struct rdma_context *rctx, int argc, char *argv[]);
static int  init_rdma_resources(struct rdma_context *rctx);
static int  exchange_and_connect(struct rdma_context *rctx);
static int  do_send_recv_test(struct rdma_context *rctx);
static void cleanup_rdma_resources(struct rdma_context *rctx);
static void print_endpoint(const char *label, const struct rdma_endpoint *ep,
                           int is_roce);

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    struct rdma_context rctx;
    int ret = 1;

    memset(&rctx, 0, sizeof(rctx));
    rctx.tcp_port = DEFAULT_PORT;

    /* 第一步：解析命令行参数 */
    if (parse_args(&rctx, argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  RDMA 手动建连示例 (RC Send/Recv)\n");
    printf("  模式: %s\n", rctx.is_server ? "服务器" : "客户端");
    printf("  TCP 端口: %d\n", rctx.tcp_port);
    printf("========================================\n\n");

    /* 第二步：初始化 RDMA 资源 */
    printf("[步骤 1] 初始化 RDMA 资源...\n");
    if (init_rdma_resources(&rctx) != 0) {
        fprintf(stderr, "[错误] RDMA 资源初始化失败\n");
        goto cleanup;
    }
    printf("[步骤 1] RDMA 资源初始化完成 ✓\n\n");

    /* 第三步：交换端点信息并建立连接 */
    printf("[步骤 2] 交换端点信息并建立 QP 连接...\n");
    if (exchange_and_connect(&rctx) != 0) {
        fprintf(stderr, "[错误] QP 连接建立失败\n");
        goto cleanup;
    }
    printf("[步骤 2] QP 连接建立完成 ✓\n\n");

    /* 第四步：执行双向 Send/Recv 测试 */
    printf("[步骤 3] 执行双向 Send/Recv 通信测试...\n");
    if (do_send_recv_test(&rctx) != 0) {
        fprintf(stderr, "[错误] Send/Recv 测试失败\n");
        goto cleanup;
    }
    printf("[步骤 3] 通信测试完成 ✓\n\n");

    printf("========================================\n");
    printf("  全部测试通过！RDMA 连接工作正常。\n");
    printf("========================================\n");
    ret = 0;

cleanup:
    cleanup_rdma_resources(&rctx);
    return ret;
}

/* ========== 打印用法 ========== */

static void print_usage(const char *prog)
{
    fprintf(stderr, "\n用法:\n");
    fprintf(stderr, "  服务器: %s -s [tcp_port]\n", prog);
    fprintf(stderr, "  客户端: %s -c <server_ip> [tcp_port]\n", prog);
    fprintf(stderr, "\n参数:\n");
    fprintf(stderr, "  -s           服务器模式 (监听等待连接)\n");
    fprintf(stderr, "  -c <ip>      客户端模式 (连接到指定服务器)\n");
    fprintf(stderr, "  tcp_port     TCP 端口号 (默认: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "\n示例:\n");
    fprintf(stderr, "  %s -s 7471\n", prog);
    fprintf(stderr, "  %s -c 192.168.1.100 7471\n", prog);
    fprintf(stderr, "\n");
}

/* ========== 解析命令行参数 ========== */

static int parse_args(struct rdma_context *rctx, int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }

    /* 解析 -s (服务器) 或 -c (客户端) 模式 */
    if (strcmp(argv[1], "-s") == 0) {
        /* 服务器模式 */
        rctx->is_server = 1;
        rctx->server_ip = NULL;
        /* 可选: TCP 端口 */
        if (argc >= 3) {
            rctx->tcp_port = atoi(argv[2]);
        }
    } else if (strcmp(argv[1], "-c") == 0) {
        /* 客户端模式: 需要服务器 IP */
        if (argc < 3) {
            fprintf(stderr, "[错误] 客户端模式需要指定服务器 IP\n");
            return -1;
        }
        rctx->is_server = 0;
        rctx->server_ip = argv[2];
        /* 可选: TCP 端口 */
        if (argc >= 4) {
            rctx->tcp_port = atoi(argv[3]);
        }
    } else {
        return -1;
    }

    return 0;
}

/* ========== 初始化 RDMA 资源 ========== */

static int init_rdma_resources(struct rdma_context *rctx)
{
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    int num_devices;
    int ret = -1;

    /* 1. 获取设备列表并打开第一个设备 */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取 RDMA 设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未发现 RDMA 设备\n");
        goto cleanup;
    }
    printf("  发现 %d 个 RDMA 设备, 使用: %s\n",
           num_devices, ibv_get_device_name(dev_list[0]));

    rctx->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(rctx->ctx, "打开 RDMA 设备失败");

    /* 2. 检测传输层类型 (IB / RoCE) */
    rctx->transport = detect_transport(rctx->ctx, IB_PORT);
    rctx->is_roce = (rctx->transport == RDMA_TRANSPORT_ROCE);
    printf("  传输类型: %s\n", transport_str(rctx->transport));

    /* 3. 分配保护域 (PD) */
    rctx->pd = ibv_alloc_pd(rctx->ctx);
    CHECK_NULL(rctx->pd, "分配保护域 (PD) 失败");

    /* 4. 创建完成队列 (CQ) */
    rctx->cq = ibv_create_cq(rctx->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(rctx->cq, "创建完成队列 (CQ) 失败");

    /* 5. 创建 RC 类型的队列对 (QP) */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq  = rctx->cq;
    qp_init_attr.recv_cq  = rctx->cq;
    qp_init_attr.qp_type  = IBV_QPT_RC;    /* RC: Reliable Connected */
    qp_init_attr.cap.max_send_wr  = MAX_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    rctx->qp = ibv_create_qp(rctx->pd, &qp_init_attr);
    CHECK_NULL(rctx->qp, "创建队列对 (QP) 失败");
    printf("  QP 编号: %u (类型: RC)\n", rctx->qp->qp_num);

    /* 打印初始 QP 状态 (应为 RESET) */
    print_qp_state(rctx->qp);

    /* 6. 分配并注册发送缓冲区 */
    rctx->send_buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(rctx->send_buf, "分配发送缓冲区失败");
    memset(rctx->send_buf, 0, BUFFER_SIZE);

    rctx->send_mr = ibv_reg_mr(rctx->pd, rctx->send_buf, BUFFER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(rctx->send_mr, "注册发送 MR 失败");

    /* 7. 分配并注册接收缓冲区 */
    rctx->recv_buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(rctx->recv_buf, "分配接收缓冲区失败");
    memset(rctx->recv_buf, 0, BUFFER_SIZE);

    rctx->recv_mr = ibv_reg_mr(rctx->pd, rctx->recv_buf, BUFFER_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(rctx->recv_mr, "注册接收 MR 失败");

    printf("  MR 注册完成: send_lkey=0x%x, recv_lkey=0x%x\n",
           rctx->send_mr->lkey, rctx->recv_mr->lkey);

    ret = 0;

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }
    return ret;
}

/* ========== 打印端点信息 ========== */

static void print_endpoint(const char *label, const struct rdma_endpoint *ep,
                           int is_roce)
{
    char gid_str[46];
    gid_to_str(&ep->gid, gid_str, sizeof(gid_str));

    printf("  %s 端点信息:\n", label);
    printf("    QP 编号 (qp_num): %u\n", ep->qp_num);
    printf("    端口号 (port):    %u\n", ep->port_num);
    printf("    PSN:              %u\n", ep->psn);
    if (is_roce) {
        printf("    GID 索引:         %u\n", ep->gid_index);
        printf("    GID:              %s\n", gid_str);
    } else {
        printf("    LID:              %u\n", ep->lid);
    }
}

/* ========== 交换端点信息并建立 QP 连接 ========== */

static int exchange_and_connect(struct rdma_context *rctx)
{
    int ret;

    /* 1. 填充本地端点信息 (QPN, LID, GID, PSN 等) */
    ret = fill_local_endpoint(rctx->ctx, rctx->qp, IB_PORT,
                              GID_INDEX, &rctx->local_ep);
    CHECK_ERRNO(ret, "填充本地端点信息失败");

    printf("\n");
    print_endpoint("本地", &rctx->local_ep, rctx->is_roce);

    /* 2. 通过 TCP Socket 交换端点信息 */
    printf("\n  通过 TCP 交换端点信息...\n");
    /*
     * exchange_endpoint_tcp():
     *   server_ip = NULL → 服务器模式 (监听)
     *   server_ip = "x.x.x.x" → 客户端模式 (连接)
     */
    ret = exchange_endpoint_tcp(rctx->server_ip, rctx->tcp_port,
                                &rctx->local_ep, &rctx->remote_ep);
    CHECK_ERRNO(ret, "TCP 端点信息交换失败");

    printf("\n");
    print_endpoint("远端", &rctx->remote_ep, rctx->is_roce);

    /* 3. 使用 qp_full_connect() 完成 QP 状态转换 */
    /*
     * qp_full_connect() 内部执行:
     *   RESET → INIT (设置端口和访问权限)
     *   INIT  → RTR  (设置对端地址，准备接收)
     *   RTR   → RTS  (准备发送)
     *
     * 对于 IB 模式: 使用 LID 寻址
     * 对于 RoCE 模式: 使用 GID 寻址 (is_global=1)
     */
    printf("\n  执行 QP 状态转换 (RESET → INIT → RTR → RTS)...\n");
    int access_flags = IBV_ACCESS_LOCAL_WRITE |
                       IBV_ACCESS_REMOTE_READ |
                       IBV_ACCESS_REMOTE_WRITE;

    ret = qp_full_connect(rctx->qp, &rctx->remote_ep,
                          IB_PORT, rctx->is_roce, access_flags);
    CHECK_ERRNO(ret, "QP 连接失败 (qp_full_connect)");

    /* 打印最终 QP 状态 (应为 RTS) */
    print_qp_state(rctx->qp);
    printf("  QP 连接建立成功! 传输类型: %s\n", transport_str(rctx->transport));

    return 0;

cleanup:
    return -1;
}

/* ========== 执行双向 Send/Recv 测试 ========== */

static int do_send_recv_test(struct rdma_context *rctx)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    int ret;

    /*
     * 通信流程:
     *   1. 两端都先 Post Recv (准备接收)
     *   2. 服务器发送 "Hello from server!"
     *   3. 客户端发送 "Hello from client!"
     *   4. 两端都 Poll CQ 获取 Send 完成和 Recv 完成
     */

    /* ---- 第一步: Post Recv (准备接收对方的消息) ---- */
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)rctx->recv_buf;
    sge.length = BUFFER_SIZE;
    sge.lkey   = rctx->recv_mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;       /* 用 wr_id=1 标识 Recv */
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(rctx->qp, &recv_wr, &bad_recv_wr);
    CHECK_ERRNO(ret, "Post Recv 失败");
    printf("  Post Recv 完成 (等待对方消息)\n");

    /* ---- 第二步: 准备发送消息 ---- */
    const char *msg = rctx->is_server ? SERVER_MSG : CLIENT_MSG;
    strncpy(rctx->send_buf, msg, BUFFER_SIZE - 1);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)rctx->send_buf;
    sge.length = (uint32_t)(strlen(msg) + 1);  /* 包含 '\0' */
    sge.lkey   = rctx->send_mr->lkey;

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;    /* 用 wr_id=2 标识 Send */
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;     /* 请求完成通知 */

    /* ---- 第三步: Post Send ---- */
    ret = ibv_post_send(rctx->qp, &send_wr, &bad_send_wr);
    CHECK_ERRNO(ret, "Post Send 失败");
    printf("  Post Send 完成 (发送: \"%s\")\n", msg);

    /* ---- 第四步: Poll CQ 等待 Send 完成 ---- */
    printf("\n  等待 Send 完成...\n");
    ret = poll_cq_blocking(rctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Send) 失败");
    printf("  Send 完成:\n");
    print_wc_detail(&wc);

    /* ---- 第五步: Poll CQ 等待 Recv 完成 ---- */
    printf("  等待 Recv 完成...\n");
    ret = poll_cq_blocking(rctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Recv) 失败");
    printf("  Recv 完成:\n");
    print_wc_detail(&wc);

    /* ---- 第六步: 打印接收到的消息 ---- */
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║  收到消息: \"%s\"\n", rctx->recv_buf);
    printf("  ╚══════════════════════════════════════════╝\n");

    return 0;

cleanup:
    return -1;
}

/* ========== 清理 RDMA 资源 ========== */

static void cleanup_rdma_resources(struct rdma_context *rctx)
{
    printf("\n[清理] 释放 RDMA 资源...\n");

    /* 按创建的逆序释放资源 */
    if (rctx->recv_mr)  { ibv_dereg_mr(rctx->recv_mr);    rctx->recv_mr = NULL; }
    if (rctx->send_mr)  { ibv_dereg_mr(rctx->send_mr);    rctx->send_mr = NULL; }
    if (rctx->recv_buf) { free(rctx->recv_buf);            rctx->recv_buf = NULL; }
    if (rctx->send_buf) { free(rctx->send_buf);            rctx->send_buf = NULL; }
    if (rctx->qp)       { ibv_destroy_qp(rctx->qp);       rctx->qp = NULL; }
    if (rctx->cq)       { ibv_destroy_cq(rctx->cq);       rctx->cq = NULL; }
    if (rctx->pd)       { ibv_dealloc_pd(rctx->pd);       rctx->pd = NULL; }
    if (rctx->ctx)      { ibv_close_device(rctx->ctx);    rctx->ctx = NULL; }

    printf("[清理] 完成\n");
}
