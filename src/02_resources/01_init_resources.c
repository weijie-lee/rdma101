/**
 * RDMA资源初始化 - 六步法
 * 
 * 本程序演示RDMA编程的六个初始化步骤：
 * 1. 获取设备列表
 * 2. 打开设备
 * 3. 分配保护域(PD)
 * 4. 注册内存区域(MR)
 * 5. 创建完成队列(CQ)
 * 6. 创建队列对(QP)
 * 
 * 编译: gcc -o 01_init_resources 01_init_resources.c -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[])
{
    struct ibv_device **device_list = NULL;
    struct ibv_device *device = NULL;
    struct ibv_context *context = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp = NULL;
    char *buffer = NULL;
    int num_devices;
    int i;
    
    printf("=== RDMA资源初始化 - 六步法 ===\n\n");
    
    /* ========== 步骤1: 获取设备列表 ========== */
    printf("[步骤1] 获取设备列表\n");
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        perror("  ibv_get_device_list 失败");
        return 1;
    }
    printf("  发现 %d 个RDMA设备\n", num_devices);
    
    for (i = 0; i < num_devices; i++) {
        printf("  设备[%d]: %s\n", i, ibv_get_device_name(device_list[i]));
    }
    
    /* 选择第一个设备 */
    device = device_list[0];
    if (!device) {
        fprintf(stderr, "  没有可用设备\n");
        goto cleanup;
    }
    
    /* ========== 步骤2: 打开设备 ========== */
    printf("\n[步骤2] 打开设备\n");
    context = ibv_open_device(device);
    if (!context) {
        perror("  ibv_open_device 失败");
        goto cleanup;
    }
    printf("  设备打开成功: %s\n", ibv_get_device_name(device));
    
    /* ========== 步骤3: 分配保护域(PD) ========== */
    printf("\n[步骤3] 分配保护域(PD)\n");
    pd = ibv_alloc_pd(context);
    if (!pd) {
        perror("  ibv_alloc_pd 失败");
        goto cleanup;
    }
    printf("  PD分配成功, handle=%d\n", pd->handle);
    
    /* ========== 步骤4: 注册内存区域(MR) ========== */
    printf("\n[步骤4] 注册内存区域(MR)\n");
    buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("  malloc 失败");
        goto cleanup;
    }
    memset(buffer, 0, BUFFER_SIZE);
    
    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, 
                     IBV_ACCESS_LOCAL_WRITE |
                     IBV_ACCESS_REMOTE_READ |
                     IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("  ibv_reg_mr 失败");
        goto cleanup;
    }
    printf("  MR注册成功\n");
    printf("    lkey=0x%x (本地访问)\n", mr->lkey);
    printf("    rkey=0x%x (远程访问)\n", mr->rkey);
    printf("    虚拟地址=%p\n", mr->addr);
    printf("    长度=%d\n", mr->length);
    
    /* ========== 步骤5: 创建完成队列(CQ) ========== */
    printf("\n[步骤5] 创建完成队列(CQ)\n");
    cq = ibv_create_cq(context, 128, NULL, NULL, 0);
    if (!cq) {
        perror("  ibv_create_cq 失败");
        goto cleanup;
    }
    printf("  CQ创建成功, 大小=%d\n", 128);
    printf("    cq_handle=%d\n", cq->handle);
    
    /* ========== 步骤6: 创建队列对(QP) ========== */
    printf("\n[步骤6] 创建队列对(QP)\n");
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 128,
            .max_recv_wr = 128,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    
    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        perror("  ibv_create_qp 失败");
        goto cleanup;
    }
    printf("  QP创建成功\n");
    printf("    QP号=%u\n", qp->qp_num);
    printf("    状态=%d (RESET=0)\n", ibv_query_qp_state(qp));
    
    printf("\n=== 所有资源初始化成功 ===\n\n");
    
    /* 打印资源关系 */
    printf("资源关系:\n");
    printf("  Device -> Context (ibv_open_device)\n");
    printf("  Context -> PD (ibv_alloc_pd)\n");
    printf("  PD -> MR (ibv_reg_mr) - 用于内存注册\n");
    printf("  Context -> CQ (ibv_create_cq) - 用于完成通知\n");
    printf("  PD + CQ -> QP (ibv_create_qp) - 用于通信\n");
    
cleanup:
    /* 逆序释放资源 */
    printf("\n[清理] 释放资源...\n");
    
    if (qp) {
        ibv_destroy_qp(qp);
        printf("  QP已销毁\n");
    }
    if (cq) {
        ibv_destroy_cq(cq);
        printf("  CQ已销毁\n");
    }
    if (mr) {
        ibv_dereg_mr(mr);
        printf("  MR已注销\n");
    }
    if (buffer) {
        free(buffer);
    }
    if (pd) {
        ibv_dealloc_pd(pd);
        printf("  PD已释放\n");
    }
    if (context) {
        ibv_close_device(context);
        printf("  设备已关闭\n");
    }
    if (device_list) {
        ibv_free_device_list(device_list);
        printf("  设备列表已释放\n");
    }
    
    printf("\n程序结束\n");
    return 0;
}
