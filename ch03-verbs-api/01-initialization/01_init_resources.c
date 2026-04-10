/**
 * RDMA资源初始化 - 六步法 (增强版)
 *
 * 本程序演示RDMA编程的六个初始化步骤：
 * 1. 获取设备列表
 * 2. 打开设备 + 查询设备属性 (ibv_query_device) + 查询端口属性 (ibv_query_port)
 * 3. 分配保护域(PD)
 * 4. 注册内存区域(MR)
 * 5. 创建完成队列(CQ)
 * 6. 创建队列对(QP)
 *
 * 编译: gcc -o 01_init_resources 01_init_resources.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

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

    /* ========== 步骤2a: 查询设备属性 (ibv_query_device) ========== */
    printf("\n[步骤2a] 查询设备属性 (ibv_query_device)\n");
    struct ibv_device_attr dev_attr;
    if (ibv_query_device(context, &dev_attr) == 0) {
        printf("  --- 基本信息 ---\n");
        printf("  固件版本 (fw_ver):          %s\n", dev_attr.fw_ver);
        printf("  节点 GUID:                  0x%016lx\n", (unsigned long)dev_attr.node_guid);
        printf("  系统镜像 GUID:              0x%016lx\n", (unsigned long)dev_attr.sys_image_guid);
        printf("  厂商 ID:                    0x%x\n", dev_attr.vendor_id);
        printf("  厂商 Part ID:               %d\n", dev_attr.vendor_part_id);
        printf("  硬件版本:                   %d\n", dev_attr.hw_ver);
        printf("  物理端口数:                 %d\n", dev_attr.phys_port_cnt);
        printf("  --- 队列能力 ---\n");
        printf("  最大 QP 数量:               %d\n", dev_attr.max_qp);
        printf("  每个 QP 最大 WR:            %d\n", dev_attr.max_qp_wr);
        printf("  每个 WR 最大 SGE:           %d\n", dev_attr.max_sge);
        printf("  最大 CQ 数量:               %d\n", dev_attr.max_cq);
        printf("  每个 CQ 最大 CQE:           %d\n", dev_attr.max_cqe);
        printf("  最大 SRQ 数量:              %d\n", dev_attr.max_srq);
        printf("  每个 SRQ 最大 WR:           %d\n", dev_attr.max_srq_wr);
        printf("  --- 内存区域 ---\n");
        printf("  最大 MR 数量:               %d\n", dev_attr.max_mr);
        printf("  最大 MR 大小:               %lu bytes\n", (unsigned long)dev_attr.max_mr_size);
        printf("  最大 PD 数量:               %d\n", dev_attr.max_pd);
        printf("  页面大小掩码:               0x%lx\n", (unsigned long)dev_attr.page_size_cap);
        printf("  --- 原子操作 ---\n");
        printf("  原子操作能力:               ");
        switch (dev_attr.atomic_cap) {
        case IBV_ATOMIC_NONE:  printf("NONE (不支持)\n"); break;
        case IBV_ATOMIC_HCA:   printf("HCA (仅本 HCA 内原子)\n"); break;
        case IBV_ATOMIC_GLOB:  printf("GLOBAL (全局原子)\n"); break;
        default:               printf("%d\n", dev_attr.atomic_cap); break;
        }
        printf("  最大 QP RD 原子操作数:      %d\n", dev_attr.max_qp_rd_atom);
        printf("  最大 QP INIT RD 原子数:     %d\n", dev_attr.max_qp_init_rd_atom);
        printf("  --- 其他 ---\n");
        printf("  最大 AH 数量:               %d\n", dev_attr.max_ah);
        printf("  最大多播组数:               %d\n", dev_attr.max_mcast_grp);
        printf("  每个多播组最大 QP 数:       %d\n", dev_attr.max_mcast_qp_attach);
    } else {
        perror("  ibv_query_device 失败");
    }

    /* ========== 步骤2b: 查询所有端口属性 (ibv_query_port) ========== */
    printf("\n[步骤2b] 查询端口属性 (ibv_query_port)\n");
    int phys_port_cnt = (ibv_query_device(context, &dev_attr) == 0)
                        ? dev_attr.phys_port_cnt : 1;
    for (int port = 1; port <= phys_port_cnt; port++) {
        struct ibv_port_attr port_attr;
        if (ibv_query_port(context, port, &port_attr) == 0) {
            printf("  --- 端口 %d ---\n", port);
            printf("  状态:          ");
            switch (port_attr.state) {
            case IBV_PORT_DOWN:    printf("DOWN\n"); break;
            case IBV_PORT_INIT:    printf("INIT\n"); break;
            case IBV_PORT_ARMED:   printf("ARMED\n"); break;
            case IBV_PORT_ACTIVE:  printf("ACTIVE\n"); break;
            default:               printf("%d\n", port_attr.state); break;
            }
            printf("  最大 MTU:      ");
            switch (port_attr.max_mtu) {
            case IBV_MTU_256:  printf("256\n"); break;
            case IBV_MTU_512:  printf("512\n"); break;
            case IBV_MTU_1024: printf("1024\n"); break;
            case IBV_MTU_2048: printf("2048\n"); break;
            case IBV_MTU_4096: printf("4096\n"); break;
            default:           printf("%d\n", port_attr.max_mtu); break;
            }
            printf("  活动 MTU:      ");
            switch (port_attr.active_mtu) {
            case IBV_MTU_256:  printf("256\n"); break;
            case IBV_MTU_512:  printf("512\n"); break;
            case IBV_MTU_1024: printf("1024\n"); break;
            case IBV_MTU_2048: printf("2048\n"); break;
            case IBV_MTU_4096: printf("4096\n"); break;
            default:           printf("%d\n", port_attr.active_mtu); break;
            }
            printf("  链路层:        %s\n",
                   port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" :
                   port_attr.link_layer == IBV_LINK_LAYER_ETHERNET   ? "Ethernet (RoCE)" :
                   "Unknown");
            printf("  LID:           %u%s\n", port_attr.lid,
                   port_attr.lid == 0 ? " (RoCE 模式, 需用 GID 寻址)" : "");
            printf("  SM LID:        %u\n", port_attr.sm_lid);
            printf("  GID 表大小:    %d\n", port_attr.gid_tbl_len);
            printf("  P_Key 表大小:  %u\n", port_attr.pkey_tbl_len);
            printf("  活动速率:      width=%d, speed=%d\n",
                   port_attr.active_width, port_attr.active_speed);
        } else {
            printf("  端口 %d 查询失败\n", port);
        }
    }

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
    printf("    状态=RESET (刚创建的 QP 处于 RESET 状态)\n");
    
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
