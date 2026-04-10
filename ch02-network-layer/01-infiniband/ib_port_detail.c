/**
 * ib_port_detail.c - InfiniBand Port Attribute Full Query Program
 *
 * Features:
 *   - Open each RDMA device
 *   - Call ibv_query_port() for each port
 *   - Print all fields of the ibv_port_attr structure with English comments
 *   - Use detect_transport() from common/rdma_utils.h to detect transport type
 *
 * Compile:
 *   gcc -o ib_port_detail ib_port_detail.c -I../../common \
 *       ../../common/librdma_utils.a -libverbs
 *
 * Or use Makefile:
 *   make
 *
 * Expected Output:
 *   ===== Device: rxe_0 =====
 *     Transport Type: RoCE
 *     --- Port 1 Attributes ---
 *     state            = 4 (ACTIVE)          -- Port state
 *     max_mtu          = 4096                -- Maximum supported MTU
 *     active_mtu       = 1024                -- Current active MTU
 *     ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>

/* Include common utility library */
#include "rdma_utils.h"

/* ========== Helper Functions: Convert enum values to readable strings ========== */

/**
 * port_state_str - Convert port state enum to readable string
 */
static const char *port_state_str(enum ibv_port_state state)
{
    switch (state) {
    case IBV_PORT_NOP:          return "NOP (No Operation/Reserved)";
    case IBV_PORT_DOWN:         return "DOWN (Port Down)";
    case IBV_PORT_INIT:         return "INIT (Initializing)";
    case IBV_PORT_ARMED:        return "ARMED (Ready, Waiting for Activation)";
    case IBV_PORT_ACTIVE:       return "ACTIVE (Active)";
    default:                    return "UNKNOWN (Unknown)";
    }
}

/**
 * mtu_to_bytes - Convert ibv_mtu enum to actual byte count
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
 * link_layer_str - Convert link layer type to readable string
 */
static const char *link_layer_str(uint8_t link_layer)
{
    switch (link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED:    return "UNSPECIFIED (Unspecified)";
    case IBV_LINK_LAYER_INFINIBAND:     return "INFINIBAND (InfiniBand)";
    case IBV_LINK_LAYER_ETHERNET:       return "ETHERNET (Ethernet/RoCE)";
    default:                            return "UNKNOWN (Unknown)";
    }
}

/**
 * width_str - Convert link width to readable string
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
 * speed_str - Convert link speed to readable string
 */
static const char *speed_str(uint8_t speed)
{
    switch (speed) {
    case 1:  return "2.5 Gbps (SDR)";
    case 2:  return "5.0 Gbps (DDR)";
    case 4:  return "10.0 Gbps (QDR)";     /* Could also be 10GbE */
    case 8:  return "10.0 Gbps (FDR10)";
    case 16: return "14.0 Gbps (FDR)";
    case 32: return "25.0 Gbps (EDR)";
    case 64: return "50.0 Gbps (HDR)";
    default: return "UNKNOWN";
    }
}

/**
 * print_port_attr - Print all fields of the ibv_port_attr structure
 *
 * Each field is accompanied by an English comment explaining its purpose
 */
static void print_port_attr(const struct ibv_port_attr *attr, uint8_t port_num)
{
    printf("\n  --- Port %u Attributes ---\n", port_num);

    /* Port state: Indicates current running state (DOWN/INIT/ARMED/ACTIVE) */
    printf("  %-20s = %d (%s)\n", "state",
           attr->state, port_state_str(attr->state));

    /* Max MTU: Maximum transmission unit supported by port hardware */
    printf("  %-20s = %d (%d bytes)\n", "max_mtu",
           attr->max_mtu, mtu_to_bytes(attr->max_mtu));

    /* Active MTU: Actual MTU in use after negotiation */
    printf("  %-20s = %d (%d bytes)\n", "active_mtu",
           attr->active_mtu, mtu_to_bytes(attr->active_mtu));

    /* GID table length: Number of GID entries for this port (used for RoCE addressing) */
    printf("  %-20s = %d\n", "gid_tbl_len",
           attr->gid_tbl_len);

    /* Port capability flags: Bitmask indicating features supported by the port */
    printf("  %-20s = 0x%08x\n", "port_cap_flags",
           attr->port_cap_flags);

    /* Max message size: Maximum bytes allowed in a single message */
    printf("  %-20s = %u\n", "max_msg_sz",
           attr->max_msg_sz);

    /* Bad P_Key counter: Count of packets received with invalid P_Key (security related) */
    printf("  %-20s = %u\n", "bad_pkey_cntr",
           attr->bad_pkey_cntr);

    /* Q_Key violation counter: Count of UD packets received with wrong Q_Key */
    printf("  %-20s = %u\n", "qkey_viol_cntr",
           attr->qkey_viol_cntr);

    /* P_Key table length: Number of entries in the partition key table (used for multi-tenant isolation) */
    printf("  %-20s = %u\n", "pkey_tbl_len",
           attr->pkey_tbl_len);

    /* LID (Local Identifier): Local identifier within IB subnet (assigned by SM) */
    printf("  %-20s = %u (0x%04x)\n", "lid",
           attr->lid, attr->lid);

    /* SM LID: LID of the Subnet Manager */
    printf("  %-20s = %u (0x%04x)\n", "sm_lid",
           attr->sm_lid, attr->sm_lid);

    /* LMC (LID Mask Control): Allows a port to have 2^LMC consecutive LIDs */
    printf("  %-20s = %u\n", "lmc",
           attr->lmc);

    /* Max VL count: Number of supported virtual lanes (QoS related) */
    printf("  %-20s = %u\n", "max_vl_num",
           attr->max_vl_num);

    /* SM SL: Service Level used to reach the Subnet Manager */
    printf("  %-20s = %u\n", "sm_sl",
           attr->sm_sl);

    /* Subnet timeout: Timeout value related to subnet management */
    printf("  %-20s = %u\n", "subnet_timeout",
           attr->subnet_timeout);

    /* Init type reply: Port initialization type information */
    printf("  %-20s = %u\n", "init_type_reply",
           attr->init_type_reply);

    /* Active width: Currently negotiated link width (1X/4X/8X/12X) */
    printf("  %-20s = %u (%s)\n", "active_width",
           attr->active_width, width_str(attr->active_width));

    /* Active speed: Currently negotiated link speed */
    printf("  %-20s = %u (%s)\n", "active_speed",
           attr->active_speed, speed_str(attr->active_speed));

    /* Physical state: Physical link state (Sleep/Polling/LinkUp etc.) */
    printf("  %-20s = %u\n", "phys_state",
           attr->phys_state);

    /* Link layer type: InfiniBand / Ethernet (RoCE) / Unspecified */
    printf("  %-20s = %u (%s)\n", "link_layer",
           attr->link_layer, link_layer_str(attr->link_layer));
}

/* ========== Main Function ========== */

int main(int argc, char *argv[])
{
    struct ibv_device **dev_list = NULL;    /* Device list pointer */
    int num_devices = 0;                    /* Number of devices */
    int ret = 0;                            /* Return value */

    printf("==============================================\n");
    printf("  IB Port Attribute Full Query Tool\n");
    printf("  Display all fields using ibv_query_port()\n");
    printf("==============================================\n");

    /* Step 1: Get list of all RDMA devices */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "[Error] ibv_get_device_list() failed: %s\n",
                strerror(errno));
        return 1;
    }

    if (num_devices == 0) {
        fprintf(stderr, "[Error] No RDMA devices found\n");
        fprintf(stderr, "  Hint: Please verify RDMA drivers are loaded\n");
        fprintf(stderr, "  - SoftRoCE: sudo rdma link add rxe_0 type rxe netdev eth0\n");
        fprintf(stderr, "  - IB: modprobe mlx5_ib\n");
        ret = 1;
        goto cleanup;
    }

    printf("\nFound %d RDMA device(s)\n", num_devices);

    /* Step 2: Iterate over each device */
    for (int i = 0; i < num_devices; i++) {
        struct ibv_device *dev = dev_list[i];           /* Current device */
        struct ibv_context *ctx = NULL;                 /* Device context */
        struct ibv_device_attr dev_attr;                /* Device attributes */

        printf("\n===== Device %d: %s =====\n", i, ibv_get_device_name(dev));

        /* Open device, get context handle */
        ctx = ibv_open_device(dev);
        if (!ctx) {
            fprintf(stderr, "[Error] Cannot open device %s: %s\n",
                    ibv_get_device_name(dev), strerror(errno));
            continue;   /* Skip this device, continue to next */
        }

        /* Use common library's detect_transport() to detect transport type */
        enum rdma_transport transport = detect_transport(ctx, 1);
        printf("  Transport Type: %s\n", transport_str(transport));

        /* Query device attributes to get port count */
        memset(&dev_attr, 0, sizeof(dev_attr));
        ret = ibv_query_device(ctx, &dev_attr);
        if (ret) {
            fprintf(stderr, "[Error] ibv_query_device() failed: %s\n",
                    strerror(errno));
            ibv_close_device(ctx);
            continue;
        }

        printf("  Device port count: %d\n", dev_attr.phys_port_cnt);

        /* Step 3: Iterate over each port, query and print attributes */
        for (uint8_t port = 1; port <= dev_attr.phys_port_cnt; port++) {
            struct ibv_port_attr port_attr;             /* Port attribute structure */

            memset(&port_attr, 0, sizeof(port_attr));

            /* Query specified port attributes */
            ret = ibv_query_port(ctx, port, &port_attr);
            if (ret) {
                fprintf(stderr, "[Error] ibv_query_port(port=%u) failed: %s\n",
                        port, strerror(errno));
                continue;   /* Skip this port */
            }

            /* Print all attribute fields for this port */
            print_port_attr(&port_attr, port);
        }

        /* Close device */
        ibv_close_device(ctx);
    }

cleanup:
    /* Free device list */
    if (dev_list) {
        ibv_free_device_list(dev_list);
    }

    printf("\nQuery complete.\n");
    return ret;
}
