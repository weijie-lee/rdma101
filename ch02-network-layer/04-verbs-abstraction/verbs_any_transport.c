/**
 * verbs_any_transport.c - Verbs Unified Abstraction Layer Demo Program
 *
 * Features:
 *   - Demonstrate that the same ibv_get_device_list / ibv_open_device code works on any transport type
 *   - Open each device, query device attributes and port attributes
 *   - Print the transport type (IB / RoCE / iWARP) for each device
 *   - Display /dev/infiniband/ device files
 *   - Show how Verbs provides a unified abstraction for three transports
 *   - Extensively uses query and print functions from common/rdma_utils.h
 *
 * Compile:
 *   gcc -o verbs_any_transport verbs_any_transport.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * Expected Output:
 *   ===== Unified Verbs API Demo =====
 *   The following code is fully generic for IB / RoCE / iWARP
 *
 *   === Device 0: rxe_0 ===
 *   Transport Type: RoCE
 *   [Device Attributes]
 *   ...
 *   [Port Attributes]
 *   ...
 *   [GID Table]
 *   ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>       /* For reading /dev/infiniband/ directory */
#include <sys/stat.h>     /* For stat() to check file type */
#include <infiniband/verbs.h>

/* Include common utility library -- provides device/port/GID query and print functions */
#include "rdma_utils.h"

/* ========== Helper Functions ========== */

/**
 * show_dev_files - Display device files under /dev/infiniband/
 *
 * Verbs interacts with the kernel through these character device files:
 *   - uverbs0, uverbs1, ... : One per RDMA device
 *   - rdma_cm : RDMA CM character device
 */
static void show_dev_files(void)
{
    const char *dev_path = "/dev/infiniband";   /* Device file directory */
    DIR *dir = NULL;                            /* Directory handle */
    struct dirent *entry = NULL;                /* Directory entry */

    printf("\n");
    printf("============================================================\n");
    printf("  /dev/infiniband/ Device Files\n");
    printf("============================================================\n");

    dir = opendir(dev_path);
    if (!dir) {
        printf("  Cannot open %s: %s\n", dev_path, strerror(errno));
        printf("  Hint: Please verify RDMA drivers are loaded\n");
        return;
    }

    int count = 0;                              /* File count */
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.')
            continue;

        /* Build full path */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dev_path, entry->d_name);

        /* Get file information */
        struct stat st;
        if (stat(full_path, &st) == 0) {
            const char *type_str;               /* File type description */
            if (S_ISCHR(st.st_mode))
                type_str = "char device";
            else if (S_ISDIR(st.st_mode))
                type_str = "directory";
            else
                type_str = "file";

            printf("  %-20s  [%s]", entry->d_name, type_str);

            /* Explain the purpose of each device file */
            if (strncmp(entry->d_name, "uverbs", 6) == 0)
                printf("  -- Verbs userspace interface");
            else if (strcmp(entry->d_name, "rdma_cm") == 0)
                printf("  -- RDMA CM connection management");
            else if (strncmp(entry->d_name, "ucm", 3) == 0)
                printf("  -- IB CM userspace interface");
            else if (strncmp(entry->d_name, "umad", 4) == 0)
                printf("  -- IB MAD userspace interface");
            else if (strncmp(entry->d_name, "issm", 4) == 0)
                printf("  -- IB SM interface");

            printf("\n");
            count++;
        }
    }

    closedir(dir);

    if (count == 0)
        printf("  (directory is empty)\n");
    else
        printf("  Total %d device file(s)\n", count);
}

/**
 * show_sysfs_info - Display sysfs info under /sys/class/infiniband/
 */
static void show_sysfs_info(void)
{
    const char *sysfs_path = "/sys/class/infiniband";
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    printf("\n");
    printf("============================================================\n");
    printf("  /sys/class/infiniband/ sysfs Information\n");
    printf("============================================================\n");

    dir = opendir(sysfs_path);
    if (!dir) {
        printf("  Cannot open %s: %s\n", sysfs_path, strerror(errno));
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        printf("  %-20s", entry->d_name);

        /* Read node_type */
        char path[512];
        char buf[64];
        FILE *fp;

        snprintf(path, sizeof(path), "%s/%s/node_type",
                 sysfs_path, entry->d_name);
        fp = fopen(path, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                /* Remove newline character */
                buf[strcspn(buf, "\n")] = '\0';
                printf("  node_type=%s", buf);
            }
            fclose(fp);
        }

        printf("\n");
    }

    closedir(dir);
}

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* Device list */
    int num_devices = 0;                    /* Number of devices */
    int ret = 0;                            /* Return value */

    printf("============================================================\n");
    printf("  Verbs Unified Abstraction Layer Demo\n");
    printf("  Same code supports InfiniBand / RoCE / iWARP\n");
    printf("============================================================\n");
    printf("\n");
    printf("  Key Insight:\n");
    printf("  ibv_get_device_list() / ibv_open_device() / ibv_query_*\n");
    printf("  These APIs are exactly the same across all transport types!\n");
    printf("  The only difference is the addressing in ah_attr setup.\n");
    printf("\n");

    /* ===== Step 1: Unified device enumeration (common to all transport types) ===== */
    printf("------------------------------------------------------------\n");
    printf("  Step 1: ibv_get_device_list() enumerates all devices\n");
    printf("  (This API is identical for IB/RoCE/iWARP)\n");
    printf("------------------------------------------------------------\n");

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "[Error] ibv_get_device_list() failed: %s\n",
                strerror(errno));
        return 1;
    }

    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        ret = 1;
        goto cleanup;
    }

    printf("  Found %d RDMA device(s)\n\n", num_devices);

    /* ===== Step 2: Iterate over each device, unified query ===== */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;             /* Device context */
        struct ibv_device_attr dev_attr;            /* Device attributes */

        printf("============================================================\n");
        printf("  Device %d: %s\n", i, ibv_get_device_name(dev));
        printf("============================================================\n");

        /* Step 2-A: ibv_open_device() (common to all transport types) */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "  [Error] ibv_open_device() failed: %s\n",
                    strerror(errno));
            continue;
        }

        /* Detect transport type */
        enum rdma_transport transport = detect_transport(ctx, 1);
        printf("\n  Transport Type: %s\n", transport_str(transport));

        /* Step 2-B: Use common library to print device attributes (common to all transport types) */
        printf("\n  [Device Attributes] (ibv_query_device - common to all transports)\n");
        query_and_print_device(ctx);

        /* Step 2-C: Query port count */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "  [Error] ibv_query_device() failed\n");
            ibv_close_device(ctx);
            continue;
        }

        /* Step 2-D: Use common library to print port attributes (common to all transport types) */
        for (uint8_t port = 1; port <= dev_attr.phys_port_cnt; port++) {
            printf("\n  [Port %u Attributes] (ibv_query_port - common to all transports)\n", port);
            query_and_print_port(ctx, port);

            /* Step 2-E: Use common library to print GID table (common to all transport types) */
            printf("\n  [Port %u GID Table] (ibv_query_gid - common to all transports)\n", port);
            query_and_print_all_gids(ctx, port);

            /* Step 2-F: Give addressing advice based on transport type */
            struct ibv_port_attr port_attr;
            memset(&port_attr, 0, sizeof(port_attr));
            ibv_query_port(ctx, port, &port_attr);

            printf("\n  [Addressing Recommendation]\n");
            if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
                /* RoCE or iWARP: Use GID addressing */
                printf("    This port is Ethernet link -> Use GID addressing\n");
                printf("    ah_attr.is_global  = 1\n");
                printf("    ah_attr.grh.dgid   = remote_gid\n");
                printf("    ah_attr.grh.sgid_index = <choose RoCE v2 GID index>\n");
            } else if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND) {
                /* IB: Use LID addressing */
                printf("    This port is InfiniBand link -> Use LID addressing\n");
                printf("    ah_attr.dlid       = remote_lid (current port LID=%u)\n",
                       port_attr.lid);
                printf("    ah_attr.is_global  = 0 (within subnet)\n");
            } else {
                printf("    Link layer type unknown, please check driver\n");
            }
        }

        /* Close device (common to all transport types) */
        ibv_close_device(ctx);
        printf("\n");
    }

    /* ===== Step 3: Display device files and sysfs ===== */
    show_dev_files();
    show_sysfs_info();

    /* ===== Summary ===== */
    printf("\n");
    printf("============================================================\n");
    printf("  Summary: Verbs Unified Abstraction\n");
    printf("============================================================\n");
    printf("\n");
    printf("  Same API, Three Transports:\n");
    printf("  +-------------------------------------------------+\n");
    printf("  | API                     | IB | RoCE | iWARP     |\n");
    printf("  +-------------------------+----+------+-----------+\n");
    printf("  | ibv_get_device_list()   |  Y |   Y  |     Y     |\n");
    printf("  | ibv_open_device()       |  Y |   Y  |     Y     |\n");
    printf("  | ibv_query_device()      |  Y |   Y  |     Y     |\n");
    printf("  | ibv_query_port()        |  Y |   Y  |     Y     |\n");
    printf("  | ibv_query_gid()         |  Y |   Y  |     Y     |\n");
    printf("  | ibv_alloc_pd()          |  Y |   Y  |     Y     |\n");
    printf("  | ibv_reg_mr()            |  Y |   Y  |     Y     |\n");
    printf("  | ibv_create_cq()         |  Y |   Y  |     Y     |\n");
    printf("  | ibv_create_qp()         |  Y |   Y  |     Y     |\n");
    printf("  | ibv_post_send()         |  Y |   Y  |     Y     |\n");
    printf("  | ibv_post_recv()         |  Y |   Y  |     Y     |\n");
    printf("  | ibv_poll_cq()           |  Y |   Y  |     Y     |\n");
    printf("  +--------------------------+----+------+-----------+\n");
    printf("\n");
    printf("  The only difference: ah_attr setup in ibv_modify_qp()\n");
    printf("    - IB:    ah_attr.dlid = remote_lid, is_global = 0\n");
    printf("    - RoCE:  ah_attr.grh.dgid = remote_gid, is_global = 1\n");
    printf("    - iWARP: Typically uses RDMA CM, does not set ah_attr directly\n");
    printf("\n");

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    return (ret < 0) ? 1 : 0;
}
