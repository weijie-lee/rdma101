/**
 * verbs_any_transport.c - Verbs 统一抽象层演示程序
 *
 * 功能：
 *   - 演示同一套 ibv_get_device_list / ibv_open_device 代码在任何传输类型下都能工作
 *   - 打开每个设备，查询设备属性和端口属性
 *   - 打印每个设备的传输类型 (IB / RoCE / iWARP)
 *   - 显示 /dev/infiniband/ 设备文件
 *   - 展示 Verbs 如何统一抽象三种传输
 *   - 大量使用 common/rdma_utils.h 的查询打印函数
 *
 * 编译:
 *   gcc -o verbs_any_transport verbs_any_transport.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * 预期输出:
 *   ===== 统一 Verbs API 演示 =====
 *   下面的代码对 IB / RoCE / iWARP 完全通用
 *
 *   === 设备 0: rxe_0 ===
 *   传输类型: RoCE
 *   [设备属性]
 *   ...
 *   [端口属性]
 *   ...
 *   [GID 表]
 *   ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>       /* 用于读取 /dev/infiniband/ 目录 */
#include <sys/stat.h>     /* 用于 stat() 检查文件类型 */
#include <infiniband/verbs.h>

/* 引入公共工具库 —— 提供设备/端口/GID 查询和打印功能 */
#include "rdma_utils.h"

/* ========== 辅助函数 ========== */

/**
 * show_dev_files - 显示 /dev/infiniband/ 下的设备文件
 *
 * Verbs 通过这些字符设备文件与内核交互：
 *   - uverbs0, uverbs1, ... : 每个 RDMA 设备对应一个
 *   - rdma_cm : RDMA CM 字符设备
 */
static void show_dev_files(void)
{
    const char *dev_path = "/dev/infiniband";   /* 设备文件目录 */
    DIR *dir = NULL;                            /* 目录句柄 */
    struct dirent *entry = NULL;                /* 目录条目 */

    printf("\n");
    printf("============================================================\n");
    printf("  /dev/infiniband/ 设备文件\n");
    printf("============================================================\n");

    dir = opendir(dev_path);
    if (!dir) {
        printf("  无法打开 %s: %s\n", dev_path, strerror(errno));
        printf("  提示: 请确认 RDMA 驱动已加载\n");
        return;
    }

    int count = 0;                              /* 文件计数 */
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过 . 和 .. */
        if (entry->d_name[0] == '.')
            continue;

        /* 构建完整路径 */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dev_path, entry->d_name);

        /* 获取文件信息 */
        struct stat st;
        if (stat(full_path, &st) == 0) {
            const char *type_str;               /* 文件类型描述 */
            if (S_ISCHR(st.st_mode))
                type_str = "字符设备";
            else if (S_ISDIR(st.st_mode))
                type_str = "目录";
            else
                type_str = "文件";

            printf("  %-20s  [%s]", entry->d_name, type_str);

            /* 解释各设备文件的用途 */
            if (strncmp(entry->d_name, "uverbs", 6) == 0)
                printf("  -- Verbs 用户态接口");
            else if (strcmp(entry->d_name, "rdma_cm") == 0)
                printf("  -- RDMA CM 连接管理");
            else if (strncmp(entry->d_name, "ucm", 3) == 0)
                printf("  -- IB CM 用户态接口");
            else if (strncmp(entry->d_name, "umad", 4) == 0)
                printf("  -- IB MAD 用户态接口");
            else if (strncmp(entry->d_name, "issm", 4) == 0)
                printf("  -- IB SM 接口");

            printf("\n");
            count++;
        }
    }

    closedir(dir);

    if (count == 0)
        printf("  (目录为空)\n");
    else
        printf("  共 %d 个设备文件\n", count);
}

/**
 * show_sysfs_info - 显示 /sys/class/infiniband/ 下的 sysfs 信息
 */
static void show_sysfs_info(void)
{
    const char *sysfs_path = "/sys/class/infiniband";
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    printf("\n");
    printf("============================================================\n");
    printf("  /sys/class/infiniband/ sysfs 信息\n");
    printf("============================================================\n");

    dir = opendir(sysfs_path);
    if (!dir) {
        printf("  无法打开 %s: %s\n", sysfs_path, strerror(errno));
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        printf("  %-20s", entry->d_name);

        /* 读取 node_type */
        char path[512];
        char buf[64];
        FILE *fp;

        snprintf(path, sizeof(path), "%s/%s/node_type",
                 sysfs_path, entry->d_name);
        fp = fopen(path, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                /* 去掉换行符 */
                buf[strcspn(buf, "\n")] = '\0';
                printf("  node_type=%s", buf);
            }
            fclose(fp);
        }

        printf("\n");
    }

    closedir(dir);
}

/* ========== 主函数 ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* 设备列表 */
    int num_devices = 0;                    /* 设备数量 */
    int ret = 0;                            /* 返回值 */

    printf("============================================================\n");
    printf("  Verbs 统一抽象层演示\n");
    printf("  同一套代码支持 InfiniBand / RoCE / iWARP\n");
    printf("============================================================\n");
    printf("\n");
    printf("  关键观点:\n");
    printf("  ibv_get_device_list() / ibv_open_device() / ibv_query_*\n");
    printf("  这些 API 在所有传输类型上都完全相同!\n");
    printf("  唯一需要区分的是设置 ah_attr 时的寻址方式。\n");
    printf("\n");

    /* ===== 第一步：统一的设备枚举 (所有传输类型通用) ===== */
    printf("------------------------------------------------------------\n");
    printf("  第一步: ibv_get_device_list() 枚举所有设备\n");
    printf("  (此 API 对 IB/RoCE/iWARP 完全相同)\n");
    printf("------------------------------------------------------------\n");

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

    printf("  发现 %d 个 RDMA 设备\n\n", num_devices);

    /* ===== 第二步：遍历每个设备，统一查询 ===== */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;             /* 设备上下文 */
        struct ibv_device_attr dev_attr;            /* 设备属性 */

        printf("============================================================\n");
        printf("  设备 %d: %s\n", i, ibv_get_device_name(dev));
        printf("============================================================\n");

        /* 第二步-A: ibv_open_device() (所有传输类型通用) */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "  [错误] ibv_open_device() 失败: %s\n",
                    strerror(errno));
            continue;
        }

        /* 检测传输类型 */
        enum rdma_transport transport = detect_transport(ctx, 1);
        printf("\n  传输类型: %s\n", transport_str(transport));

        /* 第二步-B: 使用公共库打印设备属性 (所有传输类型通用) */
        printf("\n  [设备属性] (ibv_query_device - 所有传输通用)\n");
        query_and_print_device(ctx);

        /* 第二步-C: 查询端口数量 */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "  [错误] ibv_query_device() 失败\n");
            ibv_close_device(ctx);
            continue;
        }

        /* 第二步-D: 使用公共库打印每个端口的属性 (所有传输类型通用) */
        for (uint8_t port = 1; port <= dev_attr.phys_port_cnt; port++) {
            printf("\n  [端口 %u 属性] (ibv_query_port - 所有传输通用)\n", port);
            query_and_print_port(ctx, port);

            /* 第二步-E: 使用公共库打印 GID 表 (所有传输类型通用) */
            printf("\n  [端口 %u GID 表] (ibv_query_gid - 所有传输通用)\n", port);
            query_and_print_all_gids(ctx, port);

            /* 第二步-F: 根据传输类型给出寻址建议 */
            struct ibv_port_attr port_attr;
            memset(&port_attr, 0, sizeof(port_attr));
            ibv_query_port(ctx, port, &port_attr);

            printf("\n  [寻址方式建议]\n");
            if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                /* RoCE 或 iWARP: 使用 GID 寻址 */
                printf("    此端口为以太网链路 → 使用 GID 寻址\n");
                printf("    ah_attr.is_global  = 1\n");
                printf("    ah_attr.grh.dgid   = remote_gid\n");
                printf("    ah_attr.grh.sgid_index = <选择 RoCE v2 的 GID 索引>\n");
            } else if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
                /* IB: 使用 LID 寻址 */
                printf("    此端口为 InfiniBand 链路 → 使用 LID 寻址\n");
                printf("    ah_attr.dlid       = remote_lid (当前端口 LID=%u)\n",
                       port_attr.lid);
                printf("    ah_attr.is_global  = 0 (子网内)\n");
            } else {
                printf("    链路层类型未知，请检查驱动\n");
            }
        }

        /* 关闭设备 (所有传输类型通用) */
        ibv_close_device(ctx);
        printf("\n");
    }

    /* ===== 第三步：显示设备文件和 sysfs ===== */
    show_dev_files();
    show_sysfs_info();

    /* ===== 总结 ===== */
    printf("\n");
    printf("============================================================\n");
    printf("  总结: Verbs 统一抽象\n");
    printf("============================================================\n");
    printf("\n");
    printf("  相同的 API，三种传输:\n");
    printf("  ┌─────────────────────────────────────────────────┐\n");
    printf("  │ API                     │ IB │ RoCE │ iWARP     │\n");
    printf("  ├─────────────────────────┼────┼──────┼───────────┤\n");
    printf("  │ ibv_get_device_list()   │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_open_device()       │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_query_device()      │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_query_port()        │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_query_gid()         │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_alloc_pd()          │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_reg_mr()            │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_create_cq()         │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_create_qp()         │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_post_send()         │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_post_recv()         │ ✓  │  ✓   │    ✓      │\n");
    printf("  │ ibv_poll_cq()           │ ✓  │  ✓   │    ✓      │\n");
    printf("  └─────────────────────────┴────┴──────┴───────────┘\n");
    printf("\n");
    printf("  唯一的差异: ibv_modify_qp() 中 ah_attr 的设置\n");
    printf("    - IB:    ah_attr.dlid = remote_lid, is_global = 0\n");
    printf("    - RoCE:  ah_attr.grh.dgid = remote_gid, is_global = 1\n");
    printf("    - iWARP: 通常使用 RDMA CM, 不直接设置 ah_attr\n");
    printf("\n");

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    return (ret < 0) ? 1 : 0;
}
