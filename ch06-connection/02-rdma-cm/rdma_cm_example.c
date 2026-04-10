/**
 * RDMA CM 连接管理示例 —— 使用 librdmacm 建立 RDMA 连接
 *
 * 功能：
 *   - 使用 RDMA CM (Connection Manager) 高级 API 建立连接
 *   - 服务器/客户端双模式
 *   - CM 自动处理地址解析、路由解析、QP 状态转换
 *   - 打印每个 CM 事件的名称和详情
 *   - 建连后执行 Send/Recv 测试验证
 *
 * 用法：
 *   服务器: ./rdma_cm_example -s <port>
 *   客户端: ./rdma_cm_example -c <server_ip> <port>
 *
 * 编译：
 *   gcc -o rdma_cm_example rdma_cm_example.c -lrdmacm -libverbs
 *
 * 运行示例：
 *   终端1: ./rdma_cm_example -s 7471
 *   终端2: ./rdma_cm_example -c 127.0.0.1 7471
 *
 * CM 事件流:
 *   Server                              Client
 *   ──────                              ──────
 *   rdma_listen()
 *                                       rdma_resolve_addr()
 *                                       → ADDR_RESOLVED
 *                                       rdma_resolve_route()
 *                                       → ROUTE_RESOLVED
 *                                       rdma_connect()
 *   → CONNECT_REQUEST
 *   rdma_accept()
 *   → ESTABLISHED                       → ESTABLISHED
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

/* ========== 常量定义 ========== */

#define BUFFER_SIZE         1024        /* 发送/接收缓冲区大小 */
#define TIMEOUT_MS          5000        /* 地址/路由解析超时 (毫秒) */
#define CQ_DEPTH            16          /* CQ 深度 */
#define MAX_WR              16          /* QP 最大 WR 数 */
#define SERVER_MSG          "Hello from RDMA CM server!"
#define CLIENT_MSG          "Hello from RDMA CM client!"

/* ========== 错误处理宏 ========== */

#define CM_CHECK_NULL(ptr, msg) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[错误] %s: %s (errno=%d: %s)\n", \
                (msg), #ptr, errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

#define CM_CHECK_ERRNO(ret, msg) do { \
    if ((ret) != 0) { \
        fprintf(stderr, "[错误] %s: 返回值=%d (errno=%d: %s)\n", \
                (msg), (ret), errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

/* ========== RDMA CM 资源结构体 ========== */

struct cm_context {
    /* CM 资源 */
    struct rdma_event_channel   *ec;            /* 事件通道 */
    struct rdma_cm_id           *listen_id;     /* 服务器监听 ID */
    struct rdma_cm_id           *cm_id;         /* 连接 ID (客户端/服务器通信用) */

    /* Verbs 资源 */
    struct ibv_pd               *pd;            /* 保护域 */
    struct ibv_cq               *cq;            /* 完成队列 */
    struct ibv_qp               *qp;            /* 队列对 */

    /* 内存区域 */
    struct ibv_mr               *send_mr;       /* 发送 MR */
    struct ibv_mr               *recv_mr;       /* 接收 MR */
    char                        *send_buf;      /* 发送缓冲区 */
    char                        *recv_buf;      /* 接收缓冲区 */

    /* 运行参数 */
    int                         is_server;      /* 是否为服务器模式 */
    const char                  *server_ip;     /* 服务器 IP */
    int                         port;           /* 监听端口 */
};

/* ========== CM 事件名称 ========== */

/**
 * cm_event_str - 将 CM 事件类型转为可读字符串
 *
 * RDMA CM 定义了一系列事件用于异步通知连接状态变化。
 * 应用程序通过 rdma_get_cm_event() 获取事件，处理后必须调用 rdma_ack_cm_event() 确认。
 */
static const char *cm_event_str(enum rdma_cm_event_type event)
{
    switch (event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:       return "ADDR_RESOLVED (地址已解析)";
    case RDMA_CM_EVENT_ADDR_ERROR:          return "ADDR_ERROR (地址解析失败)";
    case RDMA_CM_EVENT_ROUTE_RESOLVED:      return "ROUTE_RESOLVED (路由已解析)";
    case RDMA_CM_EVENT_ROUTE_ERROR:         return "ROUTE_ERROR (路由解析失败)";
    case RDMA_CM_EVENT_CONNECT_REQUEST:     return "CONNECT_REQUEST (收到连接请求)";
    case RDMA_CM_EVENT_CONNECT_RESPONSE:    return "CONNECT_RESPONSE (收到连接响应)";
    case RDMA_CM_EVENT_CONNECT_ERROR:       return "CONNECT_ERROR (连接错误)";
    case RDMA_CM_EVENT_UNREACHABLE:         return "UNREACHABLE (不可达)";
    case RDMA_CM_EVENT_REJECTED:            return "REJECTED (连接被拒绝)";
    case RDMA_CM_EVENT_ESTABLISHED:         return "ESTABLISHED (连接已建立)";
    case RDMA_CM_EVENT_DISCONNECTED:        return "DISCONNECTED (连接断开)";
    case RDMA_CM_EVENT_DEVICE_REMOVAL:      return "DEVICE_REMOVAL (设备移除)";
    case RDMA_CM_EVENT_MULTICAST_JOIN:      return "MULTICAST_JOIN (加入多播组)";
    case RDMA_CM_EVENT_MULTICAST_ERROR:     return "MULTICAST_ERROR (多播错误)";
    case RDMA_CM_EVENT_ADDR_CHANGE:         return "ADDR_CHANGE (地址变更)";
    case RDMA_CM_EVENT_TIMEWAIT_EXIT:       return "TIMEWAIT_EXIT (超时退出)";
    default:                                return "UNKNOWN (未知事件)";
    }
}

/* ========== 函数声明 ========== */

static void print_usage(const char *prog);
static int  parse_args(struct cm_context *cm, int argc, char *argv[]);
static int  create_qp_on_cm_id(struct cm_context *cm, struct rdma_cm_id *id);
static int  alloc_buffers(struct cm_context *cm);
static int  wait_for_event(struct cm_context *cm, enum rdma_cm_event_type expected,
                           struct rdma_cm_id **out_id);
static int  run_server(struct cm_context *cm);
static int  run_client(struct cm_context *cm);
static int  do_send_recv_test(struct cm_context *cm);
static void cleanup_cm(struct cm_context *cm);

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    struct cm_context cm;
    int ret = 1;

    memset(&cm, 0, sizeof(cm));

    /* 解析命令行参数 */
    if (parse_args(&cm, argc, argv) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    printf("========================================\n");
    printf("  RDMA CM 连接示例 (Send/Recv)\n");
    printf("  模式: %s\n", cm.is_server ? "服务器" : "客户端");
    printf("  端口: %d\n", cm.port);
    printf("========================================\n\n");

    /* 根据模式运行 */
    if (cm.is_server) {
        ret = run_server(&cm);
    } else {
        ret = run_client(&cm);
    }

    cleanup_cm(&cm);
    return ret;
}

/* ========== 打印用法 ========== */

static void print_usage(const char *prog)
{
    fprintf(stderr, "\n用法:\n");
    fprintf(stderr, "  服务器: %s -s <port>\n", prog);
    fprintf(stderr, "  客户端: %s -c <server_ip> <port>\n", prog);
    fprintf(stderr, "\n示例:\n");
    fprintf(stderr, "  %s -s 7471\n", prog);
    fprintf(stderr, "  %s -c 127.0.0.1 7471\n", prog);
    fprintf(stderr, "\n");
}

/* ========== 解析命令行参数 ========== */

static int parse_args(struct cm_context *cm, int argc, char *argv[])
{
    if (argc < 3) return -1;

    if (strcmp(argv[1], "-s") == 0) {
        cm->is_server = 1;
        cm->port = atoi(argv[2]);
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc < 4) return -1;
        cm->is_server = 0;
        cm->server_ip = argv[2];
        cm->port = atoi(argv[3]);
    } else {
        return -1;
    }

    return 0;
}

/* ========== 在 CM ID 上创建 QP ========== */

/**
 * create_qp_on_cm_id - 在指定的 rdma_cm_id 上创建 QP 和关联资源
 *
 * RDMA CM 的 QP 创建方式与直接使用 libibverbs 不同:
 *   - 使用 rdma_create_qp() 而非 ibv_create_qp()
 *   - QP 自动关联到 cm_id，CM 会自动管理 QP 状态转换
 *   - PD, CQ 需要先从 cm_id->verbs 获取设备上下文来创建
 */
static int create_qp_on_cm_id(struct cm_context *cm, struct rdma_cm_id *id)
{
    struct ibv_qp_init_attr qp_attr;
    int ret;

    /*
     * cm_id->verbs 是 CM 为我们分配的设备上下文。
     * 服务器端: 在收到 CONNECT_REQUEST 事件后，新 cm_id 上会有 verbs。
     * 客户端: 在 ADDR_RESOLVED 事件后，cm_id 上会有 verbs。
     */
    if (!id->verbs) {
        fprintf(stderr, "[错误] cm_id->verbs 为空，无法创建 QP\n");
        return -1;
    }

    /* 分配保护域 (PD) */
    cm->pd = ibv_alloc_pd(id->verbs);
    CM_CHECK_NULL(cm->pd, "分配保护域 (PD) 失败");

    /* 创建完成队列 (CQ) */
    cm->cq = ibv_create_cq(id->verbs, CQ_DEPTH, NULL, NULL, 0);
    CM_CHECK_NULL(cm->cq, "创建完成队列 (CQ) 失败");

    /* 创建 QP (通过 RDMA CM 接口) */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq  = cm->cq;
    qp_attr.recv_cq  = cm->cq;
    qp_attr.qp_type  = IBV_QPT_RC;         /* RC: 可靠连接 */
    qp_attr.cap.max_send_wr  = MAX_WR;
    qp_attr.cap.max_recv_wr  = MAX_WR;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    /*
     * rdma_create_qp() vs ibv_create_qp():
     *   - rdma_create_qp() 将 QP 绑定到 cm_id
     *   - 后续 rdma_accept() / rdma_connect() 时，CM 自动完成 QP 状态转换
     *   - 无需手动调用 ibv_modify_qp() 做 RESET→INIT→RTR→RTS
     */
    ret = rdma_create_qp(id, cm->pd, &qp_attr);
    CM_CHECK_ERRNO(ret, "rdma_create_qp 失败");

    cm->qp = id->qp;
    printf("  QP 创建成功: qp_num=%u (由 RDMA CM 管理)\n", cm->qp->qp_num);

    return 0;

cleanup:
    return -1;
}

/* ========== 分配并注册缓冲区 ========== */

static int alloc_buffers(struct cm_context *cm)
{
    /* 发送缓冲区 */
    cm->send_buf = (char *)malloc(BUFFER_SIZE);
    CM_CHECK_NULL(cm->send_buf, "分配发送缓冲区失败");
    memset(cm->send_buf, 0, BUFFER_SIZE);

    cm->send_mr = ibv_reg_mr(cm->pd, cm->send_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CM_CHECK_NULL(cm->send_mr, "注册发送 MR 失败");

    /* 接收缓冲区 */
    cm->recv_buf = (char *)malloc(BUFFER_SIZE);
    CM_CHECK_NULL(cm->recv_buf, "分配接收缓冲区失败");
    memset(cm->recv_buf, 0, BUFFER_SIZE);

    cm->recv_mr = ibv_reg_mr(cm->pd, cm->recv_buf, BUFFER_SIZE,
                              IBV_ACCESS_LOCAL_WRITE);
    CM_CHECK_NULL(cm->recv_mr, "注册接收 MR 失败");

    printf("  缓冲区注册完成: send_lkey=0x%x, recv_lkey=0x%x\n",
           cm->send_mr->lkey, cm->recv_mr->lkey);

    return 0;

cleanup:
    return -1;
}

/* ========== 等待 CM 事件 ========== */

/**
 * wait_for_event - 等待指定类型的 CM 事件
 *
 * @cm:       CM 上下文
 * @expected: 期望的事件类型
 * @out_id:   输出: 事件关联的 cm_id (可为 NULL)
 *
 * RDMA CM 使用异步事件模型:
 *   1. rdma_get_cm_event() 阻塞等待事件
 *   2. 处理事件 (检查类型、获取 cm_id 等)
 *   3. rdma_ack_cm_event() 确认事件 (必须调用!)
 *
 * 注意: rdma_ack_cm_event() 之后，event 指针不再有效。
 *       如果需要 event->id，必须在 ack 之前保存。
 */
static int wait_for_event(struct cm_context *cm,
                          enum rdma_cm_event_type expected,
                          struct rdma_cm_id **out_id)
{
    struct rdma_cm_event *event = NULL;
    int ret;

    printf("  等待 CM 事件: %s ...\n", cm_event_str(expected));

    /* 阻塞等待下一个 CM 事件 */
    ret = rdma_get_cm_event(cm->ec, &event);
    if (ret != 0) {
        fprintf(stderr, "[错误] rdma_get_cm_event 失败: %s\n", strerror(errno));
        return -1;
    }

    /* 打印收到的事件 */
    printf("  ★ 收到 CM 事件: %s\n", cm_event_str(event->event));

    /* 检查事件类型是否符合预期 */
    if (event->event != expected) {
        fprintf(stderr, "[错误] 期望事件 %s, 但收到 %s (status=%d)\n",
                cm_event_str(expected),
                cm_event_str(event->event),
                event->status);
        rdma_ack_cm_event(event);
        return -1;
    }

    /* 保存事件关联的 cm_id (在 ack 之前!) */
    if (out_id) {
        *out_id = event->id;
    }

    /*
     * 确认事件 —— 必须调用!
     * rdma_ack_cm_event() 释放事件资源。
     * 之后 event 指针不再有效。
     */
    rdma_ack_cm_event(event);

    return 0;
}

/* ========== 服务器流程 ========== */

/**
 * run_server - RDMA CM 服务器流程
 *
 * 服务器端的 CM 事件流:
 *   1. 创建事件通道 → 创建 cm_id → 绑定地址 → 监听
 *   2. 等待 CONNECT_REQUEST 事件 (客户端发起连接)
 *   3. 在新 cm_id 上创建 QP
 *   4. rdma_accept() 接受连接
 *   5. 等待 ESTABLISHED 事件 (连接建立完成)
 *   6. 数据通信
 */
static int run_server(struct cm_context *cm)
{
    struct sockaddr_in addr;
    struct rdma_conn_param conn_param;
    struct rdma_cm_id *new_cm_id = NULL;
    int ret;

    /* 第一步: 创建事件通道
     * 事件通道用于接收异步 CM 事件，类似于 epoll 的 fd。
     */
    printf("[步骤 1] 创建 RDMA CM 事件通道...\n");
    cm->ec = rdma_create_event_channel();
    CM_CHECK_NULL(cm->ec, "创建事件通道失败");
    printf("  事件通道创建成功\n");

    /* 第二步: 创建 CM ID (类似 socket)
     * RDMA_PS_TCP 表示使用 TCP 语义的连接模式 (RC QP)。
     * 也可以用 RDMA_PS_UDP 创建 UD QP。
     */
    printf("[步骤 2] 创建 RDMA CM ID...\n");
    ret = rdma_create_id(cm->ec, &cm->listen_id, NULL, RDMA_PS_TCP);
    CM_CHECK_ERRNO(ret, "rdma_create_id 失败");
    printf("  CM ID 创建成功 (RDMA_PS_TCP)\n");

    /* 第三步: 绑定地址 (类似 bind)
     * 绑定到 INADDR_ANY 和指定端口，接受来自任何地址的连接。
     */
    printf("[步骤 3] 绑定地址 (端口 %d)...\n", cm->port);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cm->port);

    ret = rdma_bind_addr(cm->listen_id, (struct sockaddr *)&addr);
    CM_CHECK_ERRNO(ret, "rdma_bind_addr 失败");
    printf("  绑定成功: 0.0.0.0:%d\n", cm->port);

    /* 第四步: 开始监听 (类似 listen)
     * backlog=1 表示最多 1 个待处理连接。
     */
    printf("[步骤 4] 开始监听连接请求...\n");
    ret = rdma_listen(cm->listen_id, 1);
    CM_CHECK_ERRNO(ret, "rdma_listen 失败");
    printf("  监听中，等待客户端连接...\n\n");

    /* 第五步: 等待 CONNECT_REQUEST 事件
     * 当客户端调用 rdma_connect() 后，服务器会收到此事件。
     * 事件中包含一个新的 cm_id (代表这个具体的连接)。
     * 注意: 不要在 listen_id 上创建 QP，而是在新 cm_id 上创建。
     */
    printf("[步骤 5] 等待客户端连接请求...\n");
    ret = wait_for_event(cm, RDMA_CM_EVENT_CONNECT_REQUEST, &new_cm_id);
    CM_CHECK_ERRNO(ret, "等待 CONNECT_REQUEST 失败");

    /* 保存新 cm_id 用于后续通信 */
    cm->cm_id = new_cm_id;

    /* 第六步: 在新 cm_id 上创建 QP 和分配缓冲区 */
    printf("[步骤 6] 创建 QP 和注册缓冲区...\n");
    ret = create_qp_on_cm_id(cm, cm->cm_id);
    CM_CHECK_ERRNO(ret, "创建 QP 失败");

    ret = alloc_buffers(cm);
    CM_CHECK_ERRNO(ret, "分配缓冲区失败");

    /* 第七步: 接受连接 (类似 accept)
     * rdma_accept() 会自动将 QP 状态转换为 RTR → RTS。
     * conn_param 可以传递私有数据 (最多 196 字节)。
     */
    printf("[步骤 7] 接受连接 (rdma_accept)...\n");
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;     /* 允许对方发起的 RDMA Read 数量 */
    conn_param.responder_resources = 1; /* 本端响应的 RDMA Read 数量 */

    ret = rdma_accept(cm->cm_id, &conn_param);
    CM_CHECK_ERRNO(ret, "rdma_accept 失败");

    /* 第八步: 等待 ESTABLISHED 事件 (连接正式建立) */
    printf("[步骤 8] 等待连接建立确认...\n");
    ret = wait_for_event(cm, RDMA_CM_EVENT_ESTABLISHED, NULL);
    CM_CHECK_ERRNO(ret, "等待 ESTABLISHED 失败");

    printf("\n  *** 连接已建立! QP 状态已由 CM 自动转换为 RTS ***\n\n");

    /* 第九步: 数据通信测试 */
    printf("[步骤 9] 执行 Send/Recv 通信测试...\n");
    ret = do_send_recv_test(cm);
    CM_CHECK_ERRNO(ret, "通信测试失败");

    printf("\n========================================\n");
    printf("  服务器: 全部测试通过!\n");
    printf("========================================\n");

    return 0;

cleanup:
    return 1;
}

/* ========== 客户端流程 ========== */

/**
 * run_client - RDMA CM 客户端流程
 *
 * 客户端的 CM 事件流:
 *   1. 创建事件通道 → 创建 cm_id
 *   2. rdma_resolve_addr() → 等待 ADDR_RESOLVED
 *   3. rdma_resolve_route() → 等待 ROUTE_RESOLVED
 *   4. 创建 QP
 *   5. rdma_connect() → 等待 ESTABLISHED
 *   6. 数据通信
 */
static int run_client(struct cm_context *cm)
{
    struct sockaddr_in addr;
    struct rdma_conn_param conn_param;
    int ret;

    /* 第一步: 创建事件通道 */
    printf("[步骤 1] 创建 RDMA CM 事件通道...\n");
    cm->ec = rdma_create_event_channel();
    CM_CHECK_NULL(cm->ec, "创建事件通道失败");

    /* 第二步: 创建 CM ID */
    printf("[步骤 2] 创建 RDMA CM ID...\n");
    ret = rdma_create_id(cm->ec, &cm->cm_id, NULL, RDMA_PS_TCP);
    CM_CHECK_ERRNO(ret, "rdma_create_id 失败");
    printf("  CM ID 创建成功\n");

    /* 第三步: 解析服务器地址
     * rdma_resolve_addr() 将 IP 地址解析为 RDMA 地址。
     * 对于 RoCE，这包括查找对应的网络接口和 GID。
     * 对于 IB，这包括查找 SM 获得路径信息。
     * 操作是异步的，结果通过 CM 事件通知。
     */
    printf("[步骤 3] 解析服务器地址: %s:%d ...\n", cm->server_ip, cm->port);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cm->port);
    if (inet_pton(AF_INET, cm->server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[错误] 无效的服务器 IP: %s\n", cm->server_ip);
        goto cleanup;
    }

    ret = rdma_resolve_addr(cm->cm_id, NULL, (struct sockaddr *)&addr, TIMEOUT_MS);
    CM_CHECK_ERRNO(ret, "rdma_resolve_addr 失败");

    /* 等待 ADDR_RESOLVED 事件 */
    ret = wait_for_event(cm, RDMA_CM_EVENT_ADDR_RESOLVED, NULL);
    CM_CHECK_ERRNO(ret, "地址解析失败");
    printf("  地址解析完成: 已找到 RDMA 设备和路径\n");

    /* 第四步: 解析路由
     * rdma_resolve_route() 确定到达目标的完整路径。
     * 对于 IB: 通过 SM (Subnet Manager) 获取路径记录。
     * 对于 RoCE: 通过系统路由表确定下一跳。
     */
    printf("[步骤 4] 解析路由...\n");
    ret = rdma_resolve_route(cm->cm_id, TIMEOUT_MS);
    CM_CHECK_ERRNO(ret, "rdma_resolve_route 失败");

    /* 等待 ROUTE_RESOLVED 事件 */
    ret = wait_for_event(cm, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL);
    CM_CHECK_ERRNO(ret, "路由解析失败");
    printf("  路由解析完成: 已确定到目标的路径\n");

    /* 第五步: 创建 QP 和分配缓冲区
     * 在路由解析完成后，cm_id->verbs 已可用。
     * 必须在 rdma_connect() 之前创建 QP。
     */
    printf("[步骤 5] 创建 QP 和注册缓冲区...\n");
    ret = create_qp_on_cm_id(cm, cm->cm_id);
    CM_CHECK_ERRNO(ret, "创建 QP 失败");

    ret = alloc_buffers(cm);
    CM_CHECK_ERRNO(ret, "分配缓冲区失败");

    /* 第六步: 发起连接
     * rdma_connect() 向服务器发送连接请求。
     * CM 会自动将 QP 转换到 INIT 和 RTR 状态。
     * 当服务器 rdma_accept() 后，QP 会自动转到 RTS。
     */
    printf("[步骤 6] 发起 RDMA 连接...\n");
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.retry_count = 7;         /* 重试次数 */
    conn_param.rnr_retry_count = 7;     /* RNR 重试次数 (7 = 无限) */

    ret = rdma_connect(cm->cm_id, &conn_param);
    CM_CHECK_ERRNO(ret, "rdma_connect 失败");

    /* 等待 ESTABLISHED 事件 (连接正式建立) */
    printf("[步骤 7] 等待连接建立确认...\n");
    ret = wait_for_event(cm, RDMA_CM_EVENT_ESTABLISHED, NULL);
    CM_CHECK_ERRNO(ret, "等待 ESTABLISHED 失败");

    printf("\n  *** 连接已建立! QP 状态已由 CM 自动转换为 RTS ***\n\n");

    /* 第七步: 数据通信测试 */
    printf("[步骤 8] 执行 Send/Recv 通信测试...\n");
    ret = do_send_recv_test(cm);
    CM_CHECK_ERRNO(ret, "通信测试失败");

    printf("\n========================================\n");
    printf("  客户端: 全部测试通过!\n");
    printf("========================================\n");

    return 0;

cleanup:
    return 1;
}

/* ========== 执行 Send/Recv 通信测试 ========== */

/**
 * do_send_recv_test - 双向 Send/Recv 测试
 *
 * 与手动建连版本的通信逻辑完全一致:
 *   1. 先 Post Recv (准备接收)
 *   2. Post Send (发送消息)
 *   3. Poll CQ 等待 Send 完成
 *   4. Poll CQ 等待 Recv 完成
 *   5. 打印接收到的消息
 *
 * 这说明: 一旦连接建立完成，不管是手动建连还是 CM 建连，
 * 后续的数据通路操作 (post_send/recv, poll_cq) 完全相同。
 */
static int do_send_recv_test(struct cm_context *cm)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    int ret, ne;
    const char *msg;

    /* 第一步: Post Recv (准备接收对方消息) */
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)cm->recv_buf;
    sge.length = BUFFER_SIZE;
    sge.lkey   = cm->recv_mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(cm->qp, &recv_wr, &bad_recv_wr);
    CM_CHECK_ERRNO(ret, "Post Recv 失败");
    printf("  Post Recv 完成 (等待对方消息)\n");

    /* 第二步: 准备并发送消息 */
    msg = cm->is_server ? SERVER_MSG : CLIENT_MSG;
    strncpy(cm->send_buf, msg, BUFFER_SIZE - 1);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)cm->send_buf;
    sge.length = (uint32_t)(strlen(msg) + 1);
    sge.lkey   = cm->send_mr->lkey;

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(cm->qp, &send_wr, &bad_send_wr);
    CM_CHECK_ERRNO(ret, "Post Send 失败");
    printf("  Post Send 完成 (发送: \"%s\")\n", msg);

    /* 第三步: Poll CQ 等待两个完成事件 (Send + Recv) */
    int completions = 0;
    while (completions < 2) {
        ne = ibv_poll_cq(cm->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[错误] ibv_poll_cq 失败\n");
            goto cleanup;
        }
        if (ne == 0) continue;

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[错误] WC 失败: %s (status=%d, wr_id=%lu)\n",
                    ibv_wc_status_str(wc.status), wc.status,
                    (unsigned long)wc.wr_id);
            goto cleanup;
        }

        if (wc.wr_id == 2) {
            printf("  Send 完成 (wr_id=%lu, bytes=%u)\n",
                   (unsigned long)wc.wr_id, wc.byte_len);
        } else if (wc.wr_id == 1) {
            printf("  Recv 完成 (wr_id=%lu, bytes=%u)\n",
                   (unsigned long)wc.wr_id, wc.byte_len);
        }

        completions++;
    }

    /* 第四步: 打印接收到的消息 */
    printf("\n  ╔══════════════════════════════════════════╗\n");
    printf("  ║  收到消息: \"%s\"\n", cm->recv_buf);
    printf("  ╚══════════════════════════════════════════╝\n");

    return 0;

cleanup:
    return -1;
}

/* ========== 清理资源 ========== */

static void cleanup_cm(struct cm_context *cm)
{
    printf("\n[清理] 释放 RDMA CM 资源...\n");

    /* 先释放 MR 和缓冲区 */
    if (cm->recv_mr)    { ibv_dereg_mr(cm->recv_mr);   cm->recv_mr = NULL; }
    if (cm->send_mr)    { ibv_dereg_mr(cm->send_mr);   cm->send_mr = NULL; }
    if (cm->recv_buf)   { free(cm->recv_buf);           cm->recv_buf = NULL; }
    if (cm->send_buf)   { free(cm->send_buf);           cm->send_buf = NULL; }

    /*
     * 释放 CM 管理的 QP:
     *   使用 rdma_destroy_qp() 而非 ibv_destroy_qp()
     *   因为 QP 是通过 rdma_create_qp() 创建的
     */
    if (cm->cm_id && cm->qp) {
        rdma_destroy_qp(cm->cm_id);
        cm->qp = NULL;
    }

    /* 释放 CQ 和 PD */
    if (cm->cq) { ibv_destroy_cq(cm->cq); cm->cq = NULL; }
    if (cm->pd) { ibv_dealloc_pd(cm->pd); cm->pd = NULL; }

    /* 断开连接并销毁 CM ID */
    if (cm->cm_id) {
        rdma_disconnect(cm->cm_id);
        rdma_destroy_id(cm->cm_id);
        cm->cm_id = NULL;
    }
    if (cm->listen_id) {
        rdma_destroy_id(cm->listen_id);
        cm->listen_id = NULL;
    }

    /* 销毁事件通道 */
    if (cm->ec) {
        rdma_destroy_event_channel(cm->ec);
        cm->ec = NULL;
    }

    printf("[清理] 完成\n");
}
