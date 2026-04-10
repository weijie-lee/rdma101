/**
 * 多设备枚举演示 (Multi-Device Enumeration)
 *
 * 本程序演示如何枚举系统中所有 RDMA 设备并查询完整拓扑:
 *   1. 使用 ibv_get_device_list() 获取所有设备
 *   2. 对每个设备: 打开、查询设备属性 (ibv_query_device)
 *   3. 对每个端口: 查询端口属性 (ibv_query_port), 打印所有 GID
 *   4. 显示完整拓扑: 设备名、传输类型、端口状态
 *
 * 编译: gcc -o 02_multi_device 02_multi_device.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/**
 * 辅助函数: 打印设备的 node_type
 */
static const char *node_type_str(enum ibv_node_type type)
{
    switch (type) {
    case IBV_NODE_CA:         return "CA (Channel Adapter)";
    case IBV_NODE_SWITCH:     return "Switch";
    case IBV_NODE_ROUTER:     return "Router";
    case IBV_NODE_RNIC:       return "RNIC (iWARP)";
    default:                  return "Unknown";
    }
}

/**
 * 辅助函数: 打印设备的 transport_type
 */
static const char *transport_type_str(enum ibv_transport_type type)
{
    switch (type) {
    case IBV_TRANSPORT_IB:      return "InfiniBand";
    case IBV_TRANSPORT_IWARP:   return "iWARP";
    default:                    return "Unknown";
    }
}

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;
    int num_devices;
    int i;

    printf("============================================\n");
    printf("  RDMA 多设备枚举与拓扑查询\n");
    printf("============================================\n\n");

    /* ========== 步骤 1: 获取设备列表 ========== */
    printf("[步骤1] 获取 RDMA 设备列表\n");
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "[错误] ibv_get_device_list 失败: %s\n", strerror(errno));
        return 1;
    }

    printf("  发现 %d 个 RDMA 设备\n\n", num_devices);

    if (num_devices == 0) {
        printf("  没有可用的 RDMA 设备。请检查:\n");
        printf("    - 是否安装了 RDMA 驱动 (mlx4_core, mlx5_core, rxe...)\n");
        printf("    - 是否加载了 ib_uverbs 内核模块\n");
        printf("    - lsmod | grep ib_\n");
        printf("    - ibstat 或 ibv_devinfo\n");
        goto cleanup;
    }

    /* ========== 步骤 2: 遍历每个设备 ========== */
    for (i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;
        struct ibv_device_attr dev_attr;

        printf("########################################\n");
        printf("# 设备 [%d/%d]: %s\n", i + 1, num_devices,
               ibv_get_device_name(dev));
        printf("########################################\n\n");

        /* 打印基本设备信息 (无需打开设备) */
        printf("  设备名:     %s\n", ibv_get_device_name(dev));
        printf("  GUID:       0x%016lx\n", (unsigned long)ibv_get_device_guid(dev));
        printf("  节点类型:   %s\n", node_type_str(dev->node_type));
        printf("  传输类型:   %s\n", transport_type_str(dev->transport_type));
        printf("\n");

        /* 打开设备 */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "  [警告] 无法打开设备 %s: %s\n",
                    ibv_get_device_name(dev), strerror(errno));
            printf("  跳过此设备\n\n");
            continue;
        }

        /* 使用工具库查询并打印设备属性 (~20 个关键字段) */
        printf("  --- 设备能力参数 ---\n");
        query_and_print_device(ctx);
        printf("\n");

        /* 查询设备属性以获取端口数量 */
        if (ibv_query_device(ctx, &dev_attr) != 0) {
            fprintf(stderr, "  [警告] ibv_query_device 失败\n");
            ibv_close_device(ctx);
            continue;
        }

        /* 遍历每个端口 */
        printf("  === 端口信息 (共 %d 个端口) ===\n\n", dev_attr.phys_port_cnt);

        int port;
        for (port = 1; port <= dev_attr.phys_port_cnt; port++) {
            printf("  ---- 端口 %d ----\n", port);

            /* 使用工具库查询并打印端口属性 */
            query_and_print_port(ctx, port);

            /* 检测传输层类型 */
            enum rdma_transport tp = detect_transport(ctx, port);
            printf("  检测到传输层: %s\n", transport_str(tp));

            /* 打印非零 GID 条目 */
            query_and_print_all_gids(ctx, port);

            printf("\n");
        }

        /* 关闭设备 */
        ibv_close_device(ctx);
    }

    /* ========== 总结 ========== */
    printf("============================================\n");
    printf("  拓扑枚举总结\n");
    printf("============================================\n");
    printf("  共发现 %d 个 RDMA 设备\n\n", num_devices);
    printf("  API 调用链:\n");
    printf("    ibv_get_device_list()  → 获取设备列表\n");
    printf("    ibv_get_device_name()  → 设备名 (如 mlx5_0)\n");
    printf("    ibv_get_device_guid()  → 设备 GUID\n");
    printf("    ibv_open_device()      → 获取上下文\n");
    printf("    ibv_query_device()     → 设备能力 (max_qp, max_mr, ...)\n");
    printf("    ibv_query_port()       → 端口状态 (state, LID, MTU, ...)\n");
    printf("    ibv_query_gid()        → GID 表 (RoCE 寻址需要)\n");
    printf("    ibv_close_device()     → 关闭设备\n");
    printf("    ibv_free_device_list() → 释放列表\n\n");

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    printf("程序结束\n");
    return 0;
}
