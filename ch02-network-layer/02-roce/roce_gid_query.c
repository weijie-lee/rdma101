/**
 * roce_gid_query.c - RoCE GID Table Enumeration and Analysis Program
 *
 * Features:
 *   - Enumerate all GID entries for all ports of all devices
 *   - Automatically detect IB vs RoCE (link_layer == ETHERNET -> RoCE)
 *   - Format and print each GID
 *   - Analyze GID type (link-local / IPv4-mapped / IPv6)
 *   - Recommend GID index to use for RoCE v2
 *   - Explain ah_attr differences between IB and RoCE
 *
 * Compile:
 *   gcc -o roce_gid_query roce_gid_query.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * Or use Makefile:
 *   make
 *
 * Expected Output:
 *   ===== Device: rxe_0 =====
 *   Port 1: link_layer=ETHERNET (RoCE mode)
 *     GID[0] = fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx  [link-local]
 *     GID[1] = 0000:0000:0000:0000:0000:ffff:0a00:0001  [IPv4-mapped] <- Recommended
 *   ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

/* Include common utility library */
#include "rdma_utils.h"

/* ========== GID Analysis Helper Functions ========== */

/**
 * gid_type_str - Analyze GID type and return description string
 *
 * GID type detection rules:
 *   - Prefix fe80:: -> link-local (typically corresponds to RoCE v1)
 *   - Prefix ::ffff:x.x.x.x -> IPv4-mapped IPv6 (corresponds to RoCE v2)
 *   - All zeros -> empty/invalid
 *   - Other -> regular IPv6 global address
 */
static const char *gid_type_str(const union ibv_gid *gid)
{
    /* Check if all zeros */
    int all_zero = 1;              /* All-zero flag */
    for (int i = 0; i < 16; i++) {
        if (gid->raw[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero)
        return "Empty (all zeros)";

    /* Check link-local prefix: fe80:0000:0000:0000:... */
    if (gid->raw[0] == 0xfe && gid->raw[1] == 0x80 &&
        gid->raw[2] == 0x00 && gid->raw[3] == 0x00 &&
        gid->raw[4] == 0x00 && gid->raw[5] == 0x00 &&
        gid->raw[6] == 0x00 && gid->raw[7] == 0x00)
        return "link-local (RoCE v1)";

    /* Check IPv4-mapped prefix: ::ffff:x.x.x.x */
    /* i.e. first 10 bytes all zeros, bytes 11-12 are 0xffff */
    int prefix_zero = 1;          /* Whether first 10 bytes are all zeros */
    for (int i = 0; i < 10; i++) {
        if (gid->raw[i] != 0) {
            prefix_zero = 0;
            break;
        }
    }
    if (prefix_zero && gid->raw[10] == 0xff && gid->raw[11] == 0xff)
        return "IPv4-mapped (RoCE v2) *Recommended";

    /* Other IPv6 global address */
    return "IPv6 global address";
}

/**
 * print_gid_ipv4 - If GID is IPv4-mapped, extract and print the IPv4 address
 */
static void print_gid_ipv4(const union ibv_gid *gid)
{
    /* Check if IPv4-mapped */
    int prefix_zero = 1;
    for (int i = 0; i < 10; i++) {
        if (gid->raw[i] != 0) {
            prefix_zero = 0;
            break;
        }
    }

    if (prefix_zero && gid->raw[10] == 0xff && gid->raw[11] == 0xff) {
        /* Extract last 4 bytes as IPv4 address */
        printf("  (IPv4: %d.%d.%d.%d)",
               gid->raw[12], gid->raw[13],
               gid->raw[14], gid->raw[15]);
    }
}

/**
 * print_ah_attr_guide - Print ah_attr setup difference guide for IB and RoCE modes
 */
static void print_ah_attr_guide(void)
{
    printf("\n");
    printf("================================================================\n");
    printf("  ah_attr Addressing Differences (IB vs RoCE)\n");
    printf("================================================================\n");
    printf("\n");
    printf("  InfiniBand (uses LID addressing):\n");
    printf("  +---------------------------------------------+\n");
    printf("  | ah_attr.dlid      = remote->lid;        |  Destination port LID\n");
    printf("  | ah_attr.sl        = 0;                  |  Service Level\n");
    printf("  | ah_attr.port_num  = port;               |  Local port number\n");
    printf("  | ah_attr.is_global = 0;                  |  GRH not needed\n");
    printf("  +---------------------------------------------+\n");
    printf("\n");
    printf("  RoCE (uses GID addressing, must set GRH):\n");
    printf("  +-------------------------------------------------+\n");
    printf("  | ah_attr.dlid           = 0;                     |  RoCE does not use LID\n");
    printf("  | ah_attr.port_num       = port;                  |  Local port number\n");
    printf("  | ah_attr.is_global      = 1;                     |  Must be 1!\n");
    printf("  | ah_attr.grh.dgid       = remote->gid;           |  Destination GID\n");
    printf("  | ah_attr.grh.sgid_index = local_gid_index;       |  Local GID index\n");
    printf("  | ah_attr.grh.hop_limit  = 64;                    |  TTL\n");
    printf("  +-------------------------------------------------+\n");
    printf("\n");
    printf("  Key Differences:\n");
    printf("    - IB:   dlid field is valid, is_global=0\n");
    printf("    - RoCE: dlid is meaningless, is_global must=1, grh.dgid carries target address\n");
    printf("    - RoCE v2 recommends using IPv4-mapped GID (marked as *Recommended)\n");
    printf("\n");
}

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* Device list */
    int num_devices = 0;                    /* Number of devices */
    int ret = 0;                            /* Return value */
    int recommended_gid_found __attribute__((unused)) = 0; /* Whether recommended GID was found */

    printf("==============================================\n");
    printf("  RoCE GID Table Enumeration and Analysis Tool\n");
    printf("  Auto-detect IB/RoCE, analyze GID types\n");
    printf("==============================================\n");

    /* Step 1: Get device list */
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

    printf("\nFound %d RDMA device(s)\n", num_devices);

    /* Step 2: Iterate over each device */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;             /* Device context */
        struct ibv_device_attr dev_attr;            /* Device attributes */

        printf("\n===== Device %d: %s =====\n", i, ibv_get_device_name(dev));

        /* Open device */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "[Error] Cannot open device %s: %s\n",
                    ibv_get_device_name(dev), strerror(errno));
            continue;
        }

        /* Query device attributes (get port count) */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "[Error] ibv_query_device() failed\n");
            ibv_close_device(ctx);
            continue;
        }

        /* Step 3: Iterate over each port */
        for (uint8_t port = 1; port <= dev_attr.phys_port_cnt; port++) {
            struct ibv_port_attr port_attr;         /* Port attributes */

            memset(&port_attr, 0, sizeof(port_attr));
            ret = ibv_query_port(ctx, port, &port_attr);
            if (ret) {
                fprintf(stderr, "[Error] ibv_query_port(port=%u) failed\n", port);
                continue;
            }

            /* Determine link layer type */
            int is_roce = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);

            printf("\n  Port %u: link_layer=%s (%s mode)\n",
                   port,
                   is_roce ? "ETHERNET" : "INFINIBAND",
                   is_roce ? "RoCE" : "IB");

            if (!is_roce) {
                /* IB mode: Display LID information */
                printf("    LID = %u (0x%04x)\n", port_attr.lid, port_attr.lid);
                printf("    SM_LID = %u\n", port_attr.sm_lid);
                printf("    (IB mode uses LID addressing, GID table is only used for cross-subnet)\n");
            }

            /* Step 4: Enumerate GID table */
            printf("    GID Table (%d entries total):\n", port_attr.gid_tbl_len);

            int valid_count = 0;               /* Valid GID count */
            int recommended_index = -1;        /* Recommended GID index */

            for (int gid_idx = 0; gid_idx < port_attr.gid_tbl_len; gid_idx++) {
                union ibv_gid gid;              /* GID value */

                /* Call ibv_query_gid() to get GID at specified index */
                ret = ibv_query_gid(ctx, port, gid_idx, &gid);
                if (ret) {
                    /* Query failed, skip */
                    continue;
                }

                /* Check if all zeros (invalid entry) */
                int all_zero = 1;
                for (int b = 0; b < 16; b++) {
                    if (gid.raw[b] != 0) {
                        all_zero = 0;
                        break;
                    }
                }
                if (all_zero)
                    continue;   /* Skip all-zero GID */

                valid_count++;

                /* Format and print GID */
                char gid_str[46];              /* GID string buffer */
                gid_to_str(&gid, gid_str, sizeof(gid_str));

                const char *type = gid_type_str(&gid);
                printf("      GID[%2d] = %s  [%s]", gid_idx, gid_str, type);

                /* If IPv4-mapped, show corresponding IPv4 address */
                print_gid_ipv4(&gid);

                printf("\n");

                /* Record first IPv4-mapped GID as recommended index */
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

            /* Summary */
            printf("\n    Valid GID entries: %d / %d\n", valid_count, port_attr.gid_tbl_len);

            if (is_roce && recommended_index >= 0) {
                printf("    * RoCE v2 recommended GID index: %d (IPv4-mapped)\n",
                       recommended_index);
                printf("      Usage: gid_index = %d\n", recommended_index);
            } else if (is_roce) {
                printf("    ! No IPv4-mapped GID found, please verify the NIC has an IP address configured\n");
            }
        }

        /* Close device */
        ibv_close_device(ctx);
    }

    /* Print ah_attr difference guide */
    print_ah_attr_guide();

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    printf("Query complete.\n");
    return (ret < 0) ? 1 : 0;
}
