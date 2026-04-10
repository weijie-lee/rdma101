/**
 * iwarp_query.c - iWARP 设备查询程序
 *
 * 功能：
 *   - 查询所有 RDMA 设备
 *   - 通过 node_type 字段区分 IB/RoCE (IBV_NODE_CA) 和 iWARP (IBV_NODE_RNIC)
 *   - 打印每个设备的传输类型信息
 *   - 解释 iWARP 的特殊之处
 *
 * 编译:
 *   gcc -o iwarp_query iwarp_query.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * 预期输出:
 *   设备: rxe_0
 *     node_type   = IBV_NODE_CA (1) -- Channel Adapter (IB 或 RoCE)
 *     link_layer  = Ethernet
 *     传输类型    = RoCE
 *   (若有 iWARP 设备:)
 *   设备: cxgb4_0
 *     node_type   = IBV_NODE_RNIC (4) -- RDMA NIC (iWARP)
 *     link_layer  = Ethernet
 *     传输类型    = iWARP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>

/* 引入公共工具库 */
#include "rdma_utils.h"

/* ========== 辅助函数 ========== */

/**
 * node_type_str - 将 node_type 枚举转为描述字符串
 *
 * node_type 是区分 IB/RoCE 与 iWARP 的关键字段：
 *   - IBV_NODE_CA (1): Channel Adapter，用于 InfiniBand 和 RoCE
 *   - IBV_NODE_SWITCH (2): IB 交换机
 *   - IBV_NODE_ROUTER (3): IB 路由器
 *   - IBV_NODE_RNIC (4): RDMA NIC，专门用于 iWARP
 */
static const char *node_type_str(enum ibv_node_type type)
{
    switch (type) {
    case IBV_NODE_CA:
        return "IBV_NODE_CA -- Channel Adapter (IB 或 RoCE)";
    case IBV_NODE_SWITCH:
        return "IBV_NODE_SWITCH -- InfiniBand 交换机";
    case IBV_NODE_ROUTER:
        return "IBV_NODE_ROUTER -- InfiniBand 路由器";
    case IBV_NODE_RNIC:
        return "IBV_NODE_RNIC -- RDMA NIC (iWARP 专用)";
    default:
        return "UNKNOWN -- 未知类型";
    }
}

/**
 * link_layer_name - 将 link_layer 转为字符串
 */
static const char *link_layer_name(uint8_t ll)
{
    switch (ll) {
    case IBV_LINK_LAYER_UNSPECIFIED:    return "Unspecified";
    case IBV_LINK_LAYER_INFINIBAND:     return "InfiniBand";
    case IBV_LINK_LAYER_ETHERNET:       return "Ethernet";
    default:                            return "Unknown";
    }
}

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* 设备列表 */
    int num_devices = 0;                    /* 设备数量 */
    int ret = 0;                            /* 返回值 */
    int ib_count = 0;                       /* IB 设备计数 */
    int roce_count = 0;                     /* RoCE 设备计数 */
    int iwarp_count = 0;                    /* iWARP 设备计数 */

    printf("==============================================\n");
    printf("  iWARP 设备查询工具\n");
    printf("  通过 node_type 区分 IB/RoCE/iWARP\n");
    printf("==============================================\n");

    /* 第一步：获取设备列表 */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "[错误] ibv_get_device_list() 失败: %s\n",
                strerror(errno));
        return 1;
    }

    if (num_devices == 0) {
        fprintf(stderr, "[错误] 未找到 RDMA 设备\n");
        ret = 1;
        goto cleanup;
    }

    printf("\n共发现 %d 个 RDMA 设备\n\n", num_devices);

    /* 第二步：遍历每个设备，打印 node_type 信息 */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;             /* 设备上下文 */
        struct ibv_device_attr dev_attr;            /* 设备属性 */
        struct ibv_port_attr port_attr;             /* 端口属性 */

        printf("───────────────────────────────────────\n");
        printf("设备 %d: %s\n", i, ibv_get_device_name(dev));
        printf("───────────────────────────────────────\n");

        /* 读取 node_type (无需打开设备即可获取) */
        enum ibv_node_type ntype = dev->node_type;
        printf("  node_type   = %s (%d)\n", node_type_str(ntype), ntype);

        /* 读取 GUID */
        uint64_t guid = ibv_get_device_guid(dev);
        printf("  node_guid   = %016llx\n", (unsigned long long)guid);

        /* 打开设备以查询更多信息 */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "  [错误] 无法打开设备: %s\n", strerror(errno));
            continue;
        }

        /* 查询设备属性 */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "  [错误] ibv_query_device() 失败\n");
            ibv_close_device(ctx);
            continue;
        }

        printf("  固件版本    = %s\n", dev_attr.fw_ver);
        printf("  端口数量    = %d\n", dev_attr.phys_port_cnt);

        /* 查询第一个端口的链路层类型 */
        memset(&port_attr, 0, sizeof(port_attr));
        ret = ibv_query_port(ctx, 1, &port_attr);
        if (ret == 0) {
            printf("  link_layer  = %s\n",
                   link_layer_name(port_attr.link_layer));
        }

        /* 使用公共库的 detect_transport() 综合判断 */
        enum rdma_transport transport = detect_transport(ctx, 1);
        printf("  传输类型    = %s\n", transport_str(transport));

        /* 统计 */
        switch (ntype) {
        case IBV_NODE_RNIC:
            iwarp_count++;                  /* iWARP 设备 */
            break;
        case IBV_NODE_CA:
            if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)
                roce_count++;               /* RoCE 设备 */
            else
                ib_count++;                 /* IB 设备 */
            break;
        default:
            break;
        }

        printf("\n");
        ibv_close_device(ctx);
    }

    /* 第三步：打印总结和 iWARP 说明 */
    printf("==============================================\n");
    printf("  设备统计\n");
    printf("==============================================\n");
    printf("  InfiniBand 设备: %d 个\n", ib_count);
    printf("  RoCE 设备:       %d 个\n", roce_count);
    printf("  iWARP 设备:      %d 个\n", iwarp_count);
    printf("\n");

    /* 打印 iWARP 特殊说明 */
    printf("==============================================\n");
    printf("  iWARP 编程要点\n");
    printf("==============================================\n");
    printf("\n");
    printf("  1. 识别方法:\n");
    printf("     - node_type == IBV_NODE_RNIC (值为 %d)\n", IBV_NODE_RNIC);
    printf("     - IB/RoCE 的 node_type 为 IBV_NODE_CA (值为 %d)\n",
           IBV_NODE_CA);
    printf("\n");
    printf("  2. 建连方式:\n");
    printf("     - iWARP 底层是 TCP，推荐使用 RDMA CM 建立连接\n");
    printf("     - rdma_resolve_addr() → rdma_resolve_route() → rdma_connect()\n");
    printf("\n");
    printf("  3. 与 IB/RoCE 的关键差异:\n");
    printf("     - 不需要无损网络 (TCP 自带重传)\n");
    printf("     - 可穿越 NAT/防火墙\n");
    printf("     - 性能略低于 IB/RoCE (TCP 处理开销)\n");
    printf("     - QP 类型通常仅支持 RC (不支持 UC/UD)\n");
    printf("\n");

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    return (ret < 0) ? 1 : 0;
}
