/**
 * Multiple Registration of the Same Buffer Demo
 *
 * This program demonstrates:
 *   1. The same malloc'd memory can be registered as different MRs multiple times -> each time lkey/rkey differs
 *   2. Overlapping MRs are allowed
 *   3. Discussion of performance impact: why you should pre-register and avoid frequent reg/dereg
 *
 * Compile: gcc -o mr_multi_reg mr_multi_reg.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE   (64 * 1024)   /* 64 KB buffer */
#define NUM_MR        5             /* Register same buffer 5 times */
#define PERF_ITER     100           /* Performance test iteration count */

/**
 * Helper function: get current time (nanoseconds)
 */
static uint64_t get_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(int argc, char *argv[])
{
    /* Resource declarations */
    struct ibv_device **dev_list = NULL;
    struct ibv_context *ctx      = NULL;
    struct ibv_pd      *pd       = NULL;
    struct ibv_mr      *mr[NUM_MR] = {NULL};
    struct ibv_mr      *mr_overlap = NULL;   /* Overlapping registration */
    char               *buffer   = NULL;
    int                 num_devices;
    int                 i;

    printf("============================================\n");
    printf("  Multiple Registration of Same Buffer Demo\n");
    printf("============================================\n\n");

    /* ========== Open device, create PD ========== */
    printf("[Step 1] Open device and create PD\n");
    dev_list = ibv_get_device_list(&num_devices);
    CHECK_NULL(dev_list, "Failed to get device list");
    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        goto cleanup;
    }
    ctx = ibv_open_device(dev_list[0]);
    CHECK_NULL(ctx, "Failed to open device");
    printf("  Device: %s\n", ibv_get_device_name(dev_list[0]));

    pd = ibv_alloc_pd(ctx);
    CHECK_NULL(pd, "Failed to allocate PD");
    printf("  PD allocated successfully\n\n");

    /* ========== Step 2: Allocate shared buffer ========== */
    printf("[Step 2] Allocate %d KB buffer\n", BUFFER_SIZE / 1024);
    buffer = malloc(BUFFER_SIZE);
    CHECK_NULL(buffer, "malloc failed");
    memset(buffer, 0, BUFFER_SIZE);
    printf("  Buffer address: %p, size: %d bytes\n\n", buffer, BUFFER_SIZE);

    /* ========== Step 3: Register same buffer multiple times ========== */
    printf("[Step 3] Register %d MRs for the same buffer\n", NUM_MR);
    printf("========================================\n");

    for (i = 0; i < NUM_MR; i++) {
        /*
         * The same memory can be registered multiple times, each time yielding
         * a different MR object with different lkey/rkey.
         * This is a completely legal operation. RDMA hardware creates independent
         * page table mappings for each registration.
         */
        int flags = IBV_ACCESS_LOCAL_WRITE;
        if (i >= 1) flags |= IBV_ACCESS_REMOTE_READ;
        if (i >= 2) flags |= IBV_ACCESS_REMOTE_WRITE;

        mr[i] = ibv_reg_mr(pd, buffer, BUFFER_SIZE, flags);
        CHECK_NULL(mr[i], "ibv_reg_mr failed");

        printf("  MR[%d]: lkey=0x%08x  rkey=0x%08x  addr=%p  len=%zu\n",
               i, mr[i]->lkey, mr[i]->rkey, mr[i]->addr,
               (size_t)mr[i]->length);
    }

    printf("\n  Key observations:\n");
    printf("    - All MRs point to the same physical memory (same addr)\n");
    printf("    - But each MR has different lkey/rkey values!\n");
    printf("    - This means you can create different permission \"views\" for the same memory\n");
    printf("    - Example: give peer A a read-only rkey, give peer B a read-write rkey\n\n");

    /* ========== Step 4: Overlapping region registration ========== */
    printf("[Step 4] Register overlapping region MR\n");
    printf("========================================\n");
    printf("  Existing MR covers: [%p, %p)\n", buffer, buffer + BUFFER_SIZE);
    printf("  New registration:   [%p, %p) (first half)\n", buffer, buffer + BUFFER_SIZE / 2);

    mr_overlap = ibv_reg_mr(pd, buffer, BUFFER_SIZE / 2, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(mr_overlap, "Failed to register overlapping MR");
    printf("  MR_overlap: lkey=0x%08x  rkey=0x%08x  len=%zu\n",
           mr_overlap->lkey, mr_overlap->rkey, (size_t)mr_overlap->length);
    printf("  Overlapping registration succeeded! RDMA allows any sub-region of the same memory to be registered\n\n");

    /* ========== Step 5: MR registration/deregistration performance test ========== */
    printf("[Step 5] MR registration/deregistration performance test (%d iterations)\n", PERF_ITER);
    printf("========================================\n");

    uint64_t t_start, t_end;
    struct ibv_mr *mr_perf;
    char *perf_buf = malloc(4096);
    CHECK_NULL(perf_buf, "malloc perf_buf failed");
    memset(perf_buf, 0, 4096);

    /* Measure registration time */
    t_start = get_ns();
    for (i = 0; i < PERF_ITER; i++) {
        mr_perf = ibv_reg_mr(pd, perf_buf, 4096, IBV_ACCESS_LOCAL_WRITE);
        if (!mr_perf) {
            fprintf(stderr, "  reg_mr failed during performance test (iter=%d)\n", i);
            break;
        }
        ibv_dereg_mr(mr_perf);
    }
    t_end = get_ns();

    if (i == PERF_ITER) {
        uint64_t total_ns = t_end - t_start;
        printf("  %d reg_mr + dereg_mr total time: %.2f ms\n",
               PERF_ITER, total_ns / 1e6);
        printf("  Average per reg+dereg: %.1f us\n",
               (double)total_ns / PERF_ITER / 1000.0);
        printf("\n  Performance insights:\n");
        printf("    - ibv_reg_mr() is a heavyweight operation (needs to pin memory + build page tables)\n");
        printf("    - Should register once during initialization, avoid repeated reg/dereg on data path\n");
        printf("    - Pre-register large memory blocks, then use different regions via offsets\n");
        printf("    - For dynamic buffers, consider using Memory Window (MW) or ODP\n");
    }

    free(perf_buf);

    /* ========== Summary ========== */
    printf("\n============================================\n");
    printf("  Multiple Registration Summary\n");
    printf("============================================\n");
    printf("  1. Same virtual memory can be registered as multiple MRs (different lkey/rkey)\n");
    printf("  2. Overlapping memory region registration is legal\n");
    printf("  3. Each MR can have different access permissions (different security views)\n");
    printf("  4. Performance best practices:\n");
    printf("     - Pre-register (register during application init)\n");
    printf("     - Avoid reg/dereg on hot path (overhead ~tens of microseconds)\n");
    printf("     - Register large buffer once, use sub-regions via offsets\n");
    printf("     - Consider Memory Pool + pre-registration pattern\n\n");

cleanup:
    printf("[Cleanup] Releasing resources...\n");
    if (mr_overlap) ibv_dereg_mr(mr_overlap);
    for (i = NUM_MR - 1; i >= 0; i--) {
        if (mr[i]) ibv_dereg_mr(mr[i]);
    }
    if (buffer)   free(buffer);
    if (pd)       ibv_dealloc_pd(pd);
    if (ctx)      ibv_close_device(ctx);
    if (dev_list) ibv_free_device_list(dev_list);

    printf("  Done\n");
    return 0;
}
