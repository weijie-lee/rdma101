/**
 * iwarp_query.c - iWARP Device Query Program
 *
 * Features:
 *   - Query all RDMA devices
 *   - Distinguish IB/RoCE (IBV_NODE_CA) and iWARP (IBV_NODE_RNIC) via node_type field
 *   - Print transport type information for each device
 *   - Explain iWARP specifics
 *
 * Compile:
 *   gcc -o iwarp_query iwarp_query.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * Expected Output:
 *   Device: rxe_0
 *     node_type   = IBV_NODE_CA (1) -- Channel Adapter (IB or RoCE)
 *     link_layer  = Ethernet
 *     Transport   = RoCE
 *   (If iWARP device present:)
 *   Device: cxgb4_0
 *     node_type   = IBV_NODE_RNIC (4) -- RDMA NIC (iWARP)
 *     link_layer  = Ethernet
 *     Transport   = iWARP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>

/* Include common utility library */
#include "rdma_utils.h"

/* ========== Helper Functions ========== */

/**
 * node_type_str - Convert node_type enum to description string
 *
 * node_type is the key field to distinguish IB/RoCE from iWARP:
 *   - IBV_NODE_CA (1): Channel Adapter, used for InfiniBand and RoCE
 *   - IBV_NODE_SWITCH (2): IB switch
 *   - IBV_NODE_ROUTER (3): IB router
 *   - IBV_NODE_RNIC (4): RDMA NIC, specifically for iWARP
 */
static const char *node_type_str(enum ibv_node_type type)
{
    switch (type) {
    case IBV_NODE_CA:
        return "IBV_NODE_CA -- Channel Adapter (IB or RoCE)";
    case IBV_NODE_SWITCH:
        return "IBV_NODE_SWITCH -- InfiniBand Switch";
    case IBV_NODE_ROUTER:
        return "IBV_NODE_ROUTER -- InfiniBand Router";
    case IBV_NODE_RNIC:
        return "IBV_NODE_RNIC -- RDMA NIC (iWARP dedicated)";
    default:
        return "UNKNOWN -- Unknown type";
    }
}

/**
 * link_layer_name - Convert link_layer to string
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

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* Device list */
    int num_devices = 0;                    /* Number of devices */
    int ret = 0;                            /* Return value */
    int ib_count = 0;                       /* IB device count */
    int roce_count = 0;                     /* RoCE device count */
    int iwarp_count = 0;                    /* iWARP device count */

    printf("==============================================\n");
    printf("  iWARP Device Query Tool\n");
    printf("  Distinguish IB/RoCE/iWARP via node_type\n");
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

    printf("\nFound %d RDMA device(s)\n\n", num_devices);

    /* Step 2: Iterate over each device, print node_type info */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;             /* Device context */
        struct ibv_device_attr dev_attr;            /* Device attributes */
        struct ibv_port_attr port_attr;             /* Port attributes */

        printf("---------------------------------------\n");
        printf("Device %d: %s\n", i, ibv_get_device_name(dev));
        printf("---------------------------------------\n");

        /* Read node_type (can be obtained without opening device) */
        enum ibv_node_type ntype = dev->node_type;
        printf("  node_type   = %s (%d)\n", node_type_str(ntype), ntype);

        /* Read GUID */
        uint64_t guid = ibv_get_device_guid(dev);
        printf("  node_guid   = %016llx\n", (unsigned long long)guid);

        /* Open device to query more information */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "  [Error] Cannot open device: %s\n", strerror(errno));
            continue;
        }

        /* Query device attributes */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "  [Error] ibv_query_device() failed\n");
            ibv_close_device(ctx);
            continue;
        }

        printf("  FW version  = %s\n", dev_attr.fw_ver);
        printf("  Port count  = %d\n", dev_attr.phys_port_cnt);

        /* Query link layer type of the first port */
        memset(&port_attr, 0, sizeof(port_attr));
        ret = ibv_query_port(ctx, 1, &port_attr);
        if (ret == 0) {
            printf("  link_layer  = %s\n",
                   link_layer_name(port_attr.link_layer));
        }

        /* Use common library's detect_transport() for comprehensive detection */
        enum rdma_transport transport = detect_transport(ctx, 1);
        printf("  Transport   = %s\n", transport_str(transport));

        /* Statistics */
        switch (ntype) {
        case IBV_NODE_RNIC:
            iwarp_count++;                  /* iWARP device */
            break;
        case IBV_NODE_CA:
            if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)
                roce_count++;               /* RoCE device */
            else
                ib_count++;                 /* IB device */
            break;
        default:
            break;
        }

        printf("\n");
        ibv_close_device(ctx);
    }

    /* Step 3: Print summary and iWARP explanation */
    printf("==============================================\n");
    printf("  Device Statistics\n");
    printf("==============================================\n");
    printf("  InfiniBand devices: %d\n", ib_count);
    printf("  RoCE devices:       %d\n", roce_count);
    printf("  iWARP devices:      %d\n", iwarp_count);
    printf("\n");

    /* Print iWARP specific notes */
    printf("==============================================\n");
    printf("  iWARP Programming Key Points\n");
    printf("==============================================\n");
    printf("\n");
    printf("  1. Identification method:\n");
    printf("     - node_type == IBV_NODE_RNIC (value %d)\n", IBV_NODE_RNIC);
    printf("     - IB/RoCE node_type is IBV_NODE_CA (value %d)\n",
           IBV_NODE_CA);
    printf("\n");
    printf("  2. Connection establishment:\n");
    printf("     - iWARP uses TCP underneath, recommend using RDMA CM for connections\n");
    printf("     - rdma_resolve_addr() -> rdma_resolve_route() -> rdma_connect()\n");
    printf("\n");
    printf("  3. Key differences from IB/RoCE:\n");
    printf("     - No lossless network required (TCP has built-in retransmission)\n");
    printf("     - Can traverse NAT/firewalls\n");
    printf("     - Slightly lower performance than IB/RoCE (TCP processing overhead)\n");
    printf("     - QP type typically only supports RC (UC/UD not supported)\n");
    printf("\n");

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    return (ret < 0) ? 1 : 0;
}
