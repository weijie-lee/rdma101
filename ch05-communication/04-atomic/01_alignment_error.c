/**
 * 原子操作对齐错误演示
 *
 * RDMA 原子操作 (Fetch-and-Add, Compare-and-Swap) 的硬性要求:
 *   ★ 目标远端地址必须 8 字节对齐 (因为操作的是 uint64_t, 即 8 字节)
 *   ★ 如果地址未对齐，NIC 硬件无法执行原子操作，返回错误 WC
 *
 * 本程序分两个实验:
 *   实验 1: 故意用 malloc + 偏移 3 字节，制造非 8 字节对齐的地址
 *           → 注册 MR，执行 FAA → 捕获错误 WC，用 print_wc_detail() 打印
 *   实验 2: 用 posix_memalign(64, sizeof(uint64_t)) 分配正确对齐的内存
 *           → 注册新 MR，执行 FAA → 成功，打印旧值
 *
 * 运行模式: server/client 双进程 (loopback 模式，通过 TCP 交换端点信息)
 *
 * 用法:
 *   终端 1: ./01_alignment_error server
 *   终端 2: ./01_alignment_error client [server_ip]
 *
 * 编译:
 *   gcc -Wall -O2 -g -o 01_alignment_error 01_alignment_error.c \
 *       -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 常量定义 ========== */
#define RAW_BUF_SIZE    128         /* 原始缓冲区大小 (用于故意偏移) */
#define TCP_PORT        19885       /* TCP 信息交换端口 */
#define MISALIGN_OFFSET 3           /* 故意偏移 3 字节，破坏 8 字节对齐 */

/* ========== RDMA 资源上下文 ========== */
struct alignment_ctx {
    struct ibv_context  *ctx;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;

    /* 故意未对齐的内存 (实验 1) */
    char                *raw_buf;       /* malloc 原始指针 */
    struct ibv_mr       *raw_mr;

    /* 正确对齐的内存 (实验 2) */
    uint64_t            *aligned_buf;
    struct ibv_mr       *aligned_mr;

    /* FAA 操作结果缓冲区 (本地写回旧值) */
    uint64_t            *result_buf;
    struct ibv_mr       *result_mr;

    uint8_t              port;
    int                  is_roce;
};

/* ========== 初始化 RDMA 资源 ========== */
static int init_resources(struct alignment_ctx *res)
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

    /* 自动检测传输层: IB 或 RoCE */
    res->port = RDMA_DEFAULT_PORT_NUM;
    enum rdma_transport transport = detect_transport(res->ctx, res->port);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("[信息] 传输层类型: %s\n", transport_str(transport));

    res->pd = ibv_alloc_pd(res->ctx);
    CHECK_NULL(res->pd, "分配保护域失败");

    res->cq = ibv_create_cq(res->ctx, 64, NULL, NULL, 0);
    CHECK_NULL(res->cq, "创建完成队列失败");

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

    /*
     * 分配原始缓冲区 (用于实验 1: 故意偏移制造未对齐地址)
     * 注意: ibv_reg_mr 注册的是整块缓冲区，但原子操作的目标地址在缓冲区内偏移
     */
    res->raw_buf = (char *)malloc(RAW_BUF_SIZE);
    CHECK_NULL(res->raw_buf, "分配 raw_buf 失败");
    memset(res->raw_buf, 0, RAW_BUF_SIZE);

    /* 注册 MR 时必须包含 REMOTE_ATOMIC 权限 */
    res->raw_mr = ibv_reg_mr(res->pd, res->raw_buf, RAW_BUF_SIZE,
                             IBV_ACCESS_LOCAL_WRITE |
                             IBV_ACCESS_REMOTE_WRITE |
                             IBV_ACCESS_REMOTE_READ |
                             IBV_ACCESS_REMOTE_ATOMIC);
    CHECK_NULL(res->raw_mr, "注册 raw MR 失败");

    /*
     * 分配正确对齐的内存 (实验 2)
     * posix_memalign: 第一个参数是对齐边界 (64 字节，满足 8 字节要求)
     *                 第二个参数是分配大小
     */
    if (posix_memalign((void **)&res->aligned_buf, 64, sizeof(uint64_t)) != 0) {
        fprintf(stderr, "[错误] posix_memalign(aligned_buf) 失败\n");
        goto cleanup;
    }
    *res->aligned_buf = 0;  /* 初始化为 0 */

    res->aligned_mr = ibv_reg_mr(res->pd, res->aligned_buf, sizeof(uint64_t),
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_WRITE |
                                 IBV_ACCESS_REMOTE_READ |
                                 IBV_ACCESS_REMOTE_ATOMIC);
    CHECK_NULL(res->aligned_mr, "注册 aligned MR 失败");

    /* FAA 结果缓冲区: 本地写回旧值 */
    if (posix_memalign((void **)&res->result_buf, 64, sizeof(uint64_t)) != 0) {
        fprintf(stderr, "[错误] posix_memalign(result_buf) 失败\n");
        goto cleanup;
    }
    *res->result_buf = 0;

    res->result_mr = ibv_reg_mr(res->pd, res->result_buf, sizeof(uint64_t),
                                IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(res->result_mr, "注册 result MR 失败");

    ret = 0;

cleanup:
    if (dev_list)
        ibv_free_device_list(dev_list);
    return ret;
}

/* ========== 执行 FAA 操作并打印结果 ========== */
static int do_faa(struct alignment_ctx *res, uint64_t target_addr,
                  uint32_t target_rkey, uint64_t add_val, const char *desc)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)res->result_buf,
        .length = sizeof(uint64_t),
        .lkey   = res->result_mr->lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_ATOMIC_FETCH_AND_ADD;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = target_addr;
    wr.wr.atomic.rkey        = target_rkey;
    wr.wr.atomic.compare_add = add_val;

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(res->qp, &wr, &bad_wr);
    if (ret != 0) {
        fprintf(stderr, "  [错误] ibv_post_send 失败: ret=%d, errno=%d\n", ret, errno);
        return -1;
    }

    /* 阻塞等待完成 */
    struct ibv_wc wc;
    ret = poll_cq_blocking(res->cq, &wc);
    if (ret != 0) {
        fprintf(stderr, "  [错误] poll_cq_blocking 失败\n");
        return -1;
    }

    /* 用 print_wc_detail() 打印完整的 WC 信息 */
    printf("  %s —— WC 详情:\n", desc);
    print_wc_detail(&wc);

    if (wc.status == IBV_WC_SUCCESS) {
        printf("  [成功] FAA +%lu, 返回旧值 = %lu\n\n",
               (unsigned long)add_val, (unsigned long)*res->result_buf);
        return 0;
    } else {
        printf("  [失败] 状态: %s (code=%d)\n\n",
               ibv_wc_status_str(wc.status), wc.status);
        return -1;
    }
}

/* ========== 清理 RDMA 资源 ========== */
static void cleanup_resources(struct alignment_ctx *res)
{
    if (res->result_mr)  ibv_dereg_mr(res->result_mr);
    if (res->aligned_mr) ibv_dereg_mr(res->aligned_mr);
    if (res->raw_mr)     ibv_dereg_mr(res->raw_mr);
    if (res->result_buf) free(res->result_buf);
    if (res->aligned_buf) free(res->aligned_buf);
    if (res->raw_buf)    free(res->raw_buf);
    if (res->qp)         ibv_destroy_qp(res->qp);
    if (res->cq)         ibv_destroy_cq(res->cq);
    if (res->pd)         ibv_dealloc_pd(res->pd);
    if (res->ctx)        ibv_close_device(res->ctx);
}

/* ========== 重建 QP (错误恢复) ========== */
static int rebuild_qp(struct alignment_ctx *res, const struct rdma_endpoint *remote_ep)
{
    /* 销毁进入 Error 状态的旧 QP */
    ibv_destroy_qp(res->qp);

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
    CHECK_NULL(res->qp, "重建 QP 失败");

    /* 重新建连 (连接到对端) */
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    int ret = qp_full_connect(res->qp, remote_ep, res->port, res->is_roce, access);
    CHECK_ERRNO(ret, "重建后 QP 建连失败");

    return 0;

cleanup:
    return -1;
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s server|client [server_ip]\n", argv[0]);
        fprintf(stderr, "  终端 1: %s server\n", argv[0]);
        fprintf(stderr, "  终端 2: %s client [server_ip]\n", argv[0]);
        return 1;
    }

    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("========================================================\n");
    printf("  原子操作对齐错误演示 (Atomic Alignment Error Demo)\n");
    printf("  角色: %s\n", is_server ? "服务器" : "客户端");
    printf("========================================================\n");
    printf("  要点: RDMA 原子操作 (FAA/CAS) 要求目标地址 8 字节对齐\n");
    printf("  实验 1: 未对齐地址 → 触发错误\n");
    printf("  实验 2: 正确对齐   → 成功执行\n");
    printf("========================================================\n\n");

    /* 1. 初始化 RDMA 资源 */
    struct alignment_ctx res;
    memset(&res, 0, sizeof(res));

    if (init_resources(&res) != 0) {
        fprintf(stderr, "[错误] 初始化 RDMA 资源失败\n");
        return 1;
    }

    /* 2. 填充本地端点信息并通过 TCP 交换 */
    struct rdma_endpoint local_ep, remote_ep;
    memset(&local_ep, 0, sizeof(local_ep));
    memset(&remote_ep, 0, sizeof(remote_ep));

    if (fill_local_endpoint(res.ctx, res.qp, res.port,
                            RDMA_DEFAULT_GID_INDEX, &local_ep) != 0) {
        fprintf(stderr, "[错误] 填充本地端点信息失败\n");
        cleanup_resources(&res);
        return 1;
    }

    /*
     * 将未对齐的地址和 rkey 交换给对端 (实验 1 的目标)
     * 对端将尝试对这个未对齐的地址执行 FAA
     */
    uint64_t misaligned_addr = (uint64_t)(res.raw_buf + MISALIGN_OFFSET);
    local_ep.buf_addr = misaligned_addr;
    local_ep.buf_rkey = res.raw_mr->rkey;

    printf("[信息] 本地端点: QP=%u, LID=%u\n", local_ep.qp_num, local_ep.lid);
    printf("[信息] 未对齐目标: addr=0x%lx (raw_buf=%p + %d), rkey=0x%x\n",
           (unsigned long)misaligned_addr, (void *)res.raw_buf,
           MISALIGN_OFFSET, res.raw_mr->rkey);

    printf("\n[信息] TCP 信息交换 (端口 %d)...\n", TCP_PORT);
    if (exchange_endpoint_tcp(server_ip, TCP_PORT, &local_ep, &remote_ep) != 0) {
        fprintf(stderr, "[错误] TCP 信息交换失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] 远端端点: QP=%u, buf_addr=0x%lx, rkey=0x%x\n",
           remote_ep.qp_num,
           (unsigned long)remote_ep.buf_addr, remote_ep.buf_rkey);

    /* 3. QP 建连: RESET → INIT → RTR → RTS */
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    if (qp_full_connect(res.qp, &remote_ep, res.port, res.is_roce, access) != 0) {
        fprintf(stderr, "[错误] QP 建连失败\n");
        cleanup_resources(&res);
        return 1;
    }
    printf("[信息] QP 建连完成 (RESET → INIT → RTR → RTS)\n\n");

    /*
     * 只有客户端发起原子操作 (实验 1 和 2)。
     * 服务器端作为 "被操作方"，其内存被远端原子修改。
     */
    if (!is_server) {
        /* ====== 实验 1: 对未对齐地址执行 FAA ====== */
        printf("========================================\n");
        printf("  实验 1: 对未对齐地址执行 FAA\n");
        printf("========================================\n\n");

        printf("  远端目标地址: 0x%lx\n", (unsigned long)remote_ep.buf_addr);
        printf("  地址 %% 8 = %lu", (unsigned long)(remote_ep.buf_addr % 8));
        if (remote_ep.buf_addr % 8 != 0) {
            printf(" (★ 未 8 字节对齐!)\n");
        } else {
            printf(" (已对齐)\n");
        }
        printf("\n  尝试 FAA +1 到未对齐地址...\n\n");

        int ret1 = do_faa(&res, remote_ep.buf_addr, remote_ep.buf_rkey,
                          1, "实验 1 (未对齐)");

        if (ret1 != 0) {
            printf("  ★ 如预期: 未对齐地址导致 FAA 失败!\n");
            printf("  ★ QP 已进入 ERROR 状态，需要重建\n\n");

            /* 打印当前 QP 状态 */
            print_qp_state(res.qp);
            printf("\n");

            /* 重建 QP 以继续实验 2 */
            printf("  正在重建 QP 并重新建连...\n");
            if (rebuild_qp(&res, &remote_ep) != 0) {
                fprintf(stderr, "[错误] QP 重建失败，无法继续实验 2\n");
                cleanup_resources(&res);
                return 1;
            }
            printf("  QP 重建并建连完成\n\n");
        } else {
            printf("  (注意: 某些 NIC/驱动可能不检测对齐错误)\n\n");
        }

        /* ====== 实验 2: 对正确对齐地址执行 FAA ====== */
        printf("========================================\n");
        printf("  实验 2: 对正确对齐地址执行 FAA\n");
        printf("========================================\n\n");

        /*
         * 使用 posix_memalign(64, sizeof(uint64_t)) 分配的内存
         * 需要将对齐的地址和新 rkey 告知远端...但为简化演示，
         * 这里我们使用本地 loopback: 对自己的 aligned_buf 执行 FAA
         * (QP 已连接到对端，但我们也可以用自己的 MR)
         *
         * 更简洁的做法: 直接用 server 的 aligned_buf
         * 但这里我们用已注册的 aligned_mr 来演示正确对齐
         */
        uint64_t aligned_addr = (uint64_t)res.aligned_buf;
        printf("  对齐缓冲区地址: %p\n", (void *)res.aligned_buf);
        printf("  地址 %% 8 = %lu", (unsigned long)(aligned_addr % 8));
        if (aligned_addr % 8 == 0) {
            printf(" (✓ 已 8 字节对齐)\n");
        } else {
            printf(" (未对齐)\n");
        }
        printf("  初始值: %lu\n\n", (unsigned long)*res.aligned_buf);

        /* 第一次 FAA: +1 */
        printf("  尝试 FAA +1 到对齐地址...\n\n");
        int ret2 = do_faa(&res, aligned_addr, res.aligned_mr->rkey,
                          1, "实验 2 (对齐) FAA +1");

        if (ret2 == 0) {
            printf("  当前值: %lu\n\n", (unsigned long)*res.aligned_buf);

            /* 第二次 FAA: +99 */
            printf("  尝试 FAA +99 到对齐地址...\n\n");
            do_faa(&res, aligned_addr, res.aligned_mr->rkey,
                   99, "实验 2 (对齐) FAA +99");
            printf("  最终值: %lu (期望 100)\n",
                   (unsigned long)*res.aligned_buf);
        }
    } else {
        /* 服务器端: 等待客户端完成操作 */
        printf("[Server] 等待 Client 执行原子操作实验...\n");
        printf("[Server] (Server 端内存被远端原子修改，无需本地操作)\n");
        sleep(10);
        printf("[Server] raw_buf 偏移 %d 处的值: ", MISALIGN_OFFSET);
        /* 尝试读取 (可能因对齐问题读到异常值) */
        uint64_t val;
        memcpy(&val, res.raw_buf + MISALIGN_OFFSET, sizeof(uint64_t));
        printf("%lu\n", (unsigned long)val);
    }

    /* 总结 */
    printf("\n========================================\n");
    printf("  总结: 原子操作 8 字节对齐要求\n");
    printf("========================================\n");
    printf("  1. RDMA 原子操作 (FAA/CAS) 操作 uint64_t (8 字节)\n");
    printf("  2. NIC 硬件要求目标远端地址必须 8 字节对齐\n");
    printf("  3. 推荐分配方式:\n");
    printf("     - posix_memalign(&ptr, 64, sizeof(uint64_t))\n");
    printf("     - aligned_alloc(8, sizeof(uint64_t))\n");
    printf("     - __attribute__((aligned(8))) uint64_t counter;\n");
    printf("  4. malloc() 通常返回 16 字节对齐的地址 (满足要求)\n");
    printf("     但手动偏移后可能破坏对齐!\n");

    printf("\n[信息] 程序结束，清理资源...\n");
    cleanup_resources(&res);
    return 0;
}
