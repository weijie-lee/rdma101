/**
 * roce_gid_query.c - RoCE GID 表枚举与分析程序
 *
 * 功能：
 *   - 枚举所有设备的所有端口的 GID 条目
 *   - 自动检测 IB vs RoCE (link_layer == ETHERNET → RoCE)
 *   - 格式化打印每个 GID
 *   - 分析 GID 类型 (link-local / IPv4-mapped / IPv6)
 *   - 建议 RoCE v2 应使用的 GID 索引
 *   - 解释 ah_attr 在 IB 和 RoCE 下的差异
 *
 * 编译:
 *   gcc -o roce_gid_query roce_gid_query.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * 或使用 Makefile:
 *   make
 *
 * 预期输出:
 *   ===== 设备: rxe_0 =====
 *   端口 1: link_layer=ETHERNET (RoCE 模式)
 *     GID[0] = fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx  [link-local]
 *     GID[1] = 0000:0000:0000:0000:0000:ffff:0a00:0001  [IPv4-mapped] ← 推荐
 *   ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

/* 引入公共工具库 */
#include "rdma_utils.h"

/* ========== GID 分析辅助函数 ========== */

/**
 * gid_type_str - 分析 GID 类型并返回描述字符串
 *
 * GID 类型判断规则：
 *   - 前缀 fe80:: → link-local (通常对应 RoCE v1)
 *   - 前缀 ::ffff:x.x.x.x → IPv4-mapped IPv6 (对应 RoCE v2)
 *   - 全零 → 空/无效
 *   - 其他 → 常规 IPv6 全局地址
 */
static const char *gid_type_str(const union ibv_gid *gid)
{
    /* 检查是否全零 */
    int all_zero = 1;              /* 是否全零标志 */
    for (int i = 0; i < 16; i++) {
        if (gid->raw[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero)
        return "空 (全零)";

    /* 检查 link-local 前缀: fe80:0000:0000:0000:... */
    if (gid->raw[0] == 0xfe && gid->raw[1] == 0x80 &&
        gid->raw[2] == 0x00 && gid->raw[3] == 0x00 &&
        gid->raw[4] == 0x00 && gid->raw[5] == 0x00 &&
        gid->raw[6] == 0x00 && gid->raw[7] == 0x00)
        return "link-local (RoCE v1)";

    /* 检查 IPv4-mapped 前缀: ::ffff:x.x.x.x */
    /* 即前 10 字节全零，第 11-12 字节为 0xffff */
    int prefix_zero = 1;          /* 前 10 字节是否全零 */
    for (int i = 0; i < 10; i++) {
        if (gid->raw[i] != 0) {
            prefix_zero = 0;
            break;
        }
    }
    if (prefix_zero && gid->raw[10] == 0xff && gid->raw[11] == 0xff)
        return "IPv4-mapped (RoCE v2) ★推荐";

    /* 其他 IPv6 全局地址 */
    return "IPv6 全局地址";
}

/**
 * print_gid_ipv4 - 如果 GID 是 IPv4-mapped，提取并打印 IPv4 地址
 */
static void print_gid_ipv4(const union ibv_gid *gid)
{
    /* 检查是否 IPv4-mapped */
    int prefix_zero = 1;
    for (int i = 0; i < 10; i++) {
        if (gid->raw[i] != 0) {
            prefix_zero = 0;
            break;
        }
    }

    if (prefix_zero && gid->raw[10] == 0xff && gid->raw[11] == 0xff) {
        /* 提取后 4 字节作为 IPv4 地址 */
        printf("  (IPv4: %d.%d.%d.%d)",
               gid->raw[12], gid->raw[13],
               gid->raw[14], gid->raw[15]);
    }
}

/**
 * print_ah_attr_guide - 打印 IB 和 RoCE 模式下 ah_attr 设置的差异说明
 */
static void print_ah_attr_guide(void)
{
    printf("\n");
    printf("================================================================\n");
    printf("  ah_attr 寻址差异 (IB vs RoCE)\n");
    printf("================================================================\n");
    printf("\n");
    printf("  InfiniBand (使用 LID 寻址):\n");
    printf("  ┌─────────────────────────────────────────┐\n");
    printf("  │ ah_attr.dlid      = remote->lid;        │  目的端口 LID\n");
    printf("  │ ah_attr.sl        = 0;                  │  Service Level\n");
    printf("  │ ah_attr.port_num  = port;               │  本地端口号\n");
    printf("  │ ah_attr.is_global = 0;                  │  不需要 GRH\n");
    printf("  └─────────────────────────────────────────┘\n");
    printf("\n");
    printf("  RoCE (使用 GID 寻址，必须设置 GRH):\n");
    printf("  ┌─────────────────────────────────────────────────┐\n");
    printf("  │ ah_attr.dlid           = 0;                     │  RoCE 不用 LID\n");
    printf("  │ ah_attr.port_num       = port;                  │  本地端口号\n");
    printf("  │ ah_attr.is_global      = 1;                     │  必须为 1!\n");
    printf("  │ ah_attr.grh.dgid       = remote->gid;           │  目的 GID\n");
    printf("  │ ah_attr.grh.sgid_index = local_gid_index;       │  本地 GID 索引\n");
    printf("  │ ah_attr.grh.hop_limit  = 64;                    │  TTL\n");
    printf("  └─────────────────────────────────────────────────┘\n");
    printf("\n");
    printf("  关键区别:\n");
    printf("    - IB:   dlid 字段有效, is_global=0\n");
    printf("    - RoCE: dlid 无意义, is_global 必须=1, grh.dgid 携带目标地址\n");
    printf("    - RoCE v2 推荐使用 IPv4-mapped GID (类型标记为★推荐)\n");
    printf("\n");
}

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* 设备列表 */
    int num_devices = 0;                    /* 设备数量 */
    int ret = 0;                            /* 返回值 */
    int recommended_gid_found __attribute__((unused)) = 0; /* 是否找到推荐的 GID */

    printf("==============================================\n");
    printf("  RoCE GID 表枚举与分析工具\n");
    printf("  自动检测 IB/RoCE，分析 GID 类型\n");
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

    printf("\n共发现 %d 个 RDMA 设备\n", num_devices);

    /* 第二步：遍历每个设备 */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;             /* 设备上下文 */
        struct ibv_device_attr dev_attr;            /* 设备属性 */

        printf("\n===== 设备 %d: %s =====\n", i, ibv_get_device_name(dev));

        /* 打开设备 */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "[错误] 无法打开设备 %s: %s\n",
                    ibv_get_device_name(dev), strerror(errno));
            continue;
        }

        /* 查询设备属性 (获取端口数量) */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "[错误] ibv_query_device() 失败\n");
            ibv_close_device(ctx);
            continue;
        }

        /* 第三步：遍历每个端口 */
        for (uint8_t port = 1; port <= dev_attr.phys_port_cnt; port++) {
            struct ibv_port_attr port_attr;         /* 端口属性 */

            memset(&port_attr, 0, sizeof(port_attr));
            ret = ibv_query_port(ctx, port, &port_attr);
            if (ret) {
                fprintf(stderr, "[错误] ibv_query_port(port=%u) 失败\n", port);
                continue;
            }

            /* 判断链路层类型 */
            int is_roce = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);

            printf("\n  端口 %u: link_layer=%s (%s 模式)\n",
                   port,
                   is_roce ? "ETHERNET" : "INFINIBAND",
                   is_roce ? "RoCE" : "IB");

            if (!is_roce) {
                /* IB 模式：显示 LID 信息 */
                printf("    LID = %u (0x%04x)\n", port_attr.lid, port_attr.lid);
                printf("    SM_LID = %u\n", port_attr.sm_lid);
                printf("    (IB 模式使用 LID 寻址，GID 表仅在跨子网时使用)\n");
            }

            /* 第四步：枚举 GID 表 */
            printf("    GID 表 (共 %d 个条目):\n", port_attr.gid_tbl_len);

            int valid_count = 0;               /* 有效 GID 计数 */
            int recommended_index = -1;        /* 推荐的 GID 索引 */

            for (int gid_idx = 0; gid_idx < port_attr.gid_tbl_len; gid_idx++) {
                union ibv_gid gid;              /* GID 值 */

                /* 调用 ibv_query_gid() 获取指定索引的 GID */
                ret = ibv_query_gid(ctx, port, gid_idx, &gid);
                if (ret) {
                    /* 查询失败，跳过 */
                    continue;
                }

                /* 检查是否全零 (无效条目) */
                int all_zero = 1;
                for (int b = 0; b < 16; b++) {
                    if (gid.raw[b] != 0) {
                        all_zero = 0;
                        break;
                    }
                }
                if (all_zero)
                    continue;   /* 跳过全零 GID */

                valid_count++;

                /* 格式化打印 GID */
                char gid_str[46];              /* GID 字符串缓冲区 */
                gid_to_str(&gid, gid_str, sizeof(gid_str));

                const char *type = gid_type_str(&gid);
                printf("      GID[%2d] = %s  [%s]", gid_idx, gid_str, type);

                /* 如果是 IPv4-mapped，显示对应的 IPv4 地址 */
                print_gid_ipv4(&gid);

                printf("\n");

                /* 记录第一个 IPv4-mapped GID 作为推荐索引 */
                if (recommended_index < 0) {
                    int pz = 1;
                    for (int b = 0; b < 10; b++) {
                        if (gid.raw[b] != 0) { pz = 0; break; }
                    }
                    if (pz && gid.raw[10] == 0xff && gid.raw[11] == 0xff) {
                        recommended_index = gid_idx;
                        recommended_gid_found = 1;
                    }
                }
            }

            /* 总结 */
            printf("\n    有效 GID 条目: %d / %d\n", valid_count, port_attr.gid_tbl_len);

            if (is_roce && recommended_index >= 0) {
                printf("    ★ RoCE v2 推荐 GID 索引: %d (IPv4-mapped)\n",
                       recommended_index);
                printf("      使用方法: gid_index = %d\n", recommended_index);
            } else if (is_roce) {
                printf("    ⚠ 未找到 IPv4-mapped GID，请确认网卡已配置 IP 地址\n");
            }
        }

        /* 关闭设备 */
        ibv_close_device(ctx);
    }

    /* 打印 ah_attr 差异指南 */
    print_ah_attr_guide();

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    printf("查询完成。\n");
    return (ret < 0) ? 1 : 0;
}
