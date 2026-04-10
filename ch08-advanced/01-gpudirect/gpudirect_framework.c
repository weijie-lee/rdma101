/**
 * GPUDirect RDMA 代码框架
 *
 * 演示如何将 GPU 显存注册为 RDMA MR，实现 GPU 显存到网络的直通传输。
 *
 * 两种编译方式:
 *   有 CUDA 环境:
 *     nvcc -DHAVE_CUDA -o gpudirect_framework gpudirect_framework.c \
 *          -libverbs -lcuda -I../../common
 *
 *   无 CUDA 环境 (仅打印说明):
 *     gcc -o gpudirect_framework gpudirect_framework.c \
 *         -libverbs -I../../common
 *
 * 运行要求 (CUDA 模式):
 *   - NVIDIA GPU + 驱动
 *   - nvidia_peermem 或 nv_peer_mem 内核模块已加载
 *   - RDMA 设备可用 (ibv_devices 能看到)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

/* 条件编译: 只有定义了 HAVE_CUDA 才包含 CUDA 头文件 */
#ifdef HAVE_CUDA
#include <cuda.h>          /* CUDA Driver API (底层 API) */
#include <cuda_runtime.h>  /* CUDA Runtime API */
#endif

/* GPU 显存分配大小 */
#define GPU_BUF_SIZE  (1024 * 1024)   /* 1MB GPU 缓冲区 */
#define MSG_SIZE      64              /* 发送消息大小 */

/* ========== CUDA 模式: 真正的 GPUDirect RDMA 流程 ========== */
#ifdef HAVE_CUDA

/**
 * check_cuda_error - 检查 CUDA 调用结果
 *
 * 封装 CUDA 错误检查，失败时打印错误信息
 */
static void check_cuda_error(CUresult result, const char *msg)
{
    if (result != CUDA_SUCCESS) {
        const char *err_str;
        cuGetErrorString(result, &err_str);     /* 获取 CUDA 错误描述 */
        fprintf(stderr, "[CUDA 错误] %s: %s (错误码=%d)\n", msg, err_str, result);
        exit(EXIT_FAILURE);
    }
}

/**
 * gpudirect_demo - GPUDirect RDMA 完整演示
 *
 * 流程:
 *   1. 初始化 CUDA 设备
 *   2. 在 GPU 显存上分配缓冲区
 *   3. 打开 RDMA 设备并创建资源
 *   4. 将 GPU 显存注册为 RDMA MR (这一步需要 nvidia_peermem)
 *   5. 使用 GPU 显存地址构造 RDMA 操作 (框架示意)
 *   6. 清理所有资源
 */
static int gpudirect_demo(void)
{
    /* ---- CUDA 相关变量 ---- */
    CUdevice   cu_device;          /* CUDA 设备句柄 */
    CUcontext  cu_ctx;             /* CUDA 上下文 */
    CUdeviceptr gpu_ptr = 0;       /* GPU 显存指针 (设备地址) */

    /* ---- RDMA 相关变量 ---- */
    struct ibv_device **dev_list = NULL;    /* 设备列表 */
    struct ibv_context *ctx      = NULL;    /* 设备上下文 */
    struct ibv_pd      *pd       = NULL;    /* 保护域 */
    struct ibv_mr      *mr       = NULL;    /* 内存区域 (将注册 GPU 显存) */
    struct ibv_cq      *cq       = NULL;    /* 完成队列 */
    struct ibv_qp      *qp       = NULL;    /* 队列对 */
    int ret = 0;
    int num_devices;

    printf("=== GPUDirect RDMA 演示 (CUDA 模式) ===\n\n");

    /* ---- 步骤 1: 初始化 CUDA ---- */
    printf("[步骤 1] 初始化 CUDA 设备...\n");

    CUresult cu_ret = cuInit(0);                /* 初始化 CUDA 驱动 */
    check_cuda_error(cu_ret, "cuInit");

    cu_ret = cuDeviceGet(&cu_device, 0);        /* 获取第 0 个 GPU */
    check_cuda_error(cu_ret, "cuDeviceGet");

    cu_ret = cuCtxCreate(&cu_ctx, 0, cu_device); /* 创建 CUDA 上下文 */
    check_cuda_error(cu_ret, "cuCtxCreate");

    /* 打印 GPU 信息 */
    char gpu_name[256];
    cuDeviceGetName(gpu_name, sizeof(gpu_name), cu_device);
    printf("  GPU 设备: %s\n", gpu_name);

    /* ---- 步骤 2: 分配 GPU 显存 ---- */
    printf("[步骤 2] 分配 GPU 显存 (%d 字节)...\n", GPU_BUF_SIZE);

    cu_ret = cuMemAlloc(&gpu_ptr, GPU_BUF_SIZE);  /* 在 GPU 上分配内存 */
    check_cuda_error(cu_ret, "cuMemAlloc");

    printf("  GPU 显存地址: 0x%llx\n", (unsigned long long)gpu_ptr);

    /* 用 CUDA 接口初始化 GPU 显存内容 */
    cuMemsetD8(gpu_ptr, 0, GPU_BUF_SIZE);         /* 清零 GPU 显存 */

    /* ---- 步骤 3: 初始化 RDMA 设备 ---- */
    printf("[步骤 3] 初始化 RDMA 设备...\n");

    dev_list = ibv_get_device_list(&num_devices);  /* 获取 RDMA 设备列表 */
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "[错误] 没有找到 RDMA 设备\n");
        ret = -1;
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);            /* 打开第一个 RDMA 设备 */
    if (!ctx) {
        fprintf(stderr, "[错误] 无法打开 RDMA 设备\n");
        ret = -1;
        goto cleanup;
    }
    printf("  RDMA 设备: %s\n", ibv_get_device_name(dev_list[0]));

    pd = ibv_alloc_pd(ctx);                        /* 分配保护域 */
    if (!pd) {
        fprintf(stderr, "[错误] 分配 PD 失败\n");
        ret = -1;
        goto cleanup;
    }

    /* ---- 步骤 4: 将 GPU 显存注册为 RDMA MR ---- */
    /*
     * 这是 GPUDirect RDMA 的核心步骤！
     *
     * ibv_reg_mr() 通常接受 CPU 虚拟地址，但当 nvidia_peermem 模块
     * 加载后，它也能接受 GPU 显存地址 (CUDA device pointer)。
     *
     * 内核流程:
     *   ibv_reg_mr → ioctl → 内核 ib_core → nvidia_peermem 回调
     *   → nvidia 驱动查找 GPU 物理页面 → 返回 DMA 映射给 NIC
     *
     * 如果 nvidia_peermem 未加载，ibv_reg_mr 会失败 (返回 NULL)
     */
    printf("[步骤 4] 注册 GPU 显存为 RDMA MR (需要 nvidia_peermem)...\n");

    mr = ibv_reg_mr(pd,
                    (void *)gpu_ptr,           /* GPU 显存地址 */
                    GPU_BUF_SIZE,              /* 注册大小 */
                    IBV_ACCESS_LOCAL_WRITE  |   /* 本地写权限 */
                    IBV_ACCESS_REMOTE_WRITE |   /* 远端写权限 (RDMA Write) */
                    IBV_ACCESS_REMOTE_READ);    /* 远端读权限 (RDMA Read) */

    if (!mr) {
        fprintf(stderr, "[错误] ibv_reg_mr 注册 GPU 显存失败 (errno=%d: %s)\n",
                errno, strerror(errno));
        fprintf(stderr, "  可能原因:\n");
        fprintf(stderr, "  1. nvidia_peermem 模块未加载 (modprobe nvidia_peermem)\n");
        fprintf(stderr, "  2. GPU 驱动版本不兼容\n");
        fprintf(stderr, "  3. RDMA 设备不支持 peer memory\n");
        ret = -1;
        goto cleanup;
    }

    printf("  ✓ GPU 显存 MR 注册成功!\n");
    printf("  MR lkey=0x%x, rkey=0x%x\n", mr->lkey, mr->rkey);

    /* ---- 步骤 5: 使用 GPU MR 进行 RDMA 操作 (框架示意) ---- */
    /*
     * 注册成功后，就可以像使用普通 CPU MR 一样使用 GPU MR:
     *
     * 示例: RDMA Write (将 GPU 显存数据直接发送到远端)
     *
     *   struct ibv_sge sge = {
     *       .addr   = (uint64_t)gpu_ptr,   // GPU 显存地址
     *       .length = MSG_SIZE,
     *       .lkey   = mr->lkey,            // GPU MR 的 lkey
     *   };
     *
     *   struct ibv_send_wr wr = {
     *       .opcode     = IBV_WR_RDMA_WRITE,
     *       .sg_list    = &sge,
     *       .num_sge    = 1,
     *       .wr.rdma.remote_addr = remote_addr,
     *       .wr.rdma.rkey        = remote_rkey,
     *   };
     *
     *   ibv_post_send(qp, &wr, &bad_wr);
     *
     * 数据流: GPU显存 → PCIe → NIC → 网络 (跳过 CPU 内存！)
     */
    printf("[步骤 5] GPU MR 已就绪，可用于 RDMA 操作\n");
    printf("  数据路径: GPU显存 → PCIe → RDMA NIC → 网络\n");
    printf("  (完整的 RDMA Write/Read 需要对端连接，此处仅演示 MR 注册)\n");

    printf("\n=== GPUDirect RDMA 框架演示完成 ===\n");

cleanup:
    /* ---- 步骤 6: 清理资源 (逆序释放) ---- */
    printf("\n[清理] 释放资源...\n");

    if (qp)       ibv_destroy_qp(qp);         /* 销毁 QP */
    if (cq)       ibv_destroy_cq(cq);         /* 销毁 CQ */
    if (mr)       ibv_dereg_mr(mr);            /* 注销 MR */
    if (pd)       ibv_dealloc_pd(pd);          /* 释放 PD */
    if (ctx)      ibv_close_device(ctx);       /* 关闭设备 */
    if (dev_list) ibv_free_device_list(dev_list); /* 释放设备列表 */
    if (gpu_ptr)  cuMemFree(gpu_ptr);          /* 释放 GPU 显存 */
    if (cu_ctx)   cuCtxDestroy(cu_ctx);        /* 销毁 CUDA 上下文 */

    return ret;
}

#else /* !HAVE_CUDA */

/* ========== 无 CUDA 模式: 打印说明 ========== */

/**
 * gpudirect_demo - 无 CUDA 模式，仅打印 GPUDirect RDMA 的工作流程说明
 */
static int gpudirect_demo(void)
{
    printf("=== GPUDirect RDMA 代码框架说明 ===\n");
    printf("\n");
    printf("当前编译未定义 HAVE_CUDA，以下是 GPUDirect RDMA 的代码流程说明:\n");
    printf("\n");
    printf("┌─────────────────────────────────────────────────┐\n");
    printf("│         GPUDirect RDMA 代码流程                  │\n");
    printf("├─────────────────────────────────────────────────┤\n");
    printf("│                                                  │\n");
    printf("│  1. 初始化 CUDA                                  │\n");
    printf("│     cuInit(0)                                    │\n");
    printf("│     cuDeviceGet(&device, 0)                      │\n");
    printf("│     cuCtxCreate(&ctx, 0, device)                 │\n");
    printf("│                                                  │\n");
    printf("│  2. 分配 GPU 显存                                │\n");
    printf("│     cuMemAlloc(&gpu_ptr, size)                   │\n");
    printf("│     // gpu_ptr 是设备地址，不是 CPU 地址          │\n");
    printf("│                                                  │\n");
    printf("│  3. 初始化 RDMA 资源                             │\n");
    printf("│     ibv_get_device_list()                        │\n");
    printf("│     ibv_open_device()                            │\n");
    printf("│     ibv_alloc_pd()                               │\n");
    printf("│                                                  │\n");
    printf("│  4. 注册 GPU 显存为 RDMA MR ← 关键步骤!         │\n");
    printf("│     ibv_reg_mr(pd, (void*)gpu_ptr, size, flags)  │\n");
    printf("│     // 需要 nvidia_peermem 内核模块              │\n");
    printf("│     // 失败 → 检查 lsmod | grep nvidia_peermem  │\n");
    printf("│                                                  │\n");
    printf("│  5. 像普通 MR 一样使用 GPU MR                    │\n");
    printf("│     sge.addr = (uint64_t)gpu_ptr                 │\n");
    printf("│     sge.lkey = mr->lkey                          │\n");
    printf("│     ibv_post_send(qp, &wr, &bad_wr)             │\n");
    printf("│     // NIC 通过 PCIe 直接读取 GPU 显存           │\n");
    printf("│                                                  │\n");
    printf("│  6. 数据路径:                                    │\n");
    printf("│     GPU显存 → PCIe → NIC → 网络                 │\n");
    printf("│     (完全跳过 CPU 内存拷贝!)                     │\n");
    printf("│                                                  │\n");
    printf("└─────────────────────────────────────────────────┘\n");
    printf("\n");
    printf("编译说明:\n");
    printf("  有 CUDA 环境:\n");
    printf("    nvcc -DHAVE_CUDA -o gpudirect_framework gpudirect_framework.c \\\n");
    printf("         -libverbs -lcuda\n");
    printf("\n");
    printf("  无 CUDA 环境 (当前模式):\n");
    printf("    gcc -o gpudirect_framework gpudirect_framework.c -libverbs\n");
    printf("\n");
    printf("运行前提:\n");
    printf("  1. NVIDIA GPU + 驱动\n");
    printf("  2. modprobe nvidia_peermem (或 nv_peer_mem)\n");
    printf("  3. RDMA 设备可用\n");
    printf("  4. 运行 gpudirect_check.sh 验证环境\n");
    printf("\n");
    printf("与普通 RDMA 程序的唯一区别:\n");
    printf("  普通:     buf = malloc(size);     ibv_reg_mr(pd, buf, ...)\n");
    printf("  GPUDirect: cuMemAlloc(&buf, size); ibv_reg_mr(pd, (void*)buf, ...)\n");
    printf("  后续的 ibv_post_send / ibv_post_recv 代码完全一样!\n");

    return 0;
}

#endif /* HAVE_CUDA */

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    (void)argc;     /* 未使用的参数 */
    (void)argv;

    return gpudirect_demo();
}
