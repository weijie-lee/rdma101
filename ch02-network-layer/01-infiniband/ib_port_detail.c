/**
 * ib_port_detail.c - InfiniBand 端口属性完整查询程序
 *
 * 功能：
 *   - 打开每个 RDMA 设备
 *   - 对每个端口调用 ibv_query_port()
 *   - 打印 ibv_port_attr 结构体的所有字段，附中文注释
 *   - 使用 common/rdma_utils.h 的 detect_transport() 检测传输类型
 *
 * 编译:
 *   gcc -o ib_port_detail ib_port_detail.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * 或使用 Makefile:
 *   make
 *
 * 预期输出:
 *   ===== 设备: rxe_0 =====
 *     传输类型: RoCE
 *     --- 端口 1 属性 ---
 *     state            = 4 (ACTIVE)          -- 端口状态
 *     max_mtu          = 4096                -- 最大支持 MTU
 *     active_mtu       = 1024                -- 当前活动 MTU
 *     ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>

/* 引入公共工具库 */
#include "rdma_utils.h"

/* ========== 辅助函数：将枚举值转为可读字符串 ========== */

/**
 * port_state_str - 将端口状态枚举转为中文字符串
 */
static const char *port_state_str(enum ibv_port_state state)
{
    switch (state) {
    case IBV_PORT_NOP:          return "NOP (无操作/保留)";
    case IBV_PORT_DOWN:         return "DOWN (端口关闭)";
    case IBV_PORT_INIT:         return "INIT (初始化中)";
    case IBV_PORT_ARMED:        return "ARMED (已就绪，等待激活)";
    case IBV_PORT_ACTIVE:       return "ACTIVE (活动)";
    default:                    return "UNKNOWN (未知)";
    }
}

/**
 * mtu_to_bytes - 将 ibv_mtu 枚举转为实际字节数
 */
static int mtu_to_bytes(enum ibv_mtu mtu)
{
    switch (mtu) {
    case IBV_MTU_256:   return 256;
    case IBV_MTU_512:   return 512;
    case IBV_MTU_1024:  return 1024;
    case IBV_MTU_2048:  return 2048;
    case IBV_MTU_4096:  return 4096;
    default:            return 0;
    }
}

/**
 * link_layer_str - 将链路层类型转为可读字符串
 */
static const char *link_layer_str(uint8_t link_layer)
{
    switch (link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED:    return "UNSPECIFIED (未指定)";
    case IBV_LINK_LAYER_INFINIBAND:     return "INFINIBAND (InfiniBand)";
    case IBV_LINK_LAYER_ETHERNET:       return "ETHERNET (以太网/RoCE)";
    default:                            return "UNKNOWN (未知)";
    }
}

/**
 * width_str - 将链路宽度转为可读字符串
 */
static const char *width_str(uint8_t width)
{
    switch (width) {
    case 1:  return "1X";
    case 2:  return "4X";
    case 4:  return "8X";
    case 8:  return "12X";
    default: return "UNKNOWN";
    }
}

/**
 * speed_str - 将链路速度转为可读字符串
 */
static const char *speed_str(uint8_t speed)
{
    switch (speed) {
    case 1:  return "2.5 Gbps (SDR)";
    case 2:  return "5.0 Gbps (DDR)";
    case 4:  return "10.0 Gbps (QDR)";     /* 也可能是 10GbE */
    case 8:  return "10.0 Gbps (FDR10)";
    case 16: return "14.0 Gbps (FDR)";
    case 32: return "25.0 Gbps (EDR)";
    case 64: return "50.0 Gbps (HDR)";
    default: return "UNKNOWN";
    }
}

/**
 * print_port_attr - 打印 ibv_port_attr 结构体的所有字段
 *
 * 每个字段都附有中文注释，解释其用途
 */
static void print_port_attr(const struct ibv_port_attr *attr, uint8_t port_num)
{
    printf("\n  --- 端口 %u 属性 ---\n", port_num);

    /* 端口状态: 标识端口当前运行状态 (DOWN/INIT/ARMED/ACTIVE) */
    printf("  %-20s = %d (%s)\n", "state",
           attr->state, port_state_str(attr->state));

    /* 最大 MTU: 端口硬件支持的最大传输单元 */
    printf("  %-20s = %d (%d 字节)\n", "max_mtu",
           attr->max_mtu, mtu_to_bytes(attr->max_mtu));

    /* 当前活动 MTU: 协商后的实际使用 MTU */
    printf("  %-20s = %d (%d 字节)\n", "active_mtu",
           attr->active_mtu, mtu_to_bytes(attr->active_mtu));

    /* GID 表长度: 该端口的 GID 条目数量 (RoCE 寻址用) */
    printf("  %-20s = %d\n", "gid_tbl_len",
           attr->gid_tbl_len);

    /* 端口能力标志: 位掩码，指示端口支持的功能 */
    printf("  %-20s = 0x%08x\n", "port_cap_flags",
           attr->port_cap_flags);

    /* 最大消息大小: 单个消息允许的最大字节数 */
    printf("  %-20s = %u\n", "max_msg_sz",
           attr->max_msg_sz);

    /* 坏的 P_Key 计数: 收到无效 P_Key 的报文计数 (安全相关) */
    printf("  %-20s = %u\n", "bad_pkey_cntr",
           attr->bad_pkey_cntr);

    /* Q_Key 违规计数: 收到错误 Q_Key 的 UD 报文计数 */
    printf("  %-20s = %u\n", "qkey_viol_cntr",
           attr->qkey_viol_cntr);

    /* P_Key 表长度: 分区密钥表的条目数 (多租户隔离用) */
    printf("  %-20s = %u\n", "pkey_tbl_len",
           attr->pkey_tbl_len);

    /* LID (Local Identifier): IB 子网内的本地标识符 (由 SM 分配) */
    printf("  %-20s = %u (0x%04x)\n", "lid",
           attr->lid, attr->lid);

    /* SM LID: Subnet Manager 的 LID */
    printf("  %-20s = %u (0x%04x)\n", "sm_lid",
           attr->sm_lid, attr->sm_lid);

    /* LMC (LID Mask Control): 允许一个端口拥有 2^LMC 个连续 LID */
    printf("  %-20s = %u\n", "lmc",
           attr->lmc);

    /* 最大 VL 数: 支持的虚拟通道数量 (QoS 相关) */
    printf("  %-20s = %u\n", "max_vl_num",
           attr->max_vl_num);

    /* SM SL: 到达 Subnet Manager 使用的 Service Level */
    printf("  %-20s = %u\n", "sm_sl",
           attr->sm_sl);

    /* 子网超时: 子网管理相关的超时值 */
    printf("  %-20s = %u\n", "subnet_timeout",
           attr->subnet_timeout);

    /* 初始化类型回复: 端口初始化类型信息 */
    printf("  %-20s = %u\n", "init_type_reply",
           attr->init_type_reply);

    /* 活动宽度: 当前链路协商的宽度 (1X/4X/8X/12X) */
    printf("  %-20s = %u (%s)\n", "active_width",
           attr->active_width, width_str(attr->active_width));

    /* 活动速度: 当前链路协商的速度 */
    printf("  %-20s = %u (%s)\n", "active_speed",
           attr->active_speed, speed_str(attr->active_speed));

    /* 物理状态: 物理链路状态 (Sleep/Polling/LinkUp 等) */
    printf("  %-20s = %u\n", "phys_state",
           attr->phys_state);

    /* 链路层类型: InfiniBand / Ethernet (RoCE) / Unspecified */
    printf("  %-20s = %u (%s)\n", "link_layer",
           attr->link_layer, link_layer_str(attr->link_layer));
}

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* 设备列表指针 */
    int num_devices = 0;                    /* 设备数量 */
    int ret = 0;                            /* 返回值 */

    printf("==============================================\n");
    printf("  IB 端口属性完整查询工具\n");
    printf("  使用 ibv_query_port() 显示所有字段\n");
    printf("==============================================\n");

    /* 第一步：获取所有 RDMA 设备列表 */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "[错误] ibv_get_device_list() 失败: %s\n",
                strerror(errno));
        return 1;
    }

    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未找到任何 RDMA 设备\n");
        fprintf(stderr, "  提示: 请确认 RDMA 驱动已加载\n");
        fprintf(stderr, "  - SoftRoCE: sudo rdma link add rxe_0 type rxe netdev eth0\n");
        fprintf(stderr, "  - IB: modprobe mlx5_ib\n");
        ret = 1;
        goto cleanup;
    }

    printf("\n共发现 %d 个 RDMA 设备\n", num_devices);

    /* 第二步：遍历每个设备 */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];           /* 当前设备 */
        struct ibv_context *ctx = NULL;                 /* 设备上下文 */
        struct ibv_device_attr dev_attr;                /* 设备属性 */

        printf("\n===== 设备 %d: %s =====\n", i, ibv_get_device_name(dev));

        /* 打开设备，获取上下文句柄 */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "[错误] 无法打开设备 %s: %s\n",
                    ibv_get_device_name(dev), strerror(errno));
            continue;   /* 跳过此设备，继续下一个 */
        }

        /* 使用公共库的 detect_transport() 检测传输类型 */
        enum rdma_transport transport = detect_transport(ctx, 1);
        printf("  传输类型: %s\n", transport_str(transport));

        /* 查询设备属性，获取端口数量 */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "[错误] ibv_query_device() 失败: %s\n",
                    strerror(errno));
            ibv_close_device(ctx);
            continue;
        }

        printf("  设备端口数量: %d\n", dev_attr.phys_port_cnt);

        /* 第三步：遍历每个端口，查询并打印属性 */
        for (uint8_t port = 1; port <= dev_attr.phys_port_cnt; port++) {
            struct ibv_port_attr port_attr;             /* 端口属性结构体 */

            memset(&port_attr, 0, sizeof(port_attr));

            /* 查询指定端口的属性 */
            ret = ibv_query_port(ctx, port, &port_attr);
            if (ret) {
                fprintf(stderr, "[错误] ibv_query_port(port=%u) 失败: %s\n",
                        port, strerror(errno));
                continue;   /* 跳过此端口 */
            }

            /* 打印该端口的所有属性字段 */
            print_port_attr(&port_attr, port);
        }

        /* 关闭设备 */
        ibv_close_device(ctx);
    }

cleanup:
    /* 释放设备列表 */
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    printf("\n查询完成。\n");
    return ret;
}
