/**
 * 多次注册同一缓冲区演示 (Multiple MR Registration)
 *
 * 本程序演示:
 *   1. 同一块 malloc 内存可以被多次注册为不同的 MR → 每次 lkey/rkey 不同
 *   2. 重叠的 MR 是被允许的 (overlapping MRs)
 *   3. 讨论性能影响: 为什么应该预注册, 避免频繁 reg/dereg
 *
 * 编译: gcc -o mr_multi_reg mr_multi_reg.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE   (64 * 1024)   /* 64 KB 缓冲区 */
#define NUM_MR        5             /* 对同一缓冲区注册 5 次 */
#define PERF_ITER     100           /* 性能测试迭代次数 */

/**
 * 辅助函数: 获取当前时间 (纳秒)
 */
static uint64_t get_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(int argc, char *argv[])
{
    /* 资源声明 */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_mr      *mr[NUM_MR] = {NULL};
    struct ibv_mr      *mr_overlap = NULL;   /* 重叠注册 */
    char               *buffer   = NULL;
    int                 num_devices;
    int                 i;

    printf("============================================\n");
    printf("  多次注册同一缓冲区演示\n");
    printf("============================================\n\n");

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

    /* ========== 步骤 2: 分配共享缓冲区 ========== */
    printf("[步骤2] 分配 %d KB 缓冲区\n", BUFFER_SIZE / 1024);
    buffer = malloc(BUFFER_SIZE);
    CHECK_NULL(buffer, "malloc 失败");
    memset(buffer, 0, BUFFER_SIZE);
    printf("  缓冲区地址: %p, 大小: %d bytes\n\n", buffer, BUFFER_SIZE);

    /* ========== 步骤 3: 对同一缓冲区注册多次 ========== */
    printf("[步骤3] 对同一缓冲区注册 %d 个 MR\n", NUM_MR);
    printf("========================================\n");

    for (i = 0; i < NUM_MR; i++) {
        /*
         * 同一块内存可以注册多次, 每次得到不同的 MR 对象, 不同的 lkey/rkey。
         * 这是完全合法的操作。RDMA 硬件会为每次注册创建独立的页表映射。
         */
        int flags = IBV_ACCESS_LOCAL_WRITE;
        if (i >= 1) flags |= IBV_ACCESS_REMOTE_READ;
        if (i >= 2) flags |= IBV_ACCESS_REMOTE_WRITE;

        mr[i] = ibv_reg_mr(pd, buffer, BUFFER_SIZE, flags);
        CHECK_NULL(mr[i], "ibv_reg_mr 失败");

        printf("  MR[%d]: lkey=0x%08x  rkey=0x%08x  addr=%p  len=%zu\n",
               i, mr[i]->lkey, mr[i]->rkey, mr[i]->addr,
               (size_t)mr[i]->length);
    }

    printf("\n  ★ 关键观察:\n");
    printf("    - 所有 MR 指向同一块物理内存 (addr 相同)\n");
    printf("    - 但每个 MR 的 lkey/rkey 值不同!\n");
    printf("    - 这意味着可以为同一内存创建不同权限的 \"视图\"\n");
    printf("    - 例如: 给对端 A 只读 rkey, 给对端 B 读写 rkey\n\n");

    /* ========== 步骤 4: 重叠区域注册 ========== */
    printf("[步骤4] 注册重叠区域的 MR\n");
    printf("========================================\n");
    printf("  已有 MR 覆盖: [%p, %p)\n", buffer, buffer + BUFFER_SIZE);
    printf("  新注册区域:   [%p, %p) (前半部分)\n", buffer, buffer + BUFFER_SIZE / 2);

    mr_overlap = ibv_reg_mr(pd, buffer, BUFFER_SIZE / 2, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr_overlap, "注册重叠 MR 失败");
    printf("  MR_overlap: lkey=0x%08x  rkey=0x%08x  len=%zu\n",
           mr_overlap->lkey, mr_overlap->rkey, (size_t)mr_overlap->length);
    printf("  ✓ 重叠注册成功! RDMA 允许同一内存的任意子区域被注册\n\n");

    /* ========== 步骤 5: MR 注册/注销性能测试 ========== */
    printf("[步骤5] MR 注册/注销性能测试 (%d 次迭代)\n", PERF_ITER);
    printf("========================================\n");

    uint64_t t_start, t_end;
    struct ibv_mr *mr_perf;
    char *perf_buf = malloc(4096);
    CHECK_NULL(perf_buf, "malloc perf_buf 失败");
    memset(perf_buf, 0, 4096);

    /* 测量注册耗时 */
    t_start = get_ns();
    for (i = 0; i < PERF_ITER; i++) {
        mr_perf = ibv_reg_mr(pd, perf_buf, 4096, IBV_ACCESS_LOCAL_WRITE);
        if (!mr_perf) {
            fprintf(stderr, "  性能测试中 reg_mr 失败 (iter=%d)\n", i);
            break;
        }
        ibv_dereg_mr(mr_perf);
    }
    t_end = get_ns();

    if (i == PERF_ITER) {
        uint64_t total_ns = t_end - t_start;
        printf("  %d 次 reg_mr + dereg_mr 总耗时: %.2f ms\n",
               PERF_ITER, total_ns / 1e6);
        printf("  平均每次 reg+dereg: %.1f us\n",
               (double)total_ns / PERF_ITER / 1000.0);
        printf("\n  ★ 性能启示:\n");
        printf("    - ibv_reg_mr() 是重量级操作 (需 pin 内存 + 建立页表)\n");
        printf("    - 应在初始化时一次性注册, 避免在数据路径上反复 reg/dereg\n");
        printf("    - 预注册大块内存, 然后通过偏移量使用不同区域\n");
        printf("    - 对于动态缓冲区, 考虑使用 Memory Window (MW) 或 ODP\n");
    }

    free(perf_buf);

    /* ========== 总结 ========== */
    printf("\n============================================\n");
    printf("  多次注册总结\n");
    printf("============================================\n");
    printf("  1. 同一虚拟内存可注册为多个 MR (不同 lkey/rkey)\n");
    printf("  2. 重叠的内存区域注册是合法的\n");
    printf("  3. 每个 MR 可以有不同的访问权限 (不同的安全视图)\n");
    printf("  4. 性能最佳实践:\n");
    printf("     - 预注册 (application init 时注册)\n");
    printf("     - 避免热路径上 reg/dereg (开销 ~几十微秒)\n");
    printf("     - 大缓冲区注册一次, 通过偏移使用子区域\n");
    printf("     - 考虑 Memory Pool + 预注册模式\n\n");

cleanup:
    printf("[清理] 释放资源...\n");
    if (mr_overlap) ibv_dereg_mr(mr_overlap);
    for (i = NUM_MR - 1; i >= 0; i--) {
        if (mr[i]) ibv_dereg_mr(mr[i]);
    }
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  完成\n");
    return 0;
}
