/**
 * UD (Unreliable Datagram) 模式 Loopback 演示
 *
 * 功能：
 *   - 展示 UD 模式的核心概念: 无连接、Address Handle、GRH 头部
 *   - 单机 Loopback: 创建两个 UD QP，一个发送，一个接收
 *   - 与 RC 模式对比，突出 UD 的关键区别
 *   - 自动检测 IB/RoCE 并适配 Address Handle 创建
 *
 * 用法：
 *   ./ud_loopback
 *
 * 编译：
 *   gcc -o ud_loopback ud_loopback.c -I../../common ../../common/librdma_utils.a -libverbs
 *
 * UD 与 RC 的关键区别：
 *   1. QP 类型: IBV_QPT_UD (不是 IBV_QPT_RC)
 *   2. RTR 转换更简单: 不需要 dest_qp_num, rq_psn 等 (无连接!)
 *   3. 发送时需要 Address Handle: wr.wr.ud.ah / remote_qpn / remote_qkey
 *   4. 接收缓冲区需额外 40 字节 GRH 头部 (网卡自动填充)
 *   5. 消息大小限制: 最大 MTU (通常 4096 字节)
 *   6. 一个 QP 可以向多个目标发送 (一对多)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */

#define MSG_SIZE        64              /* 消息大小 */
#define GRH_SIZE        40              /* GRH (Global Route Header) 固定 40 字节 */
#define RECV_BUF_SIZE   (GRH_SIZE + MSG_SIZE)   /* 接收缓冲区 = GRH + 消息 */
#define IB_PORT         1               /* RDMA 端口号 */
#define GID_INDEX       0               /* GID 表索引 */
#define CQ_DEPTH        16              /* CQ 深度 */
#define MAX_WR          16              /* QP 最大 WR 数 */
#define UD_QKEY         0x11111111      /* UD Q_Key (发送端和接收端必须匹配) */

/* 测试消息 */
#define TEST_MSG        "Hello UD mode! Unreliable Datagram works!"

/* ========== UD 资源结构体 ========== */

struct ud_context {
    /* 设备与保护域 */
    struct ibv_context      *ctx;           /* 设备上下文 */
    struct ibv_pd           *pd;            /* 保护域 (共享) */
    struct ibv_cq           *cq;            /* 完成队列 (共享) */

    /* 两个 UD QP: 一个发送，一个接收 (Loopback) */
    struct ibv_qp           *send_qp;       /* 发送端 QP */
    struct ibv_qp           *recv_qp;       /* 接收端 QP */

    /* 内存区域 */
    struct ibv_mr           *send_mr;       /* 发送缓冲区 MR */
    struct ibv_mr           *recv_mr;       /* 接收缓冲区 MR */
    char                    *send_buf;      /* 发送缓冲区 */
    char                    *recv_buf;      /* 接收缓冲区 (含 GRH) */

    /* Address Handle (UD 特有) */
    struct ibv_ah           *ah;            /* 地址句柄: 封装目标路由信息 */

    /* 传输层类型 */
    enum rdma_transport     transport;      /* IB / RoCE */
    int                     is_roce;        /* 是否为 RoCE 模式 */
};

/* ========== 函数声明 ========== */

static int  init_ud_resources(struct ud_context *uctx);
static int  create_ud_qp(struct ud_context *uctx, struct ibv_qp **qp);
static int  ud_qp_to_rts(struct ibv_qp *qp);
static int  create_address_handle(struct ud_context *uctx);
static int  do_ud_send_recv(struct ud_context *uctx);
static void cleanup_ud_resources(struct ud_context *uctx);

/* ========== 主函数 ========== */

int main(void)
{
    struct ud_context uctx;
    int ret = 1;

    memset(&uctx, 0, sizeof(uctx));

    printf("========================================\n");
    printf("  UD (Unreliable Datagram) Loopback 演示\n");
    printf("========================================\n\n");

    /* 第一步: 初始化 RDMA 资源 */
    printf("[步骤 1] 初始化 RDMA 资源...\n");
    if (init_ud_resources(&uctx) != 0) {
        fprintf(stderr, "[错误] RDMA 资源初始化失败\n");
        goto cleanup;
    }
    printf("[步骤 1] 资源初始化完成 ✓\n\n");

    /* 第二步: 创建两个 UD QP 并转换到 RTS */
    printf("[步骤 2] 创建 UD QP 并转换状态...\n");
    printf("  --- 创建发送端 QP ---\n");
    if (create_ud_qp(&uctx, &uctx.send_qp) != 0) {
        fprintf(stderr, "[错误] 创建发送端 QP 失败\n");
        goto cleanup;
    }
    printf("  发送端 QP 编号: %u\n", uctx.send_qp->qp_num);

    printf("  --- 创建接收端 QP ---\n");
    if (create_ud_qp(&uctx, &uctx.recv_qp) != 0) {
        fprintf(stderr, "[错误] 创建接收端 QP 失败\n");
        goto cleanup;
    }
    printf("  接收端 QP 编号: %u\n", uctx.recv_qp->qp_num);

    /*
     * UD QP 状态转换: RESET → INIT → RTR → RTS
     *
     * 与 RC 的关键区别:
     *   - INIT: qp_access_flags 用于设置 Q_Key
     *   - RTR:  不需要 dest_qp_num! (因为 UD 是无连接的)
     *   - RTS:  不需要 timeout, retry_cnt 等 (因为 UD 不保证可靠性)
     *
     * RC 的 RTR 需要:                    UD 的 RTR 只需要:
     *   attr.dest_qp_num = remote_qpn    attr.qp_state = IBV_QPS_RTR
     *   attr.path_mtu = ...              (就这么简单!)
     *   attr.rq_psn = ...
     *   attr.max_dest_rd_atomic = ...
     *   attr.ah_attr = ...
     */
    printf("  --- 转换发送端 QP 到 RTS ---\n");
    if (ud_qp_to_rts(uctx.send_qp) != 0) goto cleanup;
    print_qp_state(uctx.send_qp);

    printf("  --- 转换接收端 QP 到 RTS ---\n");
    if (ud_qp_to_rts(uctx.recv_qp) != 0) goto cleanup;
    print_qp_state(uctx.recv_qp);

    printf("[步骤 2] UD QP 创建并就绪 ✓\n\n");

    /* 第三步: 创建 Address Handle */
    printf("[步骤 3] 创建 Address Handle...\n");
    if (create_address_handle(&uctx) != 0) {
        fprintf(stderr, "[错误] 创建 Address Handle 失败\n");
        goto cleanup;
    }
    printf("[步骤 3] Address Handle 创建完成 ✓\n\n");

    /* 第四步: 执行发送/接收测试 */
    printf("[步骤 4] 执行 UD Send/Recv 测试...\n");
    if (do_ud_send_recv(&uctx) != 0) {
        fprintf(stderr, "[错误] UD Send/Recv 测试失败\n");
        goto cleanup;
    }
    printf("[步骤 4] 通信测试完成 ✓\n\n");

    printf("========================================\n");
    printf("  UD Loopback 测试通过!\n");
    printf("========================================\n");
    ret = 0;

cleanup:
    cleanup_ud_resources(&uctx);
    return ret;
}

/* ========== 初始化 RDMA 资源 ========== */

static int init_ud_resources(struct ud_context *uctx)
{
    struct ibv_device **dev_list = NULL;
    int num_devices;

    /* 1. 获取设备列表并打开设备 */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取 RDMA 设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未发现 RDMA 设备\n");
        goto cleanup;
    }
    printf("  发现 %d 个 RDMA 设备, 使用: %s\n",
           num_devices, ibv_get_device_name(dev_list[0]));

    uctx->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(uctx->ctx, "打开 RDMA 设备失败");

    /* 2. 检测传输层类型 */
    uctx->transport = detect_transport(uctx->ctx, IB_PORT);
    uctx->is_roce = (uctx->transport == RDMA_TRANSPORT_ROCE);
    printf("  传输类型: %s\n", transport_str(uctx->transport));

    /* 3. 分配保护域 (两个 QP 共享同一个 PD) */
    uctx->pd = ibv_alloc_pd(uctx->ctx);
    CHECK_NULL(uctx->pd, "分配保护域 (PD) 失败");

    /* 4. 创建完成队列 (两个 QP 共享同一个 CQ) */
    uctx->cq = ibv_create_cq(uctx->ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(uctx->cq, "创建完成队列 (CQ) 失败");

    /* 5. 分配并注册发送缓冲区 */
    uctx->send_buf = (char *)malloc(MSG_SIZE);
    CHECK_NULL(uctx->send_buf, "分配发送缓冲区失败");
    memset(uctx->send_buf, 0, MSG_SIZE);

    uctx->send_mr = ibv_reg_mr(uctx->pd, uctx->send_buf, MSG_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(uctx->send_mr, "注册发送 MR 失败");

    /*
     * 6. 分配并注册接收缓冲区
     *
     * *** UD 特有: 接收缓冲区需要额外 40 字节 GRH ***
     *
     * GRH (Global Route Header) 包含源/目的 GID 等路由信息。
     * UD 模式下，网卡会自动在每个收到的消息前面附加 40 字节 GRH。
     * 即使是 IB 模式 (不使用 GRH 路由)，这 40 字节也会被保留。
     *
     * 缓冲区布局:
     * ┌──────────────────┬──────────────────────┐
     * │  GRH (40 bytes)  │  实际数据 (payload)   │
     * └──────────────────┴──────────────────────┘
     *
     * 对比 RC 模式: 接收缓冲区只需要 payload 大小，没有 GRH 开销。
     */
    uctx->recv_buf = (char *)malloc(RECV_BUF_SIZE);
    CHECK_NULL(uctx->recv_buf, "分配接收缓冲区失败");
    memset(uctx->recv_buf, 0, RECV_BUF_SIZE);

    uctx->recv_mr = ibv_reg_mr(uctx->pd, uctx->recv_buf, RECV_BUF_SIZE,
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(uctx->recv_mr, "注册接收 MR 失败");

    printf("  缓冲区: send=%d bytes, recv=%d bytes (含 %d 字节 GRH)\n",
           MSG_SIZE, RECV_BUF_SIZE, GRH_SIZE);

    ibv_free_device_list(dev_list);
    return 0;

cleanup:
    if (dev_list) ibv_free_device_list(dev_list);
    return -1;
}

/* ========== 创建 UD QP ========== */

/**
 * create_ud_qp - 创建一个 UD 类型的 QP
 *
 * UD QP 与 RC QP 的创建区别:
 *   - qp_type = IBV_QPT_UD (不是 IBV_QPT_RC)
 *   - 其他参数 (CQ, max_wr, max_sge) 完全相同
 */
static int create_ud_qp(struct ud_context *uctx, struct ibv_qp **qp)
{
    struct ibv_qp_init_attr qp_init_attr;

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq  = uctx->cq;
    qp_init_attr.recv_cq  = uctx->cq;

    /*
     * *** UD 关键区别 #1: QP 类型 ***
     *
     * RC: qp_init_attr.qp_type = IBV_QPT_RC;  // Reliable Connected
     * UC: qp_init_attr.qp_type = IBV_QPT_UC;  // Unreliable Connected
     * UD: qp_init_attr.qp_type = IBV_QPT_UD;  // Unreliable Datagram
     */
    qp_init_attr.qp_type  = IBV_QPT_UD;

    qp_init_attr.cap.max_send_wr  = MAX_WR;
    qp_init_attr.cap.max_recv_wr  = MAX_WR;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    *qp = ibv_create_qp(uctx->pd, &qp_init_attr);
    CHECK_NULL(*qp, "创建 UD QP 失败");

    return 0;

cleanup:
    return -1;
}

/* ========== UD QP 状态转换: RESET → INIT → RTR → RTS ========== */

/**
 * ud_qp_to_rts - 将 UD QP 从 RESET 转换到 RTS
 *
 * UD 模式的 QP 状态转换比 RC 简单得多:
 *
 *   RC 模式:                           UD 模式:
 *   ─────────                           ─────────
 *   RESET → INIT:                       RESET → INIT:
 *     port, pkey, access_flags            port, pkey, qkey (!)
 *                                         (access_flags 不需要远端访问)
 *
 *   INIT → RTR:                         INIT → RTR:
 *     dest_qp_num (!)                     只需 qp_state = RTR
 *     path_mtu                            (不需要任何目标信息!)
 *     rq_psn
 *     max_dest_rd_atomic
 *     ah_attr (目标地址!)
 *
 *   RTR → RTS:                          RTR → RTS:
 *     sq_psn                              sq_psn
 *     timeout (!)                         (不需要 timeout, retry_cnt)
 *     retry_cnt (!)                       (因为 UD 不保证可靠性)
 *     rnr_retry (!)
 *     max_rd_atomic
 */
static int ud_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    int ret;

    /* 第一步: RESET → INIT */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state   = IBV_QPS_INIT;
    attr.pkey_index = RDMA_DEFAULT_PKEY_INDEX;
    attr.port_num   = IB_PORT;
    /*
     * UD 特有: INIT 阶段需要设置 Q_Key
     * Q_Key 用于 UD 消息的访问控制:
     *   - 发送端的 wr.wr.ud.remote_qkey 必须与接收端 QP 的 qkey 匹配
     *   - 不匹配的消息会被丢弃
     */
    attr.qkey       = UD_QKEY;

    ret = ibv_modify_qp(qp, &attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT | IBV_QP_QKEY);
    CHECK_ERRNO(ret, "UD QP RESET→INIT 失败");
    printf("    RESET → INIT (qkey=0x%x)\n", UD_QKEY);

    /*
     * 第二步: INIT → RTR
     *
     * *** UD 关键区别 #2: RTR 不需要目标信息! ***
     *
     * RC 的 RTR 需要指定:                UD 的 RTR 只需要:
     *   dest_qp_num = remote_qpn          qp_state = IBV_QPS_RTR
     *   path_mtu = IBV_MTU_1024           (结束!)
     *   rq_psn = remote_psn
     *   max_dest_rd_atomic = 1
     *   ah_attr = { dlid/dgid... }
     *
     * 这正是 UD "无连接" 的体现: QP 不绑定到特定目标。
     */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;

    ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE);
    CHECK_ERRNO(ret, "UD QP INIT→RTR 失败");
    printf("    INIT → RTR (无需目标信息, UD 是无连接的!)\n");

    /*
     * 第三步: RTR → RTS
     *
     * UD 的 RTS 比 RC 简单:
     *   - 只需要 sq_psn (Send Queue PSN)
     *   - 不需要 timeout, retry_cnt, rnr_retry (UD 不做重传)
     */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn   = RDMA_DEFAULT_PSN;

    ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
    CHECK_ERRNO(ret, "UD QP RTR→RTS 失败");
    printf("    RTR → RTS (sq_psn=%u)\n", RDMA_DEFAULT_PSN);

    return 0;

cleanup:
    return -1;
}

/* ========== 创建 Address Handle ========== */

/**
 * create_address_handle - 创建 UD 模式的 Address Handle
 *
 * Address Handle (AH) 是 UD 模式的核心概念:
 *   - RC 模式: 目标路由信息绑定在 QP 的 RTR 属性中 (一对一)
 *   - UD 模式: 目标路由信息封装在 AH 中，发送时指定 (一对多)
 *
 * AH 包含:
 *   - dlid:       目标 LID (IB 模式)
 *   - is_global:  是否使用 GRH (RoCE 必须为 1)
 *   - grh.dgid:   目标 GID (RoCE 模式)
 *   - sl:         Service Level
 *   - port_num:   本地出端口
 *
 * 本例是 Loopback，所以 AH 指向自身 (自己的 LID/GID)。
 */
static int create_address_handle(struct ud_context *uctx)
{
    struct ibv_ah_attr ah_attr;
    struct ibv_port_attr port_attr;
    union ibv_gid local_gid;

    /* 查询本地端口属性以获取 LID */
    if (ibv_query_port(uctx->ctx, IB_PORT, &port_attr) != 0) {
        fprintf(stderr, "[错误] 查询端口属性失败\n");
        goto cleanup;
    }

    /* 查询本地 GID */
    if (ibv_query_gid(uctx->ctx, IB_PORT, GID_INDEX, &local_gid) != 0) {
        fprintf(stderr, "[错误] 查询 GID[%d] 失败\n", GID_INDEX);
        goto cleanup;
    }

    /* 构建 AH 属性 */
    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.sl       = RDMA_DEFAULT_SL;
    ah_attr.port_num = IB_PORT;

    if (uctx->is_roce) {
        /*
         * RoCE 模式: 必须设置 is_global=1 并填充 GRH
         *
         * Loopback: dgid = 本地 GID (发给自己)
         * 实际使用: dgid = 远端 GID (发给对方)
         */
        ah_attr.is_global         = 1;
        ah_attr.grh.dgid          = local_gid;     /* Loopback: 目标 = 自己 */
        ah_attr.grh.sgid_index    = GID_INDEX;
        ah_attr.grh.hop_limit     = 64;
        ah_attr.grh.traffic_class = 0;
        ah_attr.grh.flow_label    = 0;

        char gid_str[46];
        gid_to_str(&local_gid, gid_str, sizeof(gid_str));
        printf("  RoCE 模式 AH: dgid=%s\n", gid_str);
    } else {
        /*
         * IB 模式: 使用 LID 寻址
         *
         * Loopback: dlid = 本地 LID (发给自己)
         * 实际使用: dlid = 远端 LID (发给对方)
         */
        ah_attr.is_global = 0;
        ah_attr.dlid      = port_attr.lid;          /* Loopback: 目标 = 自己 */

        printf("  IB 模式 AH: dlid=%u\n", port_attr.lid);
    }

    /* 创建 Address Handle */
    uctx->ah = ibv_create_ah(uctx->pd, &ah_attr);
    CHECK_NULL(uctx->ah, "创建 Address Handle (AH) 失败");
    printf("  Address Handle 创建成功\n");

    return 0;

cleanup:
    return -1;
}

/* ========== UD Send/Recv 测试 ========== */

/**
 * do_ud_send_recv - 执行 UD 模式的 Send/Recv 测试
 *
 * 流程:
 *   1. 在接收端 QP 上 Post Recv (缓冲区大小 = GRH + MSG)
 *   2. 在发送端 QP 上 Post Send (指定 AH, remote_qpn, remote_qkey)
 *   3. Poll CQ 等待 Send 完成
 *   4. Poll CQ 等待 Recv 完成
 *   5. 读取数据 (跳过前 40 字节 GRH)
 */
static int do_ud_send_recv(struct ud_context *uctx)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    struct ibv_wc wc;
    int ret;

    /* ---- 第一步: 在接收端 QP 上 Post Recv ---- */
    /*
     * *** UD 关键区别 #4: 接收缓冲区需要额外 40 字节 GRH ***
     *
     * RC 模式: sge.length = MSG_SIZE;        // 只需要消息大小
     * UD 模式: sge.length = GRH_SIZE + MSG_SIZE;  // 前 40 字节放 GRH
     *
     * 如果接收缓冲区没有预留 GRH 空间，网卡会因为缓冲区太小而丢弃消息。
     */
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)uctx->recv_buf;
    sge.length = RECV_BUF_SIZE;             /* GRH(40) + MSG_SIZE */
    sge.lkey   = uctx->recv_mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(uctx->recv_qp, &recv_wr, &bad_recv_wr);
    CHECK_ERRNO(ret, "Post Recv 失败");
    printf("  Post Recv 完成 (接收端 QP #%u, 缓冲区 %d 字节)\n",
           uctx->recv_qp->qp_num, RECV_BUF_SIZE);

    /* ---- 第二步: 准备发送数据 ---- */
    strncpy(uctx->send_buf, TEST_MSG, MSG_SIZE - 1);

    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uint64_t)uctx->send_buf;
    sge.length = (uint32_t)(strlen(TEST_MSG) + 1);
    sge.lkey   = uctx->send_mr->lkey;

    /*
     * *** UD 关键区别 #3: Send WR 需要指定 Address Handle ***
     *
     * RC 模式的 Send WR:               UD 模式的 Send WR:
     *   wr.opcode = IBV_WR_SEND          wr.opcode = IBV_WR_SEND
     *   (不需要指定目标,                   wr.wr.ud.ah = ah;          // 目标路由
     *    因为 QP 已绑定目标)               wr.wr.ud.remote_qpn = N;  // 目标 QP
     *                                     wr.wr.ud.remote_qkey = K;  // Q_Key 匹配
     */
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.opcode     = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    /* UD 特有: 设置目标信息 */
    send_wr.wr.ud.ah          = uctx->ah;              /* 地址句柄 (目标路由) */
    send_wr.wr.ud.remote_qpn  = uctx->recv_qp->qp_num; /* 目标 QP 编号 */
    send_wr.wr.ud.remote_qkey = UD_QKEY;               /* Q_Key (必须匹配) */

    /* ---- 第三步: Post Send ---- */
    ret = ibv_post_send(uctx->send_qp, &send_wr, &bad_send_wr);
    CHECK_ERRNO(ret, "Post Send 失败");
    printf("  Post Send 完成 (发送端 QP #%u → 接收端 QP #%u)\n",
           uctx->send_qp->qp_num, uctx->recv_qp->qp_num);
    printf("    AH 目标: %s, remote_qpn=%u, remote_qkey=0x%x\n",
           uctx->is_roce ? "GID" : "LID",
           uctx->recv_qp->qp_num, UD_QKEY);

    /* ---- 第四步: Poll CQ 等待 Send 完成 ---- */
    printf("\n  等待 Send 完成...\n");
    ret = poll_cq_blocking(uctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Send) 失败");
    printf("  Send 完成: wr_id=%lu, status=%s\n",
           (unsigned long)wc.wr_id, ibv_wc_status_str(wc.status));
    print_wc_detail(&wc);

    /* ---- 第五步: Poll CQ 等待 Recv 完成 ---- */
    printf("  等待 Recv 完成...\n");
    ret = poll_cq_blocking(uctx->cq, &wc);
    CHECK_ERRNO(ret, "Poll CQ (Recv) 失败");
    printf("  Recv 完成: wr_id=%lu, byte_len=%u\n",
           (unsigned long)wc.wr_id, wc.byte_len);

    /*
     * UD Recv WC 的额外信息:
     *   wc.src_qp:  发送端的 QP 编号
     *   wc.slid:    发送端的 LID (IB 模式)
     *   wc.byte_len: 包含 GRH 的总长度
     */
    printf("  Recv WC 详情: src_qp=%u, slid=%u, total_bytes=%u (含 GRH %d)\n",
           wc.src_qp, wc.slid, wc.byte_len, GRH_SIZE);
    print_wc_detail(&wc);

    /* ---- 第六步: 读取数据 (跳过 GRH) ---- */
    /*
     * *** UD 关键区别 #4: 实际数据从偏移 40 开始 ***
     *
     * RC 模式: payload = recv_buf;           // 直接从缓冲区头开始
     * UD 模式: payload = recv_buf + 40;      // 跳过 40 字节 GRH
     *
     * GRH 的内容 (40 字节):
     *   - 版本、流量类、流标签 (4 字节)
     *   - 载荷长度、下一个头部、跳数限制 (4 字节)
     *   - 源 GID (16 字节)
     *   - 目的 GID (16 字节)
     */
    char *payload = uctx->recv_buf + GRH_SIZE;
    printf("\n  ╔══════════════════════════════════════════════╗\n");
    printf("  ║  [GRH 头部: %d 字节] (自动跳过)\n", GRH_SIZE);
    printf("  ║  收到消息: \"%s\"\n", payload);
    printf("  ╚══════════════════════════════════════════════╝\n");

    /* 验证消息内容 */
    if (strcmp(payload, TEST_MSG) == 0) {
        printf("\n  消息验证成功! UD Loopback 工作正常。\n");
    } else {
        fprintf(stderr, "\n  [错误] 消息不匹配! 期望: \"%s\", 收到: \"%s\"\n",
                TEST_MSG, payload);
        goto cleanup;
    }

    return 0;

cleanup:
    return -1;
}

/* ========== 清理资源 ========== */

static void cleanup_ud_resources(struct ud_context *uctx)
{
    printf("\n[清理] 释放 UD 资源...\n");

    /* Address Handle */
    if (uctx->ah)       { ibv_destroy_ah(uctx->ah);       uctx->ah = NULL; }

    /* MR 和缓冲区 */
    if (uctx->recv_mr)  { ibv_dereg_mr(uctx->recv_mr);    uctx->recv_mr = NULL; }
    if (uctx->send_mr)  { ibv_dereg_mr(uctx->send_mr);    uctx->send_mr = NULL; }
    if (uctx->recv_buf) { free(uctx->recv_buf);            uctx->recv_buf = NULL; }
    if (uctx->send_buf) { free(uctx->send_buf);            uctx->send_buf = NULL; }

    /* QP */
    if (uctx->send_qp)  { ibv_destroy_qp(uctx->send_qp);  uctx->send_qp = NULL; }
    if (uctx->recv_qp)  { ibv_destroy_qp(uctx->recv_qp);  uctx->recv_qp = NULL; }

    /* CQ, PD, 设备 */
    if (uctx->cq)        { ibv_destroy_cq(uctx->cq);       uctx->cq = NULL; }
    if (uctx->pd)        { ibv_dealloc_pd(uctx->pd);       uctx->pd = NULL; }
    if (uctx->ctx)       { ibv_close_device(uctx->ctx);    uctx->ctx = NULL; }

    printf("[清理] 完成\n");
}
