/**
 * Multi-Device Enumeration Demo
 *
 * This program demonstrates how to enumerate all RDMA devices in the system
 * and query the complete topology:
 *   1. Use ibv_get_device_list() to get all devices
 *   2. For each device: open, query device attributes (ibv_query_device)
 *   3. For each port: query port attributes (ibv_query_port), print all GIDs
 *   4. Display complete topology: device name, transport type, port state
 *
 * Compile: gcc -o 02_multi_device 02_multi_device.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/**
 * Helper function: print the device node_type
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
 * Helper function: print the device transport_type
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
    printf("  RDMA Multi-Device Enumeration and Topology Query\n");
    printf("============================================\n\n");

    /* ========== Step 1: Get device list ========== */
    printf("[Step 1] Get RDMA device list\n");
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "[Error] ibv_get_device_list failed: %s\n", strerror(errno));
        return 1;
    }

    printf("  Found %d RDMA device(s)\n\n", num_devices);

    if (num_devices == 0) {
        printf("  No RDMA devices available. Please check:\n");
        printf("    - Whether RDMA drivers are installed (mlx4_core, mlx5_core, rxe...)\n");
        printf("    - Whether ib_uverbs kernel module is loaded\n");
        printf("    - lsmod | grep ib_\n");
        printf("    - ibstat or ibv_devinfo\n");
        goto cleanup;
    }

    /* ========== Step 2: Iterate over each device ========== */
    for (i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];
        struct ibv_context *ctx = NULL;
        struct ibv_device_attr dev_attr;

        printf("########################################\n");
        printf("# Device [%d/%d]: %s\n", i + 1, num_devices,
               ibv_get_device_name(dev));
        printf("########################################\n\n");

        /* Print basic device info (no need to open device) */
        printf("  Device name:    %s\n", ibv_get_device_name(dev));
        printf("  GUID:           0x%016lx\n", (unsigned long)ibv_get_device_guid(dev));
        printf("  Node type:      %s\n", node_type_str(dev->node_type));
        printf("  Transport type: %s\n", transport_type_str(dev->transport_type));
        printf("\n");

        /* Open device */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "  [Warning] Cannot open device %s: %s\n",
                    ibv_get_device_name(dev), strerror(errno));
            printf("  Skipping this device\n\n");
            continue;
        }

        /* Use utility library to query and print device attributes (~20 key fields) */
        printf("  --- Device Capability Parameters ---\n");
        query_and_print_device(ctx);
        printf("\n");

        /* Query device attributes to get port count */
        if (ibv_query_device(ctx, &dev_attr) != 0) {
            fprintf(stderr, "  [Warning] ibv_query_device failed\n");
            ibv_close_device(ctx);
            continue;
        }

        /* Iterate over each port */
        printf("  === Port Information (%d port(s) total) ===\n\n", dev_attr.phys_port_cnt);

        int port;
        for (port = 1; port <= dev_attr.phys_port_cnt; port++) {
            printf("  ---- Port %d ----\n", port);

            /* Use utility library to query and print port attributes */
            query_and_print_port(ctx, port);

            /* Detect transport layer type */
            enum rdma_transport tp = detect_transport(ctx, port);
            printf("  Detected transport layer: %s\n", transport_str(tp));

            /* Print non-zero GID entries */
            query_and_print_all_gids(ctx, port);

            printf("\n");
        }

        /* Close device */
        ibv_close_device(ctx);
    }

    /* ========== Summary ========== */
    printf("============================================\n");
    printf("  Topology Enumeration Summary\n");
    printf("============================================\n");
    printf("  Found %d RDMA device(s) in total\n\n", num_devices);
    printf("  API call chain:\n");
    printf("    ibv_get_device_list()  -> Get device list\n");
    printf("    ibv_get_device_name()  -> Device name (e.g. mlx5_0)\n");
    printf("    ibv_get_device_guid()  -> Device GUID\n");
    printf("    ibv_open_device()      -> Get context\n");
    printf("    ibv_query_device()     -> Device capabilities (max_qp, max_mr, ...)\n");
    printf("    ibv_query_port()       -> Port state (state, LID, MTU, ...)\n");
    printf("    ibv_query_gid()        -> GID table (needed for RoCE addressing)\n");
    printf("    ibv_close_device()     -> Close device\n");
    printf("    ibv_free_device_list() -> Free list\n\n");

cleanup:
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    printf("Program finished\n");
    return 0;
}
