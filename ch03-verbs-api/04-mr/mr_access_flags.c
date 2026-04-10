/**
 * MR 访问标志 (Access Flags) 演示
 *
 * 本程序演示 ibv_reg_mr() 中不同访问标志的作用:
 *   1. 注册多种标志组合的 MR, 打印 lkey/rkey
 *   2. 展示: LOCAL_WRITE only, LOCAL_WRITE+REMOTE_READ, +REMOTE_WRITE, +REMOTE_ATOMIC
 *   3. 使用 loopback QP 测试: 尝试 RDMA Write 到只读 MR → 产生保护错误
 *   4. 打印系统内存锁定限制信息 (ulimit -l, nr_hugepages)
 *
 * 编译: gcc -o mr_access_flags mr_access_flags.c -I../../common ../../common/librdma_utils.a -libverbs
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
#define CQ_DEPTH    32

/**
 * 辅助函数: 将 MR 访问标志转为可读字符串
 */
static void print_access_flags(int flags)
{
    printf("    标志组合: ");
    if (flags & IBV_ACCESS_LOCAL_WRITE)   printf("LOCAL_WRITE ");
    if (flags & IBV_ACCESS_REMOTE_WRITE)  printf("REMOTE_WRITE ");
    if (flags & IBV_ACCESS_REMOTE_READ)   printf("REMOTE_READ ");
    if (flags & IBV_ACCESS_REMOTE_ATOMIC) printf("REMOTE_ATOMIC ");
    if (flags == 0)                        printf("(无 — 仅本地只读)");
    printf("\n");
}

/**
 * 辅助函数: 打印系统内存锁定限制
 */
static void print_memory_limits(void)
{
    FILE *fp;
    char line[256];

    printf("\n=== 系统内存锁定信息 ===\n");

    /* ulimit -l (通过 /proc/self/limits 读取) */
    fp = fopen("/proc/self/limits", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "Max locked memory")) {
                printf("  %s", line);
                break;
            }
        }
        fclose(fp);
    }

    /* /proc/sys/vm/nr_hugepages */
    fp = fopen("/proc/sys/vm/nr_hugepages", "r");
    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            printf("  /proc/sys/vm/nr_hugepages = %s", line);
        }
        fclose(fp);
    }

    printf("  提示: MR 注册需要锁定物理内存 (pinned memory)\n");
    printf("  如果 ulimit -l 太小, ibv_reg_mr 会失败 (errno=ENOMEM)\n");
    printf("  解决: ulimit -l unlimited 或修改 /etc/security/limits.conf\n");
    printf("============================\n\n");
}

int main(int argc, char *argv[])
{
    /* 资源声明 */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_cq      *cq       = NULL;
    struct ibv_qp      *qp       = NULL;
    struct ibv_mr      *mr_arr[4] = {NULL};  /* 4 种不同标志的 MR */
    char               *buf_arr[4] = {NULL};
    struct ibv_mr      *mr_src   = NULL;     /* loopback 测试的源 MR */
    char               *buf_src  = NULL;
    int                 num_devices;
    int                 ret;
    int                 i;

    printf("============================================\n");
    printf("  MR 访问标志 (Access Flags) 演示\n");
    printf("============================================\n\n");

    /* 打印系统内存限制信息 */
    print_memory_limits();

    /* ========== 打开设备, 创建 PD ========== */
    printf("[步骤1] 打开设备并创建 PD\n");
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "获取设备列表失败");
    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未发现 RDMA 设备\n");
        goto cleanup;
    }
    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "打开设备失败");
    printf("  设备: %s\n", ibv_get_device_name(dev_list[0]));

    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "分配 PD 失败");
    printf("  PD 分配成功\n\n");

    /* ========== 步骤 2: 注册不同标志组合的 MR ========== */
    printf("[步骤2] 注册不同访问标志的 MR\n");
    printf("========================================\n");

    /* 定义 4 种标志组合 */
    int flag_combos[] = {
        /* 组合 1: 仅本地写 (最小权限, 适用于仅本地使用的缓冲区) */
        IBV_ACCESS_LOCAL_WRITE,
        /* 组合 2: 本地写 + 远端读 (允许对端 RDMA Read 此 MR) */
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ,
        /* 组合 3: 本地写 + 远端写 (允许对端 RDMA Write 此 MR) */
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE,
        /* 组合 4: 全部权限 (含远端原子操作) */
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
        IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC,
    };
    const char *combo_desc[] = {
        "仅本地写 (LOCAL_WRITE)",
        "本地写 + 远端读 (LOCAL_WRITE | REMOTE_READ)",
        "本地写 + 远端写 (LOCAL_WRITE | REMOTE_WRITE)",
        "全部权限 (LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE | REMOTE_ATOMIC)",
    };

    for (i = 0; i < 4; i++) {
        printf("\n  --- MR[%d]: %s ---\n", i, combo_desc[i]);
        print_access_flags(flag_combos[i]);

        buf_arr[i] = malloc(BUFFER_SIZE);
        CHECK_NULL(buf_arr[i], "malloc 失败");
        memset(buf_arr[i], 'A' + i, BUFFER_SIZE);

        mr_arr[i] = ibv_reg_mr(pd, buf_arr[i], BUFFER_SIZE, flag_combos[i]);
        CHECK_NULL(mr_arr[i], "ibv_reg_mr 失败");

        printf("    lkey = 0x%08x (本地访问密钥)\n", mr_arr[i]->lkey);
        printf("    rkey = 0x%08x (远程访问密钥)\n", mr_arr[i]->rkey);
        printf("    addr = %p, length = %zu\n",
               mr_arr[i]->addr, (size_t)mr_arr[i]->length);
    }

    printf("\n  ★ 关键观察: 每个 MR 的 lkey/rkey 值不同\n");
    printf("  ★ lkey 用于本地 WR 的 SGE, rkey 需告知对端用于 RDMA Read/Write\n");
    printf("  ★ REMOTE_* 标志控制对端是否可以 RDMA Read/Write/Atomic 此区域\n\n");

    /* ========== 步骤 3: 创建 loopback QP 测试 RDMA Write ========== */
    printf("[步骤3] 创建 loopback QP, 测试 RDMA Write 权限检查\n");
    printf("========================================\n\n");

    cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);
    CHECK_NULL(cq, "创建 CQ 失败");

    struct ibv_qp_init_attr qp_init = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = 16,
            .max_recv_wr  = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    qp = ibv_create_qp(pd, &qp_init);
    CHECK_NULL(qp, "创建 QP 失败");
    printf("  QP 创建成功, qp_num=%u\n", qp->qp_num);

    /* 准备源 MR (用于 RDMA Write 的源数据) */
    buf_src = malloc(BUFFER_SIZE);
    CHECK_NULL(buf_src, "malloc buf_src 失败");
    memset(buf_src, 'S', BUFFER_SIZE);
    mr_src = ibv_reg_mr(pd, buf_src, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr_src, "注册源 MR 失败");

    /* 准备 loopback 端点信息 */
    struct rdma_endpoint local_ep;
    ret = fill_local_endpoint(ctx, qp, PORT_NUM, RDMA_DEFAULT_GID_INDEX, &local_ep);
    CHECK_ERRNO(ret, "填充端点信息失败");

    /* 检测传输类型 */
    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("  传输类型: %s\n", transport_str(transport));

    /* 全部权限连接 QP (loopback) */
    int full_access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    ret = qp_full_connect(qp, &local_ep, PORT_NUM, is_roce, full_access);
    if (ret != 0) {
        printf("  QP 连接失败 (loopback), 跳过 RDMA Write 测试\n");
        printf("  → 提示: 确保端口状态为 ACTIVE\n");
        goto skip_rdma_test;
    }
    printf("  QP 已连接 (loopback, RESET→INIT→RTR→RTS)\n\n");

    /*
     * 测试: RDMA Write 到 MR[0] (仅 LOCAL_WRITE, 没有 REMOTE_WRITE 权限)
     * 预期: HCA 产生 IBV_WC_REM_ACCESS_ERR (Remote Access Error)
     */
    printf("  测试: RDMA Write → MR[0] (仅 LOCAL_WRITE, 无 REMOTE_WRITE)\n");
    printf("  目标 rkey=0x%08x, 预期产生 Remote Access Error...\n", mr_arr[0]->rkey);

    struct ibv_sge sge_test = {
        .addr   = (uintptr_t)buf_src,
        .length = 64,
        .lkey   = mr_src->lkey,
    };
    struct ibv_send_wr wr_test = {
        .wr_id      = 100,
        .sg_list    = &sge_test,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = (uintptr_t)buf_arr[0],
            .rkey        = mr_arr[0]->rkey,  /* 目标 MR 无 REMOTE_WRITE */
        },
    };
    struct ibv_send_wr *bad_wr = NULL;

    ret = ibv_post_send(qp, &wr_test, &bad_wr);
    if (ret != 0) {
        printf("  ibv_post_send 失败: %s\n", strerror(errno));
    } else {
        /* 轮询 CQ 获取结果 */
        struct ibv_wc wc;
        int ne;
        int poll_count = 0;
        while (poll_count < 1000000) {
            ne = ibv_poll_cq(cq, 1, &wc);
            if (ne > 0) {
                printf("  CQ 结果: status=%s (%d)\n",
                       ibv_wc_status_str(wc.status), wc.status);
                if (wc.status != IBV_WC_SUCCESS) {
                    printf("  ✓ 预期的权限错误! RDMA Write 被拒绝 (MR 无 REMOTE_WRITE)\n");
                }
                break;
            }
            if (ne < 0) {
                printf("  ibv_poll_cq 出错\n");
                break;
            }
            poll_count++;
        }
        if (poll_count >= 1000000) {
            printf("  轮询超时, 未收到完成事件\n");
        }
    }

skip_rdma_test:
    /* ========== 总结 ========== */
    printf("\n============================================\n");
    printf("  MR 访问标志总结\n");
    printf("============================================\n");
    printf("  IBV_ACCESS_LOCAL_WRITE:    允许本地 QP 写入此 MR\n");
    printf("  IBV_ACCESS_REMOTE_READ:    允许对端 RDMA Read 此 MR\n");
    printf("  IBV_ACCESS_REMOTE_WRITE:   允许对端 RDMA Write 此 MR\n");
    printf("  IBV_ACCESS_REMOTE_ATOMIC:  允许对端对此 MR 执行原子操作\n");
    printf("  ★ 最小权限原则: 只开启实际需要的标志\n");
    printf("  ★ REMOTE_WRITE/REMOTE_ATOMIC 都隐含需要 LOCAL_WRITE\n");
    printf("  ★ 注册 MR 会锁定物理页, 注意 ulimit -l 限制\n\n");

cleanup:
    printf("[清理] 释放资源...\n");
    if (qp)     ibv_destroy_qp(qp);
    if (cq)     ibv_destroy_cq(cq);
    if (mr_src) ibv_dereg_mr(mr_src);
    if (buf_src) free(buf_src);
    for (i = 3; i >= 0; i--) {
        if (mr_arr[i])  ibv_dereg_mr(mr_arr[i]);
        if (buf_arr[i]) free(buf_arr[i]);
    }
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  完成\n");
    return 0;
}
