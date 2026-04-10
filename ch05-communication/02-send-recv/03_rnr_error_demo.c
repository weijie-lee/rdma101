/**
 * RNR (Receiver Not Ready) 错误演示
 *
 * 什么是 RNR 错误:
 *   当发送端 (Sender) 发出 RDMA Send 请求，但接收端 (Receiver) 没有预先
 *   ibv_post_recv()，NIC 无法找到匹配的 Recv WR，就会返回 RNR NAK。
 *
 * 本程序故意触发 RNR 错误:
 *   1. Client 端立即 ibv_post_send()，不等待 Server post_recv
 *   2. 设置 rnr_retry=0，使 QP 不会重试，立即报错
 *   3. 捕获 IBV_WC_RNR_RETRY_EXC_ERR 错误
 *
 * 如何避免 RNR 错误:
 *   - 接收端在发送端发送前 post 足够的 recv WR
 *   - 设置 min_rnr_timer (0~31) 控制 NAK 等待时间
 *   - 设置 rnr_retry (0~7, 7=无限重试) 控制重试次数
 *
 * 用法:
 *   服务器: ./03_rnr_error_demo server
 *   客户端: ./03_rnr_error_demo client <server_ip>
 *
 * 编译: gcc -Wall -O2 -g -o 03_rnr_error_demo 03_rnr_error_demo.c -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE     1024
#define TCP_PORT        19877

/* ========== RDMA 资源 ========== */
struct rnr_demo_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *mr;
    char                *buf;
    uint8_t              port;
    int                  is_roce;
};

/* ========== 初始化资源 ========== */
static int init_resources(struct rnr_demo_ctx *res)
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
    printf("[信息] 设备: %s\n", ibv_get_device_name(dev_list[0]));

    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[信息] 传输层: %s\n", transport_str(transport));

    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配 PD 失败");

    res->cq = ibv_create_cq(res->ctx, 64, NULL, NULL, 0);
    CHECK_NULL(res->cq, "创建 CQ 失败");

    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "创建 QP 失败");

    res->buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(res->buf, "分配缓冲区失败");
    memset(res->buf, 0, BUFFER_SIZE);

    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->mr, "注册 MR 失败");

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/**
 * 自定义 QP 建连 - 设置 rnr_retry=0 以快速触发错误
 *
 * 与正常建连的区别:
 *   - rnr_retry = 0: 收到 RNR NAK 后不重试，立即报错
 *   - 正常程序通常设置 rnr_retry = 7 (无限重试)
 */
static int qp_connect_rnr_demo(struct rnr_demo_ctx *res,
                               const struct rdma_endpoint *remote)
{
    struct ibv_qp_attr attr;
    int ret;

    /* RESET -> INIT */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.port_num        = res->port;
    attr.pkey_index      = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    ret = ibv_modify_qp(res->qp, &attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    CHECK_ERRNO_RETURN(ret, "QP RESET->INIT");

    /* INIT -> RTR */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = RDMA_DEFAULT_MTU;
    attr.dest_qp_num        = remote->qp_num;
    attr.rq_psn             = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 1;   /* 最短等待时间 (0.01ms) */

    attr.ah_attr.dlid     = remote->lid;
    attr.ah_attr.sl       = 0;
    attr.ah_attr.port_num = res->port;

    /* RoCE 模式: 启用全局路由 */
    if (res->is_roce) {
        attr.ah_attr.is_global          = 1;
        attr.ah_attr.grh.dgid           = remote->gid;
        attr.ah_attr.grh.sgid_index     = RDMA_DEFAULT_GID_INDEX;
        attr.ah_attr.grh.flow_label     = 0;
        attr.ah_attr.grh.hop_limit      = 64;
        attr.ah_attr.grh.traffic_class  = 0;
    }

    ret = ibv_modify_qp(res->qp, &attr,
                        IBV_QP_STATE | IBV_QP_PATH_MTU |
                        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                        IBV_QP_AV);
    CHECK_ERRNO_RETURN(ret, "QP INIT->RTR");

    /* RTR -> RTS: 关键 - rnr_retry=0 */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.sq_psn         = RDMA_DEFAULT_PSN;
    attr.timeout        = 14;
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 0;   /* ★ 关键: 不重试 RNR，立即报错 */
    attr.max_rd_atomic  = 1;

    printf("\n[关键参数] rnr_retry = 0 (不重试)，min_rnr_timer = 1 (0.01ms)\n");
    printf("  rnr_retry 含义:\n");
    printf("    0   = 不重试，立即报 IBV_WC_RNR_RETRY_EXC_ERR\n");
    printf("    1-6 = 重试 1-6 次\n");
    printf("    7   = 无限重试 (默认推荐值)\n\n");

    ret = ibv_modify_qp(res->qp, &attr,
                        IBV_QP_STATE | IBV_QP_SQ_PSN |
                        IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    CHECK_ERRNO_RETURN(ret, "QP RTR->RTS");

    printf("[信息] QP 建连完成 (rnr_retry=0)\n");
    return 0;
}

/* ========== Client: 立即发送，故意触发 RNR ========== */
static int client_trigger_rnr(struct rnr_demo_ctx *res)
{
    const char *msg = "RNR Test Message";
    size_t msg_len = strlen(msg) + 1;
    memcpy(res->buf, msg, msg_len);

    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = (uint32_t)msg_len,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 3001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad_wr = NULL;

    printf("[Client] 立即发送消息 (Server 尚未 post recv)...\n");
    printf("[Client] 这将触发 RNR 错误!\n\n");

    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] ibv_post_send 失败: %d\n", ret);
        return -1;
    }

    /* 等待完成 - 应该收到 RNR 错误 */
    struct ibv_wc wc;
    printf("[Client] 等待 CQ 完成...\n");
    ret = poll_cq_blocking(res->cq, &wc);

    printf("\n[Client] ========== WC 详细信息 ==========\n");
    print_wc_detail(&wc);

    if (wc.status == IBV_WC_RNR_RETRY_EXC_ERR) {
        printf("\n[Client] ★★★ 成功捕获 RNR 错误! ★★★\n");
        printf("[Client] 错误码: IBV_WC_RNR_RETRY_EXC_ERR (%d)\n", wc.status);
        printf("[Client] 含义: 对端 Receive Queue 为空，没有可用的 Recv WR\n");
        printf("[Client] 原因: Server 没有调用 ibv_post_recv()\n");
        printf("\n[Client] === 如何修复 RNR 错误 ===\n");
        printf("  1. 接收端提前 post 足够的 recv WR\n");
        printf("  2. 增大 rnr_retry 值 (7=无限重试)\n");
        printf("  3. 增大 min_rnr_timer 给接收端更多准备时间\n");
        printf("  4. 使用 SRQ (Shared Receive Queue) 集中管理 recv WR\n");
    } else if (wc.status == IBV_WC_SUCCESS) {
        printf("\n[Client] 意外: 发送成功 (Server 可能已经 post recv 了)\n");
    } else {
        printf("\n[Client] 收到其他错误: %s (%d)\n",
               ibv_wc_status_str(wc.status), wc.status);
    }

    return 0;
}

/* ========== 清理 ========== */
static void cleanup_resources(struct rnr_demo_ctx *res)
{
    if (res->mr)  ibv_dereg_mr(res->mr);
    if (res->buf) free(res->buf);
    if (res->qp)  ibv_destroy_qp(res->qp);
    if (res->cq)  ibv_destroy_cq(res->cq);
    if (res->pd)  ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s server|client [server_ip]\n", argv[0]);
        fprintf(stderr, "  服务器: %s server\n", argv[0]);
        fprintf(stderr, "  客户端: %s client <server_ip>\n", argv[0]);
        return 1;
    }

    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   RNR (Receiver Not Ready) 错误演示          ║\n");
    printf("║   角色: %-37s║\n", is_server ? "服务器" : "客户端");
    printf("╚══════════════════════════════════════════════╝\n\n");

    struct rnr_demo_ctx res;
    memset(&res, 0, sizeof(res));

    /* 1. 初始化资源 */
    if (init_resources(&res) != 0) {
        return 1;
    }

    /* 2. 交换端点信息 */
    struct rdma_endpoint local_ep, remote_ep;
    memset(&local_ep, 0, sizeof(local_ep));
    memset(&remote_ep, 0, sizeof(remote_ep));

    if (fill_local_endpoint(res.ctx, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &local_ep) != 0) {
        cleanup_resources(&res);
        return 1;
    }

    printf("[信息] TCP 信息交换 (端口 %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[错误] TCP 信息交换失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] 远端: QP=%u, LID=%u\n", remote_ep.qp_num, remote_ep.lid);

    /* 3. 建连 (rnr_retry=0) */
    if (qp_connect_rnr_demo(&res, &remote_ep) != 0) {
        cleanup_resources(&res);
        return 1;
    }

    /* 4. 角色动作 */
    if (is_server) {
        /*
         * Server: 故意不 post recv!
         * 正常情况下应该在这里调用 ibv_post_recv()，
         * 但为了演示 RNR 错误，我们什么都不做。
         */
        printf("\n[Server] ★ 故意不调用 ibv_post_recv()! ★\n");
        printf("[Server] 等待 Client 发送 (将触发 RNR 错误)...\n");
        printf("[Server] 等待 5 秒...\n");
        sleep(5);
        printf("[Server] 完成。Client 应该已经收到 RNR 错误了。\n");

        /* 打印 QP 状态 - RNR 错误后 QP 可能已转入 ERROR 状态 */
        print_qp_state(res.qp);
    } else {
        /* Client: 立即发送 */
        /* 给 Server 一点时间确保 QP 已建连但尚未 post recv */
        sleep(1);
        client_trigger_rnr(&res);

        /* 打印 QP 状态 - RNR 后 QP 转入 ERROR */
        printf("\n");
        print_qp_state(res.qp);
    }

    printf("\n[信息] 程序结束\n");
    cleanup_resources(&res);
    return 0;
}
