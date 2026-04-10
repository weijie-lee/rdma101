/**
 * RDMA Write with Immediate Data 示例
 *
 * 演示 IBV_WR_RDMA_WRITE_WITH_IMM 操作：
 *   - Client 使用 RDMA Write with Imm 向 Server 写入数据
 *   - Server 必须预先 ibv_post_recv() 才能接收 immediate data
 *   - Server 的 WC 中 opcode = IBV_WC_RECV_RDMA_WITH_IMM，且 wc.imm_data 被设置
 *   - imm_data (32-bit) 可作为 "通知信号"，告知 Server 数据已写入完成
 *
 * 对比普通 RDMA Write：
 *   - 普通 Write: Server 端完全无感知，没有任何 CQ 通知
 *   - Write with Imm: Server 端会收到一个带 imm_data 的 recv completion
 *   - 代价: Server 必须提前 post recv WR（消耗一个 recv WR 槽位）
 *
 * 用法:
 *   服务器: ./01_write_imm server
 *   客户端: ./01_write_imm client <server_ip>
 *
 * 编译: gcc -Wall -O2 -g -o 01_write_imm 01_write_imm.c -I../../common -L../../common -lrdma_utils -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE     4096
#define TCP_PORT        19876
#define IMM_MAGIC       0x12345678  /* 自定义立即数：表示 "写入完成" */

/* ========== RDMA 资源 ========== */
struct write_imm_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *mr;
    char                *buf;
    uint8_t              port;
    int                  is_roce;   /* 自动检测: 0=IB, 1=RoCE */
};

/* ========== 初始化 RDMA 资源 ========== */
static int init_resources(struct write_imm_ctx *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices = 0;
    int ret = -1;

    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取 RDMA 设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 没有找到任何 RDMA 设备\n");
        goto cleanup;
    }

    res->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(res->ctx, "打开 RDMA 设备失败");
    printf("[信息] 打开设备: %s\n", ibv_get_device_name(dev_list[0]));

    /* 检测传输层类型 */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[信息] 传输层类型: %s\n", transport_str(transport));

    /* 分配 PD */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配保护域失败");

    /* 创建 CQ */
    res->cq = ibv_create_cq(res->ctx, 128, NULL, NULL, 0);
    CHECK_NULL(res->cq, "创建完成队列失败");

    /* 创建 RC QP */
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
    CHECK_NULL(res->qp, "创建队列对失败");

    /* 分配并注册内存 */
    res->buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(res->buf, "分配缓冲区失败");
    memset(res->buf, 0, BUFFER_SIZE);

    /* Server 需要 REMOTE_WRITE 权限; Client 需要 LOCAL_WRITE */
    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    CHECK_NULL(res->mr, "注册内存区域失败");

    printf("[信息] QP num=%u, MR addr=%p, lkey=0x%x, rkey=0x%x\n",
           res->qp->qp_num, (void *)res->buf, res->mr->lkey, res->mr->rkey);

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Server: 预先 post recv 以接收 imm_data ========== */
static int server_post_recv(struct write_imm_ctx *res)
{
    /*
     * Write with Immediate 会消耗对端一个 recv WR。
     * Server 必须提前 post recv，否则会产生 RNR 错误。
     * recv WR 的 SGE 可以为空（length=0）因为数据是通过 RDMA Write 直接写入的，
     * 但这里我们仍然提供一个 SGE，以便在 WC 中看到 byte_len。
     */
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = BUFFER_SIZE,
        .lkey   = res->mr->lkey,
    };

    struct ibv_recv_wr wr = {
        .wr_id   = 1001,   /* 自定义 ID，方便在 WC 中识别 */
        .sg_list = &sge,
        .num_sge = 1,
    };

    struct ibv_recv_wr *bad_wr = NULL;
    int ret = ibv_post_recv(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] ibv_post_recv 失败: ret=%d errno=%d\n", ret, errno);
        return -1;
    }
    printf("[Server] 已 post recv (wr_id=1001)，等待 Write with Imm 通知\n");
    return 0;
}

/* ========== Client: 执行 RDMA Write with Immediate ========== */
static int client_write_imm(struct write_imm_ctx *res,
                            uint64_t remote_addr, uint32_t remote_rkey)
{
    /* 准备要写入的数据 */
    const char *msg = "Hello! 这是通过 RDMA Write with Immediate Data 写入的消息!";
    size_t msg_len = strlen(msg) + 1;
    memcpy(res->buf, msg, msg_len);

    struct ibv_sge sge = {
        .addr   = (uint64_t)res->buf,
        .length = (uint32_t)msg_len,
        .lkey   = res->mr->lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 2001;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE_WITH_IMM;  /* 关键: Write + Immediate */
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data   = htobe32(IMM_MAGIC);  /* 立即数 (网络字节序) */
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] ibv_post_send 失败: ret=%d errno=%d\n", ret, errno);
        return -1;
    }

    printf("[Client] 已发送 RDMA Write with Imm (imm_data=0x%X)\n", IMM_MAGIC);
    printf("[Client]   数据: \"%s\" (%zu 字节)\n", msg, msg_len);
    printf("[Client]   目标: remote_addr=0x%lx, rkey=0x%x\n",
           (unsigned long)remote_addr, remote_rkey);

    /* 等待发送完成 */
    struct ibv_wc wc;
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;

    printf("\n[Client] === 发送端 WC 详情 ===\n");
    print_wc_detail(&wc);

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[错误] 发送失败: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }

    printf("[Client] RDMA Write with Imm 完成!\n");
    return 0;
}

/* ========== Server: 等待并处理 Write with Imm 的 recv completion ========== */
static int server_wait_imm(struct write_imm_ctx *res)
{
    struct ibv_wc wc;
    int ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) return -1;

    printf("\n[Server] === 接收端 WC 详情 ===\n");
    print_wc_detail(&wc);

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[错误] 接收 WC 失败: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }

    /* 验证 opcode */
    if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        uint32_t imm = be32toh(wc.imm_data);
        printf("\n[Server] *** 收到 Write with Immediate 通知! ***\n");
        printf("[Server]   opcode = IBV_WC_RECV_RDMA_WITH_IMM (正确)\n");
        printf("[Server]   imm_data = 0x%X", imm);
        if (imm == IMM_MAGIC) {
            printf(" (匹配 IMM_MAGIC，确认写入完成)\n");
        } else {
            printf(" (未知立即数)\n");
        }
        printf("[Server]   wr_id = %lu\n", (unsigned long)wc.wr_id);
    } else {
        printf("[Server] 意外的 opcode: %d\n", wc.opcode);
    }

    /* 读取 RDMA Write 写入的数据 */
    printf("\n[Server] 缓冲区中的数据: \"%s\"\n", res->buf);

    return 0;
}

/* ========== 对比说明: 普通 RDMA Write ========== */
static void print_comparison(void)
{
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║          普通 RDMA Write  vs  RDMA Write with Immediate     ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ 特性           │ 普通 Write      │ Write with Imm           ║\n");
    printf("║────────────────┼─────────────────┼──────────────────────────║\n");
    printf("║ Server 通知    │ 无 (完全无感知) │ 有 (CQ 收到 recv WC)    ║\n");
    printf("║ Server post_rv │ 不需要          │ 必须预先 post_recv      ║\n");
    printf("║ WC opcode      │ 无              │ IBV_WC_RECV_RDMA_WITH_IM║\n");
    printf("║ imm_data       │ 无              │ 32-bit 自定义数据       ║\n");
    printf("║ 适用场景       │ 不需要通知      │ 需要写完成通知          ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

/* ========== 清理资源 ========== */
static void cleanup_resources(struct write_imm_ctx *res)
{
    if (res->mr)      ibv_dereg_mr(res->mr);
    if (res->buf)     free(res->buf);
    if (res->qp)      ibv_destroy_qp(res->qp);
    if (res->cq)      ibv_destroy_cq(res->cq);
    if (res->pd)      ibv_dealloc_pd(res->pd);
    if (res->ctx)     ibv_close_device(res->ctx);
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

    printf("========================================\n");
    printf("  RDMA Write with Immediate Data 示例\n");
    printf("  角色: %s\n", is_server ? "服务器" : "客户端");
    printf("========================================\n\n");

    print_comparison();

    struct write_imm_ctx res;
    memset(&res, 0, sizeof(res));

    /* 1. 初始化 RDMA 资源 */
    if (init_resources(&res) != 0) {
        fprintf(stderr, "[错误] 初始化 RDMA 资源失败\n");
        return 1;
    }

    /* 2. 填充本地端点信息 */
    struct rdma_endpoint local_ep, remote_ep;
    memset(&local_ep, 0, sizeof(local_ep));
    memset(&remote_ep, 0, sizeof(remote_ep));

    if (fill_local_endpoint(res.ctx, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &local_ep) != 0) {
        fprintf(stderr, "[错误] 填充本地端点信息失败\n");
        cleanup_resources(&res);
        return 1;
    }
    /* 附加 MR 信息供远端 RDMA Write 使用 */
    local_ep.buf_addr = (uint64_t)res.buf;
    local_ep.buf_rkey = res.mr->rkey;

    printf("[信息] 本地端点: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           local_ep.qp_num, local_ep.lid,
           (unsigned long)local_ep.buf_addr, local_ep.buf_rkey);

    /* 3. TCP 交换端点信息 */
    printf("\n[信息] 通过 TCP 交换连接信息 (端口 %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[错误] TCP 信息交换失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] 远端端点: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           remote_ep.qp_num, remote_ep.lid,
           (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);

    /* 4. Server: 在建连前先 post recv (QP 在 INIT 状态即可 post recv) */
    /*    先做 INIT 转换 */
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    if (qp_to_init(res.qp, res.port, access) != 0) {
        fprintf(stderr, "[错误] QP RESET->INIT 失败\n");
        cleanup_resources(&res);
        return 1;
    }

    if (is_server) {
        /* Server: INIT 状态下 post recv，准备接收 imm_data 通知 */
        if (server_post_recv(&res) != 0) {
            cleanup_resources(&res);
            return 1;
        }
    }

    /* 5. QP 状态转换: INIT -> RTR -> RTS */
    if (qp_to_rtr(res.qp, &remote_ep, res.port, res.is_roce) != 0) {
        fprintf(stderr, "[错误] QP INIT->RTR 失败\n");
        cleanup_resources(&res);
        return 1;
    }
    if (qp_to_rts(res.qp) != 0) {
        fprintf(stderr, "[错误] QP RTR->RTS 失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] QP 状态: RESET -> INIT -> RTR -> RTS (就绪)\n\n");

    /* 6. 执行操作 */
    if (is_server) {
        printf("[Server] 等待 Client 的 RDMA Write with Immediate...\n");
        if (server_wait_imm(&res) != 0) {
            fprintf(stderr, "[Server] 接收失败\n");
        }
    } else {
        /* Client: 向 Server 的 MR 执行 Write with Immediate */
        if (client_write_imm(&res, remote_ep.buf_addr, remote_ep.buf_rkey) != 0) {
            fprintf(stderr, "[Client] 写入失败\n");
        }
    }

    printf("\n[信息] 程序结束，清理资源...\n");
    cleanup_resources(&res);
    return 0;
}
