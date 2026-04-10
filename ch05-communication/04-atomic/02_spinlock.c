/**
 * 分布式自旋锁 (Distributed Spinlock) —— 基于 RDMA CAS (Compare-and-Swap) 实现
 *
 * 原理:
 *   在分布式系统中，多个节点需要互斥访问共享资源。传统方案依赖中心化的锁服务器，
 *   每次加锁/解锁都需要网络往返 (RTT) 和服务器 CPU 参与。
 *
 *   基于 RDMA 原子操作的分布式锁可以完全绕过服务器 CPU:
 *     - 服务器只需维护锁变量和共享数据的内存，不参与任何锁逻辑
 *     - 客户端通过 RDMA CAS 直接操作服务器内存中的锁变量
 *     - 全程零拷贝、零 CPU 干预 (one-sided RDMA)
 *
 *   锁变量语义:
 *     uint64_t lock: 0 = 未锁定 (unlocked), 1 = 已锁定 (locked)
 *
 *   加锁 (Acquire): CAS(lock, expected=0, new=1)
 *     - 若远端 lock == 0 → 原子地设为 1，返回旧值 0 → 加锁成功
 *     - 若远端 lock != 0 → 不做修改，返回旧值 1 → 锁被占用，自旋重试
 *
 *   解锁 (Release): CAS(lock, expected=1, new=0)
 *     - 若远端 lock == 1 → 原子地设为 0，返回旧值 1 → 解锁成功
 *     - 若远端 lock != 1 → 异常状态 (锁未被持有就尝试释放)
 *
 * 应用场景:
 *   - 分布式 KV 存储 (如 FaRM, DrTM)
 *   - 分布式文件系统的元数据互斥
 *   - RDMA-based 共享内存并发控制
 *
 * 用法:
 *   服务器: ./02_spinlock server
 *   客户端: ./02_spinlock client <server_ip>
 *
 * 编译: gcc -o 02_spinlock 02_spinlock.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define TCP_PORT        7780
#define DATA_BUF_SIZE   256         /* 共享数据区大小 */
#define MAX_SPIN_COUNT  1000000     /* 自旋上限，防止死锁/无限等待 */

/* ========== 扩展连接信息 ========== */

/**
 * 除了标准 rdma_endpoint (QP号, LID, GID 等) 之外，
 * 分布式锁还需要交换锁变量和数据区的远端地址/rkey，
 * 因为 rdma_endpoint 只提供一组 buf_addr/buf_rkey。
 */
struct spinlock_conn_info {
    struct rdma_endpoint ep;        /* 标准端点信息 */
    uint64_t    lock_addr;          /* 锁变量远端虚拟地址 */
    uint32_t    lock_rkey;          /* 锁变量 MR 的 rkey */
    uint64_t    data_addr;          /* 共享数据区远端虚拟地址 */
    uint32_t    data_rkey;          /* 共享数据区 MR 的 rkey */
};

/* ========== TCP 交换扩展连接信息 ========== */

/**
 * exchange_spinlock_conn_tcp - 通过 TCP 交换分布式锁的连接信息
 *
 * @server_ip: NULL=作为服务器监听; 非NULL=作为客户端连接
 * @local:     本地信息 (输入)
 * @remote:    对端信息 (输出)
 *
 * 返回: 0 成功, -1 失败
 */
static int exchange_spinlock_conn_tcp(const char *server_ip,
                                      const struct spinlock_conn_info *local,
                                      struct spinlock_conn_info *remote)
{
    int sock = -1, conn = -1, ret = -1;
    int reuse = 1;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_PORT);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("[TCP] socket"); return -1; }

    if (server_ip == NULL) {
        /* 服务端: 监听等待客户端连接 */
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[TCP] bind"); goto out;
        }
        if (listen(sock, 1) < 0) {
            perror("[TCP] listen"); goto out;
        }
        printf("[TCP] 等待客户端连接 (端口 %d)...\n", TCP_PORT);
        conn = accept(sock, NULL, NULL);
        if (conn < 0) { perror("[TCP] accept"); goto out; }

        /* 服务端: 先收后发 */
        if (recv(conn, remote, sizeof(*remote), MSG_WAITALL) != (ssize_t)sizeof(*remote)) {
            perror("[TCP] recv"); goto out;
        }
        if (send(conn, local, sizeof(*local), 0) != (ssize_t)sizeof(*local)) {
            perror("[TCP] send"); goto out;
        }
    } else {
        /* 客户端: 连接到服务端 */
        inet_pton(AF_INET, server_ip, &addr.sin_addr);
        printf("[TCP] 连接服务器 %s:%d ...\n", server_ip, TCP_PORT);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[TCP] connect"); goto out;
        }
        conn = sock;
        sock = -1;

        /* 客户端: 先发后收 */
        if (send(conn, local, sizeof(*local), 0) != (ssize_t)sizeof(*local)) {
            perror("[TCP] send"); goto out;
        }
        if (recv(conn, remote, sizeof(*remote), MSG_WAITALL) != (ssize_t)sizeof(*remote)) {
            perror("[TCP] recv"); goto out;
        }
    }
    ret = 0;

out:
    if (conn >= 0) close(conn);
    if (sock >= 0) close(sock);
    return ret;
}

/* ========== CAS 原子操作封装 ========== */

/**
 * post_cas_and_poll - 执行一次 CAS 原子操作并等待完成
 *
 * CAS 语义: 原子地比较 remote_addr 处的值与 expected，
 *   若相等则替换为 new_val，并将操作前的旧值写入 result_buf。
 *
 * @qp:          队列对
 * @cq:          完成队列
 * @result_addr: 本地结果缓冲区地址 (旧值写回此处)
 * @result_lkey: 结果缓冲区 MR 的 lkey
 * @remote_addr: 远端锁变量地址
 * @remote_rkey: 远端锁变量 MR 的 rkey
 * @expected:    期望的旧值
 * @new_val:     要写入的新值
 *
 * 返回: 0 成功, -1 失败
 */
static int post_cas_and_poll(struct ibv_qp *qp, struct ibv_cq *cq,
                             uint64_t result_addr, uint32_t result_lkey,
                             uint64_t remote_addr, uint32_t remote_rkey,
                             uint64_t expected, uint64_t new_val)
{
    struct ibv_sge sge = {
        .addr   = result_addr,
        .length = sizeof(uint64_t),
        .lkey   = result_lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 0;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_ATOMIC_CMP_AND_SWP;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = remote_addr;
    wr.wr.atomic.rkey        = remote_rkey;
    wr.wr.atomic.compare_add = expected;   /* 比较值 */
    wr.wr.atomic.swap        = new_val;    /* 交换值 */

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("[CAS] ibv_post_send 失败");
        return -1;
    }

    /* 阻塞等待完成 */
    struct ibv_wc wc;
    if (poll_cq_blocking(cq, &wc) != 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[CAS] WC 失败: %s\n",
                wc.status != IBV_WC_SUCCESS ? ibv_wc_status_str(wc.status) : "poll error");
        return -1;
    }
    return 0;
}

/* ========== RDMA Write 封装 ========== */

/**
 * post_write_and_poll - 执行一次 RDMA Write 并等待完成
 *
 * 将本地数据单向写入远端内存，不通知远端 CPU (one-sided)。
 */
static int post_write_and_poll(struct ibv_qp *qp, struct ibv_cq *cq,
                               uint64_t local_addr, uint32_t local_lkey,
                               uint32_t length,
                               uint64_t remote_addr, uint32_t remote_rkey)
{
    struct ibv_sge sge = {
        .addr   = local_addr,
        .length = length,
        .lkey   = local_lkey,
    };

    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id      = 1;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = remote_rkey;

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("[WRITE] ibv_post_send 失败");
        return -1;
    }

    struct ibv_wc wc;
    if (poll_cq_blocking(cq, &wc) != 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[WRITE] WC 失败: %s\n",
                wc.status != IBV_WC_SUCCESS ? ibv_wc_status_str(wc.status) : "poll error");
        return -1;
    }
    return 0;
}

/* ========== 主程序 ========== */

int main(int argc, char *argv[])
{
    /* --- 参数解析 --- */
    if (argc < 2) {
        fprintf(stderr, "用法: %s server|client [server_ip]\n", argv[0]);
        return 1;
    }
    int is_server = (strcmp(argv[1], "server") == 0);
    const char *server_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("=== RDMA 分布式自旋锁 (CAS) ===\n");
    printf("角色: %s\n\n", is_server ? "服务端 (锁/数据持有者)" : "客户端 (锁请求者)");

    /* --- RDMA 资源声明 --- */
    struct ibv_device     **dev_list  = NULL;
    struct ibv_context     *ctx       = NULL;
    struct ibv_pd          *pd        = NULL;
    struct ibv_cq          *cq        = NULL;
    struct ibv_qp          *qp        = NULL;

    /* 服务端: 锁变量和共享数据 (独立 posix_memalign，各自注册 MR) */
    uint64_t  *lock_var     = NULL;     /* 锁: 0=未锁定, 1=已锁定 */
    uint64_t  *shared_data  = NULL;     /* 共享数据区 */
    struct ibv_mr *lock_mr  = NULL;
    struct ibv_mr *data_mr  = NULL;

    /* 客户端: CAS 结果缓冲区 + RDMA Write 源数据缓冲区 */
    uint64_t  *result_buf   = NULL;     /* CAS 返回旧值写入此处 */
    char      *write_buf    = NULL;     /* RDMA Write 源数据 */
    struct ibv_mr *result_mr = NULL;
    struct ibv_mr *write_mr  = NULL;

    int ret = 1;
    int num_devices;

    /* ========== 步骤1: 打开 RDMA 设备 ========== */
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 没有找到 RDMA 设备\n");
        goto cleanup;
    }
    printf("[步骤1] 设备: %s (共 %d 个)\n",
           ibv_get_device_name(dev_list[0]), num_devices);

    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "打开设备失败");

    /* 自动检测 IB/RoCE 传输层类型 */
    enum rdma_transport transport = detect_transport(ctx, RDMA_DEFAULT_PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  传输层: %s\n\n", transport_str(transport));

    /* ========== 步骤2: 创建 PD / CQ / QP ========== */
    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "分配 PD 失败");

    cq = ibv_create_cq(ctx, 128, NULL, NULL, 0);
    CHECK_NULL(cq, "创建 CQ 失败");

    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr  = 64;
    qp_init_attr.cap.max_recv_wr  = 64;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(pd, &qp_init_attr);
    CHECK_NULL(qp, "创建 QP 失败");
    printf("[步骤2] PD/CQ/QP 创建完成 (QP号=%u)\n", qp->qp_num);

    /* ========== 步骤3: 分配内存 + 注册 MR ========== */
    /*
     * 锁变量和共享数据分别 posix_memalign 到 64 字节边界:
     *   - 原子操作要求目标地址至少 8 字节对齐
     *   - 64 字节对齐可避免 false sharing (cache line = 64B)
     *
     * 注册 MR 时需要开启:
     *   REMOTE_WRITE  - 允许客户端 RDMA Write 写入数据
     *   REMOTE_READ   - 允许客户端 RDMA Read 读取数据 (可选)
     *   REMOTE_ATOMIC - 允许客户端 CAS/FAA 原子操作修改锁
     *   LOCAL_WRITE   - 允许 RDMA 硬件写入本地内存 (接收侧必需)
     */
    int access_flags = IBV_ACCESS_LOCAL_WRITE  | IBV_ACCESS_REMOTE_WRITE |
                       IBV_ACCESS_REMOTE_READ  | IBV_ACCESS_REMOTE_ATOMIC;

    /* 锁变量: uint64_t, 64 字节对齐 */
    if (posix_memalign((void **)&lock_var, 64, sizeof(uint64_t)) != 0) {
        perror("posix_memalign(lock_var)");
        goto cleanup;
    }
    *lock_var = 0;  /* 初始: 未锁定 */

    lock_mr = ibv_reg_mr(pd, lock_var, sizeof(uint64_t), access_flags);
    CHECK_NULL(lock_mr, "注册 lock MR 失败");

    /* 共享数据区: posix_memalign 到 64 字节 */
    if (posix_memalign((void **)&shared_data, 64, DATA_BUF_SIZE) != 0) {
        perror("posix_memalign(shared_data)");
        goto cleanup;
    }
    memset(shared_data, 0, DATA_BUF_SIZE);

    data_mr = ibv_reg_mr(pd, shared_data, DATA_BUF_SIZE, access_flags);
    CHECK_NULL(data_mr, "注册 data MR 失败");

    /* CAS 结果缓冲区 (客户端用，服务端也分配以简化代码) */
    if (posix_memalign((void **)&result_buf, 64, sizeof(uint64_t)) != 0) {
        perror("posix_memalign(result_buf)");
        goto cleanup;
    }
    *result_buf = 0;

    result_mr = ibv_reg_mr(pd, result_buf, sizeof(uint64_t), IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(result_mr, "注册 result MR 失败");

    /* RDMA Write 源数据缓冲区 (客户端用) */
    if (posix_memalign((void **)&write_buf, 64, DATA_BUF_SIZE) != 0) {
        perror("posix_memalign(write_buf)");
        goto cleanup;
    }
    memset(write_buf, 0, DATA_BUF_SIZE);

    write_mr = ibv_reg_mr(pd, write_buf, DATA_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(write_mr, "注册 write MR 失败");

    printf("[步骤3] 内存分配 + MR 注册完成\n");
    printf("  lock_var:    addr=%p, rkey=0x%x (初始值=%lu)\n",
           (void *)lock_var, lock_mr->rkey, (unsigned long)*lock_var);
    printf("  shared_data: addr=%p, rkey=0x%x\n",
           (void *)shared_data, data_mr->rkey);
    printf("  result_buf:  addr=%p, lkey=0x%x\n",
           (void *)result_buf, result_mr->lkey);
    printf("  write_buf:   addr=%p, lkey=0x%x\n\n",
           (void *)write_buf, write_mr->lkey);

    /* ========== 步骤4: 交换连接信息 (TCP 带外) ========== */
    struct spinlock_conn_info local_info, remote_info;
    memset(&local_info, 0, sizeof(local_info));
    memset(&remote_info, 0, sizeof(remote_info));

    /* 填充标准端点信息 (QP号, LID, GID 等) */
    int fill_ret = fill_local_endpoint(ctx, qp, RDMA_DEFAULT_PORT_NUM,
                                       RDMA_DEFAULT_GID_INDEX, &local_info.ep);
    CHECK_ERRNO(fill_ret, "填充端点信息失败");

    /* 填充锁和数据的远端地址/rkey */
    local_info.lock_addr = (uint64_t)(uintptr_t)lock_var;
    local_info.lock_rkey = lock_mr->rkey;
    local_info.data_addr = (uint64_t)(uintptr_t)shared_data;
    local_info.data_rkey = data_mr->rkey;

    printf("[步骤4] TCP 信息交换 (端口 %d)...\n", TCP_PORT);
    if (exchange_spinlock_conn_tcp(server_ip, &local_info, &remote_info) != 0) {
        fprintf(stderr, "[错误] TCP 信息交换失败\n");
        goto cleanup;
    }

    printf("  本地: QP=%u, lock=0x%lx, data=0x%lx\n",
           local_info.ep.qp_num,
           (unsigned long)local_info.lock_addr,
           (unsigned long)local_info.data_addr);
    printf("  远端: QP=%u, lock=0x%lx (rkey=0x%x), data=0x%lx (rkey=0x%x)\n",
           remote_info.ep.qp_num,
           (unsigned long)remote_info.lock_addr, remote_info.lock_rkey,
           (unsigned long)remote_info.data_addr, remote_info.data_rkey);

    /* QP 状态转换: RESET → INIT → RTR → RTS */
    int conn_ret = qp_full_connect(qp, &remote_info.ep,
                                   RDMA_DEFAULT_PORT_NUM, is_roce,
                                   access_flags);
    CHECK_ERRNO(conn_ret, "QP 建连失败");
    printf("  QP 连接就绪 (RESET→INIT→RTR→RTS)\n\n");

    /* ========== 步骤5: 分布式锁操作 ========== */

    if (is_server) {
        /* ======= 服务端: 被动等待，只维护内存 ======= */
        /*
         * 分布式锁的核心特性: 服务端 CPU 不参与锁操作。
         * 客户端通过 RDMA CAS 直接修改服务端内存中的 lock_var，
         * 通过 RDMA Write 直接写入服务端内存中的 shared_data。
         * 服务端只需保持内存可访问即可 (真正的 one-sided RDMA)。
         */
        printf("=== 服务端: 等待客户端操作 ===\n");
        printf("  lock_var 地址: %p, rkey=0x%x\n", (void *)lock_var, lock_mr->rkey);
        printf("  shared_data 地址: %p, rkey=0x%x\n", (void *)shared_data, data_mr->rkey);
        printf("  当前 lock = %lu (0=未锁定)\n\n",
               (unsigned long)*lock_var);

        /* 定期打印锁和数据的状态，观察客户端的远程操作效果 */
        printf("  等待客户端完成操作 (每秒打印状态)...\n");
        for (int i = 0; i < 12; i++) {
            sleep(1);
            printf("  [t=%2ds] lock=%lu (%s), shared_data=\"%s\"\n",
                   i + 1,
                   (unsigned long)*lock_var,
                   *lock_var == 0 ? "未锁定" : "已锁定",
                   (char *)shared_data);
        }

        /* 最终状态打印 */
        printf("\n=== 服务端: 最终状态 ===\n");
        printf("  lock_var    = %lu (%s)\n",
               (unsigned long)*lock_var,
               *lock_var == 0 ? "未锁定 ✓ 客户端已正确释放锁" :
                                "已锁定 ✗ 异常！锁未释放");
        printf("  shared_data = \"%s\"\n", (char *)shared_data);

    } else {
        /* ======= 客户端: 加锁 → 写数据 → 解锁 ======= */
        int spin_count;
        printf("=== 客户端: 分布式锁操作流程 ===\n\n");

        sleep(1);  /* 等待服务端就绪 */

        /* --- 阶段1: 获取锁 (Acquire Lock) --- */
        /*
         * 自旋获取算法:
         *   do {
         *       old_val = CAS(remote_lock, expected=0, new=1);
         *   } while (old_val != 0);
         *
         * result_buf 存储 CAS 操作前的旧值:
         *   旧值 == 0: 之前未锁 → 成功设为 1 → 加锁成功
         *   旧值 == 1: 锁被占用 → 未修改 → 继续自旋
         */
        printf("[阶段1] 获取分布式锁 (CAS: 0→1 自旋)\n");
        printf("  目标: remote lock_addr=0x%lx, lock_rkey=0x%x\n\n",
               (unsigned long)remote_info.lock_addr, remote_info.lock_rkey);

        spin_count = 0;
        while (spin_count < MAX_SPIN_COUNT) {
            spin_count++;

            /* CAS: 若远端 lock==0 则设为 1 */
            if (post_cas_and_poll(qp, cq,
                                  (uint64_t)(uintptr_t)result_buf, result_mr->lkey,
                                  remote_info.lock_addr, remote_info.lock_rkey,
                                  0,    /* expected: 未锁定 */
                                  1     /* new: 已锁定 */
                                 ) != 0) {
                fprintf(stderr, "[错误] CAS 加锁操作失败\n");
                goto cleanup;
            }

            /* 检查返回的旧值 */
            if (*result_buf == 0) {
                /* 旧值=0 → 加锁成功! */
                printf("  ★ 锁已获取! (第 %d 次尝试, 返回旧值=%lu)\n\n",
                       spin_count, (unsigned long)*result_buf);
                break;
            }

            /* 旧值≠0 → 锁被占用，继续自旋 */
            if (spin_count <= 3 || spin_count % 1000 == 0) {
                printf("  [自旋] 第 %d 次: 锁被占用 (旧值=%lu), 重试...\n",
                       spin_count, (unsigned long)*result_buf);
            }
        }

        if (spin_count >= MAX_SPIN_COUNT) {
            fprintf(stderr, "[错误] 自旋 %d 次仍未获取锁，疑似死锁\n", MAX_SPIN_COUNT);
            goto cleanup;
        }

        /* --- 阶段2: 临界区 —— 通过 RDMA Write 写入共享数据 --- */
        /*
         * 此时客户端持有锁 (远端 lock_var == 1)。
         * 在锁的保护下，通过 RDMA Write 将数据写入服务端的 shared_data。
         * 这模拟了在分布式临界区内修改共享资源。
         *
         * 注意: RDMA Write 是 one-sided 操作:
         *   - 数据直接从客户端内存 DMA 到服务端内存
         *   - 服务端 CPU 完全不感知这次写入
         *   - 没有任何中断或上下文切换
         */
        printf("[阶段2] 临界区: RDMA Write 写入共享数据\n");
        const char *msg = "DATA_FROM_CLIENT";
        strncpy(write_buf, msg, DATA_BUF_SIZE - 1);
        printf("  写入内容: \"%s\" (%zu 字节)\n", msg, strlen(msg) + 1);

        if (post_write_and_poll(qp, cq,
                                (uint64_t)(uintptr_t)write_buf, write_mr->lkey,
                                (uint32_t)(strlen(msg) + 1),
                                remote_info.data_addr, remote_info.data_rkey
                               ) != 0) {
            fprintf(stderr, "[错误] RDMA Write 失败 (仍需释放锁)\n");
            /* 即使写入失败也必须释放锁，否则死锁 */
        } else {
            printf("  RDMA Write 完成 ✓\n\n");
        }

        /* --- 阶段3: 释放锁 (Release Lock) --- */
        /*
         * CAS(lock, expected=1, new=0):
         *   旧值 == 1: 之前已锁 → 成功设为 0 → 解锁成功
         *   旧值 != 1: 异常 (不该发生 —— 我们持有锁时没人能修改它)
         */
        printf("[阶段3] 释放分布式锁 (CAS: 1→0)\n");

        if (post_cas_and_poll(qp, cq,
                              (uint64_t)(uintptr_t)result_buf, result_mr->lkey,
                              remote_info.lock_addr, remote_info.lock_rkey,
                              1,    /* expected: 已锁定 */
                              0     /* new: 未锁定 */
                             ) != 0) {
            fprintf(stderr, "[错误] CAS 解锁操作失败\n");
            goto cleanup;
        }

        if (*result_buf == 1) {
            printf("  ★ 锁已释放! (旧值=%lu → 新值=0)\n\n",
                   (unsigned long)*result_buf);
        } else {
            fprintf(stderr, "  [警告] 解锁异常: 返回旧值=%lu (期望=1)\n\n",
                    (unsigned long)*result_buf);
        }

        /* --- 操作总结 --- */
        printf("=== 客户端: 操作完成 ===\n");
        printf("  1. 获取锁 (CAS 0→1)           ✓\n");
        printf("  2. 写入共享数据 (RDMA Write)   ✓\n");
        printf("  3. 释放锁 (CAS 1→0)           ✓\n");
    }

    ret = 0;

cleanup:
    /* ========== 资源清理 (逆序释放) ========== */
    printf("\n[清理] 释放 RDMA 资源...\n");
    if (write_mr)   ibv_dereg_mr(write_mr);
    if (result_mr)  ibv_dereg_mr(result_mr);
    if (data_mr)    ibv_dereg_mr(data_mr);
    if (lock_mr)    ibv_dereg_mr(lock_mr);
    if (qp)         ibv_destroy_qp(qp);
    if (cq)         ibv_destroy_cq(cq);
    if (pd)         ibv_dealloc_pd(pd);
    if (ctx)        ibv_close_device(ctx);
    if (dev_list)   ibv_free_device_list(dev_list);
    if (write_buf)  free(write_buf);
    if (result_buf) free(result_buf);
    if (shared_data) free(shared_data);
    if (lock_var)   free(lock_var);
    printf("  清理完成\n");

    return ret;
}
