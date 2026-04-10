/**
 * 批量 RDMA Read 示例 —— 单次 ibv_post_send() 提交 4 个链式 RDMA Read WR
 *
 * 演示要点:
 *   - Server 准备 4KB 缓冲区，分为 4 个 1KB 块: "CHUNK-00:data...", "CHUNK-01:data...", ...
 *   - Client 创建 4 个 RDMA Read WR，通过 next 指针串联成链表:
 *       wr0.next = &wr1, wr1.next = &wr2, wr2.next = &wr3, wr3.next = NULL
 *   - 单次 ibv_post_send() 一次性提交全部 4 个 WR
 *   - 轮询 CQ 4 次获取每个 WR 的完成事件
 *   - 打印每个 chunk 的 wr_id 和读取到的内容
 *
 * 链式提交的优势:
 *   - 减少 ibv_post_send() 的调用次数 (用户态 → 内核态切换)
 *   - NIC 可以流水线处理多个请求，提升吞吐
 *   - 注意: max_rd_atomic 限制了同时在途的 RDMA Read 数量
 *
 * 用法:
 *   服务器: ./01_batch_read server
 *   客户端: ./01_batch_read client <server_ip>
 *
 * 编译:
 *   gcc -Wall -O2 -g -o 01_batch_read 01_batch_read.c \
 *       -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */
#define BUFFER_SIZE     4096        /* 总缓冲区大小: 4KB */
#define CHUNK_SIZE      1024        /* 每个块: 1KB */
#define NUM_CHUNKS      4           /* 块数量 */
#define TCP_PORT        19880       /* TCP 信息交换端口 */

/* ========== RDMA 资源上下文 ========== */
struct batch_read_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *mr;
    char                *buf;       /* 4KB 缓冲区 */
    uint8_t              port;
    int                  is_roce;   /* 自动检测: 0=IB, 1=RoCE */
};

/* ========== 初始化 RDMA 资源 ========== */
static int init_resources(struct batch_read_ctx *res)
{
    struct ibv_device **dev_list = NULL;
    int num_devices = 0;
    int ret = -1;

    /* 获取设备列表 */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取 RDMA 设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 没有找到任何 RDMA 设备\n");
        goto cleanup;
    }

    /* 打开第一个设备 */
    res->ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(res->ctx, "打开 RDMA 设备失败");
    printf("[信息] 打开设备: %s\n", ibv_get_device_name(dev_list[0]));

    /* 自动检测传输层类型 (IB / RoCE) */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[信息] 传输层类型: %s\n", transport_str(transport));

    /* 查询设备能力 —— 确认 max_qp_rd_atomic (同时在途的 RDMA Read 上限) */
    struct ibv_device_attr dev_attr;
    if (ibv_query_device(res->ctx, &dev_attr) == 0) {
        printf("[信息] max_qp_rd_atomic = %d (同时在途的 RDMA Read/Atomic 上限)\n",
               dev_attr.max_qp_rd_atom);
    }

    /* 分配保护域 */
    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配保护域失败");

    /* 创建完成队列 —— 至少能容纳 NUM_CHUNKS 个完成事件 */
    res->cq = ibv_create_cq(res->ctx, 128, NULL, NULL, 0);
    CHECK_NULL(res->cq, "创建完成队列失败");

    /* 创建 RC 队列对 */
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = res->cq;
    qp_attr.recv_cq = res->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = 16;     /* 至少 >= NUM_CHUNKS */
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_attr);
    CHECK_NULL(res->qp, "创建队列对失败");

    /* 分配并注册内存区域 */
    res->buf = (char *)malloc(BUFFER_SIZE);
    CHECK_NULL(res->buf, "分配缓冲区失败");
    memset(res->buf, 0, BUFFER_SIZE);

    /*
     * 访问权限说明:
     *   LOCAL_WRITE:  Client RDMA Read 结果需要写入本地 MR
     *   REMOTE_READ:  Server 端允许远端读取 (RDMA Read 的核心权限)
     *   REMOTE_WRITE: 预留，保持统一
     */
    res->mr = ibv_reg_mr(res->pd, res->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_READ |
                         IBV_ACCESS_REMOTE_WRITE);
    CHECK_NULL(res->mr, "注册内存区域失败");

    printf("[信息] QP num=%u, MR addr=%p, lkey=0x%x, rkey=0x%x\n",
           res->qp->qp_num, (void *)res->buf, res->mr->lkey, res->mr->rkey);

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== Server: 填充 4 个 1KB 数据块 ========== */
static void server_fill_chunks(char *buf)
{
    /*
     * 将 4KB 缓冲区分为 4 个 1KB 块，每块格式:
     *   "CHUNK-XX:data_xxxxxxxx..."
     * 其中 XX 为块编号 (00~03)，后面填充十六进制示例数据
     */
    printf("[Server] 填充 %d 个数据块 (每块 %d 字节):\n", NUM_CHUNKS, CHUNK_SIZE);

    for (int i = 0; i < NUM_CHUNKS; i++) {
        char *chunk = buf + i * CHUNK_SIZE;
        int offset = 0;

        /* 写入块头部标识: "CHUNK-XX:data_" */
        offset += snprintf(chunk + offset, CHUNK_SIZE - offset,
                           "CHUNK-%02d:data_", i);

        /* 填充可辨识的十六进制数据 */
        for (int j = 0; j < 120 && offset < CHUNK_SIZE - 1; j++) {
            offset += snprintf(chunk + offset, CHUNK_SIZE - offset,
                               "%02X", (unsigned char)((i * 100 + j) & 0xFF));
        }
        chunk[CHUNK_SIZE - 1] = '\0';  /* 确保以 null 结尾 */

        /* 打印前 60 个字符预览 */
        char preview[64];
        snprintf(preview, sizeof(preview), "%.60s", chunk);
        printf("  块 %d (偏移 %4d): \"%s...\"\n", i, i * CHUNK_SIZE, preview);
    }
    printf("\n");
}

/* ========== Client: 构建 4 个链式 WR 并批量提交 RDMA Read ========== */
static int client_batch_read(struct batch_read_ctx *res,
                             uint64_t remote_addr, uint32_t remote_rkey)
{
    /*
     * 链式 WR 提交原理:
     *   wr[0].next → &wr[1]
     *   wr[1].next → &wr[2]
     *   wr[2].next → &wr[3]
     *   wr[3].next → NULL
     *
     * 调用 ibv_post_send(qp, &wr[0], &bad_wr) 会一次性提交整条链。
     * HCA 按链表顺序入队处理，但完成顺序在高并发下不严格保证 (通常是顺序的)。
     */
    struct ibv_sge     sge[NUM_CHUNKS];
    struct ibv_send_wr wr[NUM_CHUNKS];

    memset(sge, 0, sizeof(sge));
    memset(wr, 0, sizeof(wr));

    printf("[Client] 构建 %d 个 RDMA Read WR (链式):\n", NUM_CHUNKS);

    for (int i = 0; i < NUM_CHUNKS; i++) {
        /* 每个 SGE 指向本地缓冲区的不同 1KB 区域 */
        sge[i].addr   = (uint64_t)(res->buf + i * CHUNK_SIZE);
        sge[i].length = CHUNK_SIZE;
        sge[i].lkey   = res->mr->lkey;

        /* 配置 RDMA Read WR */
        wr[i].wr_id      = (uint64_t)i;            /* 用块编号作为 wr_id */
        wr[i].opcode     = IBV_WR_RDMA_READ;
        wr[i].sg_list    = &sge[i];
        wr[i].num_sge    = 1;
        wr[i].send_flags = IBV_SEND_SIGNALED;       /* 每个 WR 都产生 CQ 完成 */

        /* 远端地址: 服务器缓冲区的第 i 个 1KB 块 */
        wr[i].wr.rdma.remote_addr = remote_addr + (uint64_t)(i * CHUNK_SIZE);
        wr[i].wr.rdma.rkey        = remote_rkey;

        /* 链式连接: 除最后一个外，都指向下一个 WR */
        wr[i].next = (i < NUM_CHUNKS - 1) ? &wr[i + 1] : NULL;

        printf("  WR[%d]: wr_id=%d, remote_offset=%d, local_offset=%d, "
               "size=%d, next=%s\n",
               i, i, i * CHUNK_SIZE, i * CHUNK_SIZE, CHUNK_SIZE,
               wr[i].next ? "&wr[next]" : "NULL");
    }

    /* 一次 ibv_post_send() 提交整条 WR 链 */
    struct ibv_send_wr *bad_wr = NULL;
    printf("\n[Client] 调用 ibv_post_send(&wr[0])，一次性提交 %d 个 WR...\n",
           NUM_CHUNKS);

    int ret = ibv_post_send(res->qp, &wr[0], &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "[错误] ibv_post_send 失败: ret=%d, errno=%d (%s)\n",
                ret, errno, strerror(errno));
        if (bad_wr) {
            fprintf(stderr, "  失败的 WR: wr_id=%lu\n",
                    (unsigned long)bad_wr->wr_id);
        }
        return -1;
    }
    printf("[Client] 提交成功!\n\n");

    /* 轮询 CQ，收集 4 个完成事件 */
    printf("[Client] 开始轮询 CQ，等待 %d 个完成...\n", NUM_CHUNKS);

    int completed = 0;
    int errors = 0;

    while (completed < NUM_CHUNKS) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(res->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[错误] ibv_poll_cq 返回 %d\n", ne);
            return -1;
        }
        if (ne == 0)
            continue;   /* CQ 暂无完成，继续轮询 */

        completed++;
        printf("\n--- 完成事件 #%d/%d ---\n", completed, NUM_CHUNKS);
        print_wc_detail(&wc);

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "  [失败] WR wr_id=%lu: %s\n",
                    (unsigned long)wc.wr_id, ibv_wc_status_str(wc.status));
            errors++;
            continue;
        }

        /* 打印该块读取到的内容 (前 72 字符预览) */
        int chunk_idx = (int)wc.wr_id;
        if (chunk_idx >= 0 && chunk_idx < NUM_CHUNKS) {
            char preview[80];
            snprintf(preview, sizeof(preview), "%.76s",
                     res->buf + chunk_idx * CHUNK_SIZE);
            printf("  [成功] wr_id=%d, byte_len=%u\n", chunk_idx, wc.byte_len);
            printf("  数据预览: \"%s...\"\n", preview);
        }
    }

    if (errors > 0) {
        fprintf(stderr, "\n[Client] %d 个 WR 执行失败!\n", errors);
        return -1;
    }

    return 0;
}

/* ========== 清理 RDMA 资源 ========== */
static void cleanup_resources(struct batch_read_ctx *res)
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

    printf("================================================\n");
    printf("  批量 RDMA Read —— 4 个链式 WR 单次提交\n");
    printf("  角色: %s\n", is_server ? "服务器 (提供数据)" : "客户端 (发起读取)");
    printf("  缓冲区: %d 字节 = %d 块 x %d 字节/块\n",
           BUFFER_SIZE, NUM_CHUNKS, CHUNK_SIZE);
    printf("================================================\n\n");

    /* 1. 初始化 RDMA 资源 */
    struct batch_read_ctx res;
    memset(&res, 0, sizeof(res));

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

    /* 附加 MR 信息: 远端需要知道我们的 buf 地址和 rkey */
    local_ep.buf_addr = (uint64_t)res.buf;
    local_ep.buf_rkey = res.mr->rkey;

    printf("[信息] 本地端点: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           local_ep.qp_num, local_ep.lid,
           (unsigned long)local_ep.buf_addr, local_ep.buf_rkey);

    /* Server 在交换信息之前先填充数据，确保 Client 连上后数据已就绪 */
    if (is_server) {
        printf("\n");
        server_fill_chunks(res.buf);
    }

    /* 3. TCP 带外交换端点信息 */
    printf("[信息] 通过 TCP 交换连接信息 (端口 %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[错误] TCP 信息交换失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] 远端端点: QP=%u, LID=%u, buf_addr=0x%lx, rkey=0x%x\n",
           remote_ep.qp_num, remote_ep.lid,
           (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);

    /* 4. QP 一键建连: RESET → INIT → RTR → RTS */
    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE;
    if (qp_full_connect(res.qp, &remote_ep, res.port, res.is_roce, access) != 0) {
        fprintf(stderr, "[错误] QP 建连失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] QP 状态: RESET → INIT → RTR → RTS (就绪)\n\n");

    /* 5. 执行角色相关操作 */
    if (is_server) {
        /*
         * 服务器端: 数据已准备好，等待客户端远程读取。
         * RDMA Read 完全由客户端发起，服务器端无需任何操作 (CPU bypass)。
         */
        printf("[Server] 数据已就绪，等待 Client 执行 RDMA Read...\n");
        printf("[Server] (Server 端无需任何操作 —— RDMA Read 完全由 Client 驱动)\n");
        printf("[Server] 等待 15 秒后退出...\n");
        sleep(15);

        /* 验证: RDMA Read 不会修改源端数据 */
        printf("\n[Server] 验证: 缓冲区内容未被改变 (RDMA Read 不修改源端):\n");
        for (int i = 0; i < NUM_CHUNKS; i++) {
            char preview[64];
            snprintf(preview, sizeof(preview), "%.60s",
                     res.buf + i * CHUNK_SIZE);
            printf("  块 %d: \"%s...\"\n", i, preview);
        }
        printf("[Server] 完成。\n");
    } else {
        /* 客户端: 等待 Server 数据就绪，然后批量 RDMA Read */
        printf("[Client] 等待 1 秒让 Server 准备就绪...\n");
        sleep(1);

        printf("[Client] 开始批量 RDMA Read:\n");
        printf("  远端: addr=0x%lx, rkey=0x%x\n",
               (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);
        printf("  策略: %d 个 WR 链式提交，每个读 %d 字节\n\n",
               NUM_CHUNKS, CHUNK_SIZE);

        if (client_batch_read(&res, remote_ep.buf_addr, remote_ep.buf_rkey) != 0) {
            fprintf(stderr, "[Client] 批量 Read 失败\n");
            cleanup_resources(&res);
            return 1;
        }

        /* 汇总打印所有块 */
        printf("\n================================================\n");
        printf("[Client] 批量 Read 完成! 所有 %d 个块的内容:\n", NUM_CHUNKS);
        printf("================================================\n");
        for (int i = 0; i < NUM_CHUNKS; i++) {
            char preview[80];
            snprintf(preview, sizeof(preview), "%.76s",
                     res.buf + i * CHUNK_SIZE);
            printf("  块 %d (偏移 %4d): \"%s...\"\n",
                   i, i * CHUNK_SIZE, preview);
        }
        printf("================================================\n");
    }

    /* 6. 清理资源 */
    printf("\n[信息] 清理 RDMA 资源...\n");
    cleanup_resources(&res);
    return 0;
}
