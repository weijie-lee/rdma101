/**
 * GPUDirect RDMA Code Framework
 *
 * Demonstrates how to register GPU memory as an RDMA MR for direct
 * GPU-to-network data transfer.
 *
 * Two compilation modes:
 *   With CUDA environment:
 *     nvcc -DHAVE_CUDA -o gpudirect_framework gpudirect_framework.c \
 *          -libverbs -lcuda -I../../common
 *
 *   Without CUDA environment (prints description only):
 *     gcc -o gpudirect_framework gpudirect_framework.c \
 *         -libverbs -I../../common
 *
 * Runtime requirements (CUDA mode):
 *   - NVIDIA GPU + driver
 *   - nvidia_peermem or nv_peer_mem kernel module loaded
 *   - RDMA device available (visible via ibv_devices)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

/* Conditional compilation: only include CUDA headers if HAVE_CUDA is defined */
#ifdef HAVE_CUDA
#include <cuda.h>          /* CUDA Driver API (low-level API) */
#include <cuda_runtime.h>  /* CUDA Runtime API */
#endif

/* GPU memory allocation size */
#define GPU_BUF_SIZE  (1024 * 1024)   /* 1MB GPU buffer */
#define MSG_SIZE      64              /* Send message size */

/* ========== CUDA Mode: Actual GPUDirect RDMA Flow ========== */
#ifdef HAVE_CUDA

/**
 * check_cuda_error - Check CUDA call result
 *
 * Wraps CUDA error checking, prints error message on failure
 */
static void check_cuda_error(CUresult result, const char *msg)
{
    if (result != CUDA_SUCCESS) {
        const char *err_str;
        cuGetErrorString(result, &err_str);     /* Get CUDA error description */
        fprintf(stderr, "[CUDA Error] %s: %s (error code=%d)\n", msg, err_str, result);
        exit(EXIT_FAILURE);
    }
}

/**
 * gpudirect_demo - GPUDirect RDMA complete demonstration
 *
 * Flow:
 *   1. Initialize CUDA device
 *   2. Allocate buffer in GPU memory
 *   3. Open RDMA device and create resources
 *   4. Register GPU memory as RDMA MR (this step requires nvidia_peermem)
 *   5. Construct RDMA operation using GPU memory address (framework illustration)
 *   6. Clean up all resources
 */
static int gpudirect_demo(void)
{
    /* ---- CUDA related variables ---- */
    CUdevice   cu_device;          /* CUDA device handle */
    CUcontext  cu_ctx;             /* CUDA context */
    CUdeviceptr gpu_ptr = 0;       /* GPU memory pointer (device address) */

    /* ---- RDMA related variables ---- */
    struct ibv_device **dev_list = NULL;    /* Device list */
    struct ibv_context *ctx      = NULL;    /* Device context */
    struct ibv_pd      *pd       = NULL;    /* Protection Domain */
    struct ibv_mr      *mr       = NULL;    /* Memory Region (will register GPU memory) */
    struct ibv_cq      *cq       = NULL;    /* Completion Queue */
    struct ibv_qp      *qp       = NULL;    /* Queue Pair */
    int ret = 0;
    int num_devices;

    printf("=== GPUDirect RDMA Demo (CUDA Mode) ===\n\n");

    /* ---- Step 1: Initialize CUDA ---- */
    printf("[Step 1] Initializing CUDA device...\n");

    CUresult cu_ret = cuInit(0);                /* Initialize CUDA driver */
    check_cuda_error(cu_ret, "cuInit");

    cu_ret = cuDeviceGet(&cu_device, 0);        /* Get GPU #0 */
    check_cuda_error(cu_ret, "cuDeviceGet");

    cu_ret = cuCtxCreate(&cu_ctx, 0, cu_device); /* Create CUDA context */
    check_cuda_error(cu_ret, "cuCtxCreate");

    /* Print GPU info */
    char gpu_name[256];
    cuDeviceGetName(gpu_name, sizeof(gpu_name), cu_device);
    printf("  GPU device: %s\n", gpu_name);

    /* ---- Step 2: Allocate GPU memory ---- */
    printf("[Step 2] Allocating GPU memory (%d bytes)...\n", GPU_BUF_SIZE);

    cu_ret = cuMemAlloc(&gpu_ptr, GPU_BUF_SIZE);  /* Allocate memory on GPU */
    check_cuda_error(cu_ret, "cuMemAlloc");

    printf("  GPU memory address: 0x%llx\n", (unsigned long long)gpu_ptr);

    /* Initialize GPU memory content via CUDA interface */
    cuMemsetD8(gpu_ptr, 0, GPU_BUF_SIZE);         /* Zero out GPU memory */

    /* ---- Step 3: Initialize RDMA device ---- */
    printf("[Step 3] Initializing RDMA device...\n");

    dev_list = ibv_get_device_list(&num_devices);  /* Get RDMA device list */
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        ret = -1;
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);            /* Open first RDMA device */
    if (!ctx) {
        fprintf(stderr, "[Error] Cannot open RDMA device\n");
        ret = -1;
        goto cleanup;
    }
    printf("  RDMA device: %s\n", ibv_get_device_name(dev_list[0]));

    pd = ibv_alloc_pd(ctx);                        /* Allocate Protection Domain */
    if (!pd) {
        fprintf(stderr, "[Error] Failed to allocate PD\n");
        ret = -1;
        goto cleanup;
    }

    /* ---- Step 4: Register GPU memory as RDMA MR ---- */
    /*
     * This is the core step of GPUDirect RDMA!
     *
     * ibv_reg_mr() normally accepts CPU virtual addresses, but when the
     * nvidia_peermem module is loaded, it can also accept GPU memory
     * addresses (CUDA device pointers).
     *
     * Kernel flow:
     *   ibv_reg_mr -> ioctl -> kernel ib_core -> nvidia_peermem callback
     *   -> nvidia driver looks up GPU physical pages -> returns DMA mapping to NIC
     *
     * If nvidia_peermem is not loaded, ibv_reg_mr will fail (returns NULL)
     */
    printf("[Step 4] Registering GPU memory as RDMA MR (requires nvidia_peermem)...\n");

    mr = ibv_reg_mr(pd,
                    (void *)gpu_ptr,           /* GPU memory address */
                    GPU_BUF_SIZE,              /* Registration size */
                    IBV_ACCESS_LOCAL_WRITE  |   /* Local write permission */
                    IBV_ACCESS_REMOTE_WRITE |   /* Remote write permission (RDMA Write) */
                    IBV_ACCESS_REMOTE_READ);    /* Remote read permission (RDMA Read) */

    if (!mr) {
        fprintf(stderr, "[Error] ibv_reg_mr failed to register GPU memory (errno=%d: %s)\n",
                errno, strerror(errno));
        fprintf(stderr, "  Possible causes:\n");
        fprintf(stderr, "  1. nvidia_peermem module not loaded (modprobe nvidia_peermem)\n");
        fprintf(stderr, "  2. Incompatible GPU driver version\n");
        fprintf(stderr, "  3. RDMA device does not support peer memory\n");
        ret = -1;
        goto cleanup;
    }

    printf("  ✓ GPU memory MR registration successful!\n");
    printf("  MR lkey=0x%x, rkey=0x%x\n", mr->lkey, mr->rkey);

    /* ---- Step 5: Use GPU MR for RDMA operations (framework illustration) ---- */
    /*
     * After successful registration, the GPU MR can be used just like a regular CPU MR:
     *
     * Example: RDMA Write (send GPU memory data directly to remote)
     *
     *   struct ibv_sge sge = {
     *       .addr   = (uint64_t)gpu_ptr,   // GPU memory address
     *       .length = MSG_SIZE,
     *       .lkey   = mr->lkey,            // GPU MR's lkey
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
     * Data flow: GPU Memory -> PCIe -> NIC -> Network (bypasses CPU memory!)
     */
    printf("[Step 5] GPU MR is ready, can be used for RDMA operations\n");
    printf("  Data path: GPU Memory -> PCIe -> RDMA NIC -> Network\n");
    printf("  (Full RDMA Write/Read requires a remote peer connection; this only demonstrates MR registration)\n");

    printf("\n=== GPUDirect RDMA Framework Demo Complete ===\n");

cleanup:
    /* ---- Step 6: Clean up resources (release in reverse order) ---- */
    printf("\n[Cleanup] Releasing resources...\n");

    if (qp)       ibv_destroy_qp(qp);         /* Destroy QP */
    if (cq)       ibv_destroy_cq(cq);         /* Destroy CQ */
    if (mr)       ibv_dereg_mr(mr);            /* Deregister MR */
    if (pd)       ibv_dealloc_pd(pd);          /* Deallocate PD */
    if (ctx)      ibv_close_device(ctx);       /* Close device */
    if (dev_list) ibv_free_device_list(dev_list); /* Free device list */
    if (gpu_ptr)  cuMemFree(gpu_ptr);          /* Free GPU memory */
    if (cu_ctx)   cuCtxDestroy(cu_ctx);        /* Destroy CUDA context */

    return ret;
}

#else /* !HAVE_CUDA */

/* ========== No CUDA Mode: Print Description ========== */

/**
 * gpudirect_demo - No CUDA mode, only prints GPUDirect RDMA workflow description
 */
static int gpudirect_demo(void)
{
    printf("=== GPUDirect RDMA Code Framework Description ===\n");
    printf("\n");
    printf("HAVE_CUDA is not defined in the current build. Here is the GPUDirect RDMA code flow:\n");
    printf("\n");
    printf("+--------------------------------------------------+\n");
    printf("|         GPUDirect RDMA Code Flow                  |\n");
    printf("+--------------------------------------------------+\n");
    printf("|                                                   |\n");
    printf("|  1. Initialize CUDA                               |\n");
    printf("|     cuInit(0)                                     |\n");
    printf("|     cuDeviceGet(&device, 0)                       |\n");
    printf("|     cuCtxCreate(&ctx, 0, device)                  |\n");
    printf("|                                                   |\n");
    printf("|  2. Allocate GPU memory                           |\n");
    printf("|     cuMemAlloc(&gpu_ptr, size)                    |\n");
    printf("|     // gpu_ptr is a device address, not CPU addr  |\n");
    printf("|                                                   |\n");
    printf("|  3. Initialize RDMA resources                     |\n");
    printf("|     ibv_get_device_list()                         |\n");
    printf("|     ibv_open_device()                             |\n");
    printf("|     ibv_alloc_pd()                                |\n");
    printf("|                                                   |\n");
    printf("|  4. Register GPU memory as RDMA MR <- Key step!   |\n");
    printf("|     ibv_reg_mr(pd, (void*)gpu_ptr, size, flags)   |\n");
    printf("|     // Requires nvidia_peermem kernel module       |\n");
    printf("|     // Failure -> check lsmod | grep nvidia_peermem|\n");
    printf("|                                                   |\n");
    printf("|  5. Use GPU MR like a regular MR                  |\n");
    printf("|     sge.addr = (uint64_t)gpu_ptr                  |\n");
    printf("|     sge.lkey = mr->lkey                           |\n");
    printf("|     ibv_post_send(qp, &wr, &bad_wr)              |\n");
    printf("|     // NIC reads GPU memory directly via PCIe     |\n");
    printf("|                                                   |\n");
    printf("|  6. Data path:                                    |\n");
    printf("|     GPU Memory -> PCIe -> NIC -> Network          |\n");
    printf("|     (Completely bypasses CPU memory copy!)         |\n");
    printf("|                                                   |\n");
    printf("+--------------------------------------------------+\n");
    printf("\n");
    printf("Build instructions:\n");
    printf("  With CUDA environment:\n");
    printf("    nvcc -DHAVE_CUDA -o gpudirect_framework gpudirect_framework.c \\\n");
    printf("         -libverbs -lcuda\n");
    printf("\n");
    printf("  Without CUDA environment (current mode):\n");
    printf("    gcc -o gpudirect_framework gpudirect_framework.c -libverbs\n");
    printf("\n");
    printf("Prerequisites:\n");
    printf("  1. NVIDIA GPU + driver\n");
    printf("  2. modprobe nvidia_peermem (or nv_peer_mem)\n");
    printf("  3. RDMA device available\n");
    printf("  4. Run gpudirect_check.sh to verify environment\n");
    printf("\n");
    printf("The only difference from a regular RDMA program:\n");
    printf("  Regular:    buf = malloc(size);     ibv_reg_mr(pd, buf, ...)\n");
    printf("  GPUDirect:  cuMemAlloc(&buf, size); ibv_reg_mr(pd, (void*)buf, ...)\n");
    printf("  The subsequent ibv_post_send / ibv_post_recv code is exactly the same!\n");

    return 0;
}

#endif /* HAVE_CUDA */

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    (void)argc;     /* Unused parameter */
    (void)argv;

    return gpudirect_demo();
}
