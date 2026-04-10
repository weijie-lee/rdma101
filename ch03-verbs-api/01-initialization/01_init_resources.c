/**
 * RDMA Resource Initialization - Six-Step Method (Enhanced Version)
 *
 * This program demonstrates the six initialization steps of RDMA programming:
 * 1. Get device list
 * 2. Open device + Query device attributes (ibv_query_device) + Query port attributes (ibv_query_port)
 * 3. Allocate Protection Domain (PD)
 * 4. Register Memory Region (MR)
 * 5. Create Completion Queue (CQ)
 * 6. Create Queue Pair (QP)
 *
 * Compile: gcc -o 01_init_resources 01_init_resources.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define BUFFER_SIZE 4096

int main(int argc, char *argv[])
{
    struct ibv_device **device_list = NULL;
    struct ibv_device *device = NULL;
    struct ibv_context *context = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp = NULL;
    char *buffer = NULL;
    int num_devices;
    int i;

    printf("=== RDMA Resource Initialization - Six-Step Method ===\n\n");

    /* ========== Step 1: Get device list ========== */
    printf("[Step 1] Get device list\n");
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        perror("  ibv_get_device_list failed");
        return 1;
    }
    printf("  Found %d RDMA device(s)\n", num_devices);

    for (i = 0; i < num_devices; i++) {
        printf("  Device[%d]: %s\n", i, ibv_get_device_name(device_list[i]));
    }

    /* Select the first device */
    device = device_list[0];
    if (!device) {
        fprintf(stderr, "  No available device\n");
        goto cleanup;
    }

    /* ========== Step 2: Open device ========== */
    printf("\n[Step 2] Open device\n");
    context = ibv_open_device(device);
    if (!context) {
        perror("  ibv_open_device failed");
        goto cleanup;
    }
    printf("  Device opened successfully: %s\n", ibv_get_device_name(device));

    /* ========== Step 2a: Query device attributes (ibv_query_device) ========== */
    printf("\n[Step 2a] Query device attributes (ibv_query_device)\n");
    struct ibv_device_attr dev_attr;
    if (ibv_query_device(context, &dev_attr) == 0) {
        printf("  --- Basic Information ---\n");
        printf("  Firmware version (fw_ver):   %s\n", dev_attr.fw_ver);
        printf("  Node GUID:                   0x%016lx\n", (unsigned long)dev_attr.node_guid);
        printf("  System image GUID:           0x%016lx\n", (unsigned long)dev_attr.sys_image_guid);
        printf("  Vendor ID:                   0x%x\n", dev_attr.vendor_id);
        printf("  Vendor Part ID:              %d\n", dev_attr.vendor_part_id);
        printf("  Hardware version:            %d\n", dev_attr.hw_ver);
        printf("  Physical port count:         %d\n", dev_attr.phys_port_cnt);
        printf("  --- Queue Capabilities ---\n");
        printf("  Max QP count:                %d\n", dev_attr.max_qp);
        printf("  Max WR per QP:               %d\n", dev_attr.max_qp_wr);
        printf("  Max SGE per WR:              %d\n", dev_attr.max_sge);
        printf("  Max CQ count:                %d\n", dev_attr.max_cq);
        printf("  Max CQE per CQ:              %d\n", dev_attr.max_cqe);
        printf("  Max SRQ count:               %d\n", dev_attr.max_srq);
        printf("  Max WR per SRQ:              %d\n", dev_attr.max_srq_wr);
        printf("  --- Memory Region ---\n");
        printf("  Max MR count:                %d\n", dev_attr.max_mr);
        printf("  Max MR size:                 %lu bytes\n", (unsigned long)dev_attr.max_mr_size);
        printf("  Max PD count:                %d\n", dev_attr.max_pd);
        printf("  Page size mask:              0x%lx\n", (unsigned long)dev_attr.page_size_cap);
        printf("  --- Atomic Operations ---\n");
        printf("  Atomic capability:           ");
        switch (dev_attr.atomic_cap) {
        case IBV_ATOMIC_NONE:  printf("NONE (not supported)\n"); break;
        case IBV_ATOMIC_HCA:   printf("HCA (atomic within HCA only)\n"); break;
        case IBV_ATOMIC_GLOB:  printf("GLOBAL (global atomic)\n"); break;
        default:               printf("%d\n", dev_attr.atomic_cap); break;
        }
        printf("  Max QP RD atomic ops:        %d\n", dev_attr.max_qp_rd_atom);
        printf("  Max QP INIT RD atomic:       %d\n", dev_attr.max_qp_init_rd_atom);
        printf("  --- Other ---\n");
        printf("  Max AH count:                %d\n", dev_attr.max_ah);
        printf("  Max multicast group count:   %d\n", dev_attr.max_mcast_grp);
        printf("  Max QP per multicast group:  %d\n", dev_attr.max_mcast_qp_attach);
    } else {
        perror("  ibv_query_device failed");
    }

    /* ========== Step 2b: Query all port attributes (ibv_query_port) ========== */
    printf("\n[Step 2b] Query port attributes (ibv_query_port)\n");
    int phys_port_cnt = (ibv_query_device(context, &dev_attr) == 0)
                        ? dev_attr.phys_port_cnt : 1;
    for (int port = 1; port <= phys_port_cnt; port++) {
        struct ibv_port_attr port_attr;
        if (ibv_query_port(context, port, &port_attr) == 0) {
            printf("  --- Port %d ---\n", port);
            printf("  State:         ");
            switch (port_attr.state) {
            case IBV_PORT_DOWN:    printf("DOWN\n"); break;
            case IBV_PORT_INIT:    printf("INIT\n"); break;
            case IBV_PORT_ARMED:   printf("ARMED\n"); break;
            case IBV_PORT_ACTIVE:  printf("ACTIVE\n"); break;
            default:               printf("%d\n", port_attr.state); break;
            }
            printf("  Max MTU:       ");
            switch (port_attr.max_mtu) {
            case IBV_MTU_256:  printf("256\n"); break;
            case IBV_MTU_512:  printf("512\n"); break;
            case IBV_MTU_1024: printf("1024\n"); break;
            case IBV_MTU_2048: printf("2048\n"); break;
            case IBV_MTU_4096: printf("4096\n"); break;
            default:           printf("%d\n", port_attr.max_mtu); break;
            }
            printf("  Active MTU:    ");
            switch (port_attr.active_mtu) {
            case IBV_MTU_256:  printf("256\n"); break;
            case IBV_MTU_512:  printf("512\n"); break;
            case IBV_MTU_1024: printf("1024\n"); break;
            case IBV_MTU_2048: printf("2048\n"); break;
            case IBV_MTU_4096: printf("4096\n"); break;
            default:           printf("%d\n", port_attr.active_mtu); break;
            }
            printf("  Link layer:    %s\n",
                   port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" :
                   port_attr.link_layer == IBV_LINK_LAYER_ETHERNET   ? "Ethernet (RoCE)" :
                   "Unknown");
            printf("  LID:           %u%s\n", port_attr.lid,
                   port_attr.lid == 0 ? " (RoCE mode, need GID for addressing)" : "");
            printf("  SM LID:        %u\n", port_attr.sm_lid);
            printf("  GID table size:    %d\n", port_attr.gid_tbl_len);
            printf("  P_Key table size:  %u\n", port_attr.pkey_tbl_len);
            printf("  Active speed:      width=%d, speed=%d\n",
                   port_attr.active_width, port_attr.active_speed);
        } else {
            printf("  Port %d query failed\n", port);
        }
    }

    /* ========== Step 3: Allocate Protection Domain (PD) ========== */
    printf("\n[Step 3] Allocate Protection Domain (PD)\n");
    pd = ibv_alloc_pd(context);
    if (!pd) {
        perror("  ibv_alloc_pd failed");
        goto cleanup;
    }
    printf("  PD allocated successfully, handle=%d\n", pd->handle);

    /* ========== Step 4: Register Memory Region (MR) ========== */
    printf("\n[Step 4] Register Memory Region (MR)\n");
    buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("  malloc failed");
        goto cleanup;
    }
    memset(buffer, 0, BUFFER_SIZE);

    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
                     IBV_ACCESS_LOCAL_WRITE |
                     IBV_ACCESS_REMOTE_READ |
                     IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("  ibv_reg_mr failed");
        goto cleanup;
    }
    printf("  MR registered successfully\n");
    printf("    lkey=0x%x (local access)\n", mr->lkey);
    printf("    rkey=0x%x (remote access)\n", mr->rkey);
    printf("    virtual address=%p\n", mr->addr);
    printf("    length=%d\n", mr->length);

    /* ========== Step 5: Create Completion Queue (CQ) ========== */
    printf("\n[Step 5] Create Completion Queue (CQ)\n");
    cq = ibv_create_cq(context, 128, NULL, NULL, 0);
    if (!cq) {
        perror("  ibv_create_cq failed");
        goto cleanup;
    }
    printf("  CQ created successfully, size=%d\n", 128);
    printf("    cq_handle=%d\n", cq->handle);

    /* ========== Step 6: Create Queue Pair (QP) ========== */
    printf("\n[Step 6] Create Queue Pair (QP)\n");
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 128,
            .max_recv_wr = 128,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };

    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        perror("  ibv_create_qp failed");
        goto cleanup;
    }
    printf("  QP created successfully\n");
    printf("    QP number=%u\n", qp->qp_num);
    printf("    state=RESET (newly created QP is in RESET state)\n");

    printf("\n=== All resources initialized successfully ===\n\n");

    /* Print resource relationships */
    printf("Resource relationships:\n");
    printf("  Device -> Context (ibv_open_device)\n");
    printf("  Context -> PD (ibv_alloc_pd)\n");
    printf("  PD -> MR (ibv_reg_mr) - for memory registration\n");
    printf("  Context -> CQ (ibv_create_cq) - for completion notification\n");
    printf("  PD + CQ -> QP (ibv_create_qp) - for communication\n");

cleanup:
    /* Release resources in reverse order */
    printf("\n[Cleanup] Releasing resources...\n");

    if (qp) {
        ibv_destroy_qp(qp);
        printf("  QP destroyed\n");
    }
    if (cq) {
        ibv_destroy_cq(cq);
        printf("  CQ destroyed\n");
    }
    if (mr) {
        ibv_dereg_mr(mr);
        printf("  MR deregistered\n");
    }
    if (buffer) {
        free(buffer);
    }
    if (pd) {
        ibv_dealloc_pd(pd);
        printf("  PD deallocated\n");
    }
    if (context) {
        ibv_close_device(context);
        printf("  Device closed\n");
    }
    if (device_list) {
        ibv_free_device_list(device_list);
        printf("  Device list freed\n");
    }

    printf("\nProgram finished\n");
    return 0;
}
