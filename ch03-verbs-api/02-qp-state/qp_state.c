/**
 * QP State Transition Example (Enhanced - IB/RoCE Dual-Mode Support)
 *
 * Demonstrates how to transition QP from RESET -> INIT -> RTR -> RTS
 * Automatically detects transport type (IB uses LID, RoCE uses GID)
 * Calls ibv_query_qp() after each state transition to print full QP attributes
 *
 * Compile: gcc -o qp_state qp_state.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define PORT_NUM RDMA_DEFAULT_PORT_NUM

/* Resource structure */
struct rdma_dev {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
};

/**
 * Query and print detailed QP attributes (enhanced version)
 *
 * Called after each state transition to show complete QP parameter changes.
 * Selects appropriate query mask based on current QP state.
 */
void query_and_print_qp_full(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;

    /* Query as many attributes as possible based on QP state */
    int mask = IBV_QP_STATE | IBV_QP_CUR_STATE | IBV_QP_PKEY_INDEX |
               IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PATH_MTU |
               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN |
               IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
               IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_MAX_DEST_RD_ATOMIC |
               IBV_QP_MIN_RNR_TIMER | IBV_QP_AV;

    memset(&attr, 0, sizeof(attr));
    memset(&init_attr, 0, sizeof(init_attr));

    if (ibv_query_qp(qp, &attr, mask, &init_attr) != 0) {
        /* Some attributes cannot be queried in lower states, retry with minimal mask */
        mask = IBV_QP_STATE | IBV_QP_CUR_STATE;
        memset(&attr, 0, sizeof(attr));
        if (ibv_query_qp(qp, &attr, mask, &init_attr) != 0) {
            fprintf(stderr, "  [Error] ibv_query_qp failed\n");
            return;
        }
    }

    printf("=== QP #%u Full Attributes ===\n", qp->qp_num);
    printf("  QP number:                 %u\n", qp->qp_num);
    printf("  QP type:                   %s\n",
           qp->qp_type == IBV_QPT_RC ? "RC" :
           qp->qp_type == IBV_QPT_UC ? "UC" :
           qp->qp_type == IBV_QPT_UD ? "UD" : "Other");
    printf("  State (qp_state):          %s (%d)\n",
           qp_state_str(attr.qp_state), attr.qp_state);
    printf("  Current state (cur_qp_state): %s (%d)\n",
           qp_state_str(attr.cur_qp_state), attr.cur_qp_state);

    /* Attributes for INIT state and above */
    if (attr.qp_state >= IBV_QPS_INIT) {
        printf("  Port number (port_num):    %d\n", attr.port_num);
        printf("  PKey index:                %d\n", attr.pkey_index);
        printf("  Access flags:              0x%x", attr.qp_access_flags);
        if (attr.qp_access_flags & IBV_ACCESS_LOCAL_WRITE)   printf(" LOCAL_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_READ)   printf(" REMOTE_READ");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_WRITE)  printf(" REMOTE_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_ATOMIC) printf(" REMOTE_ATOMIC");
        printf("\n");
    }

    /* Attributes for RTR state and above */
    if (attr.qp_state >= IBV_QPS_RTR) {
        printf("  Path MTU:                  %d", attr.path_mtu);
        switch (attr.path_mtu) {
        case IBV_MTU_256:  printf(" (256 bytes)"); break;
        case IBV_MTU_512:  printf(" (512 bytes)"); break;
        case IBV_MTU_1024: printf(" (1024 bytes)"); break;
        case IBV_MTU_2048: printf(" (2048 bytes)"); break;
        case IBV_MTU_4096: printf(" (4096 bytes)"); break;
        default: break;
        }
        printf("\n");
        printf("  Dest QP number (dest_qp_num): %u\n", attr.dest_qp_num);
        printf("  RQ PSN:                    %u\n", attr.rq_psn);
        printf("  Max dest RD atomic:        %d\n", attr.max_dest_rd_atomic);
        printf("  Min RNR timer:             %d\n", attr.min_rnr_timer);

        /* Address Vector (AH) information */
        printf("  --- Address Vector (AH Attr) ---\n");
        printf("    DLID:                    %u\n", attr.ah_attr.dlid);
        printf("    SL:                      %d\n", attr.ah_attr.sl);
        printf("    Port number:             %d\n", attr.ah_attr.port_num);
        printf("    is_global:               %d\n", attr.ah_attr.is_global);
        if (attr.ah_attr.is_global) {
            char gid_str[46];
            gid_to_str(&attr.ah_attr.grh.dgid, gid_str, sizeof(gid_str));
            printf("    GRH.dgid:                %s\n", gid_str);
            printf("    GRH.sgid_index:          %d\n", attr.ah_attr.grh.sgid_index);
            printf("    GRH.hop_limit:           %d\n", attr.ah_attr.grh.hop_limit);
            printf("    GRH.flow_label:          %u\n", attr.ah_attr.grh.flow_label);
            printf("    GRH.traffic_class:       %d\n", attr.ah_attr.grh.traffic_class);
        }
    }

    /* Attributes for RTS state and above */
    if (attr.qp_state >= IBV_QPS_RTS) {
        printf("  SQ PSN:                    %u\n", attr.sq_psn);
        printf("  Timeout:                   %d\n", attr.timeout);
        printf("  Retry count (retry_cnt):   %d\n", attr.retry_cnt);
        printf("  RNR retry (rnr_retry):     %d\n", attr.rnr_retry);
        printf("  Max RD atomic:             %d\n", attr.max_rd_atomic);
    }

    /* QP initial attributes */
    printf("  --- QP Init Attr ---\n");
    printf("    max_send_wr:             %d\n", init_attr.cap.max_send_wr);
    printf("    max_recv_wr:             %d\n", init_attr.cap.max_recv_wr);
    printf("    max_send_sge:            %d\n", init_attr.cap.max_send_sge);
    printf("    max_recv_sge:            %d\n", init_attr.cap.max_recv_sge);
    printf("    max_inline_data:         %d\n", init_attr.cap.max_inline_data);
    printf("=============================\n");
}

/* Get port information (original version preserved, added GID printing) */
void print_port_info(struct ibv_context *context, uint8_t port)
{
    struct ibv_port_attr attr;

    if (ibv_query_port(context, port, &attr) == 0) {
        printf("=== Port %d Info ===\n", port);
        printf("Link layer: %s\n",
               attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" : "Ethernet (RoCE)");
        printf("State: %s\n",
               attr.state == IBV_PORT_ACTIVE ? "ACTIVE" : "not ACTIVE");
        printf("LID: %u\n", attr.lid);
        printf("SM LID: %u\n", attr.sm_lid);
        printf("GID table size: %d\n", attr.gid_tbl_len);

        /* Print the first non-zero GID */
        union ibv_gid gid;
        if (ibv_query_gid(context, port, 0, &gid) == 0) {
            char gid_str[46];
            gid_to_str(&gid, gid_str, sizeof(gid_str));
            printf("GID[0]: %s\n", gid_str);
        }
        printf("===================\n");
    }
}

/* RESET -> INIT (same as before) */
int qp_reset_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = PORT_NUM,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE,
    };

    int ret = ibv_modify_qp(qp, &attr,
                             IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                             IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        perror("QP RESET->INIT failed");
    }
    return ret;
}

/*
 * INIT -> RTR (enhanced - auto-detect IB/RoCE)
 *
 * Automatically selects addressing mode based on port link_layer:
 *   - IB:   uses remote_lid (LID addressing)
 *   - RoCE: uses remote_gid (GID addressing, is_global=1)
 */
int qp_init_to_rtr(struct ibv_qp *qp, struct ibv_context *context,
                   uint32_t remote_qp_num, uint16_t remote_lid,
                   union ibv_gid *remote_gid, int gid_index, int is_roce)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = RDMA_DEFAULT_MTU;
    attr.dest_qp_num        = remote_qp_num;
    attr.rq_psn             = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;

    /* Address vector: select addressing mode based on transport type */
    attr.ah_attr.sl       = 0;
    attr.ah_attr.port_num = PORT_NUM;

    if (is_roce) {
        /*
         * RoCE mode: must set is_global=1 and fill in GRH
         * dgid = remote GID (for loopback, this is the local GID)
         * sgid_index = index in the local GID table
         */
        printf("  [RoCE mode] Using GID addressing (is_global=1)\n");
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = *remote_gid;
        attr.ah_attr.grh.sgid_index = gid_index;
        attr.ah_attr.grh.hop_limit  = 64;
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.traffic_class = 0;
        attr.ah_attr.dlid = 0;     /* dlid not used in RoCE */

        char gid_str[46];
        gid_to_str(remote_gid, gid_str, sizeof(gid_str));
        printf("  dgid = %s, sgid_index = %d\n", gid_str, gid_index);
    } else {
        /*
         * IB mode: use LID addressing, is_global=0
         */
        printf("  [IB mode] Using LID addressing (dlid=%u)\n", remote_lid);
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid      = remote_lid;
    }

    int ret = ibv_modify_qp(qp, &attr,
                             IBV_QP_STATE | IBV_QP_PATH_MTU |
                             IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                             IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                             IBV_QP_AV);
    if (ret) {
        perror("QP INIT->RTR failed");
        if (is_roce) {
            fprintf(stderr, "  Hint: In RoCE mode, check if GID is correct and gid_index is valid\n");
        }
    }
    return ret;
}

/* RTR -> RTS (same as before) */
int qp_rtr_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = 0,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .max_rd_atomic = 1,
    };

    int ret = ibv_modify_qp(qp, &attr,
                             IBV_QP_STATE | IBV_QP_SQ_PSN |
                             IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                             IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        perror("QP RTR->RTS failed");
    }
    return ret;
}

int main(int argc, char *argv[])
{
    struct rdma_dev dev = {0};
    struct ibv_device **list;
    int num;

    printf("=== QP State Transition Example (IB/RoCE Dual-Mode Support) ===\n");
    printf("=============================================\n\n");

    /* Get device */
    list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return 1;
    }

    /* Open device */
    dev.context = ibv_open_device(list[0]);
    if (!dev.context) {
        perror("Failed to open device");
        return 1;
    }
    printf("Device: %s\n\n", ibv_get_device_name(list[0]));

    /* Print port information */
    print_port_info(dev.context, PORT_NUM);

    /* Auto-detect transport type */
    enum rdma_transport transport = detect_transport(dev.context, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("\nDetected transport type: %s\n", transport_str(transport));
    if (is_roce) {
        printf("-> Will use GID addressing (RoCE mode)\n\n");
    } else {
        printf("-> Will use LID addressing (IB mode)\n\n");
    }

    /* Allocate PD */
    dev.pd = ibv_alloc_pd(dev.context);
    if (!dev.pd) {
        perror("Failed to allocate PD");
        return 1;
    }

    /* Create CQ */
    dev.cq = ibv_create_cq(dev.context, 128, NULL, NULL, 0);
    if (!dev.cq) {
        perror("Failed to create CQ");
        return 1;
    }

    /* Create QP */
    struct ibv_qp_init_attr init_attr = {
        .send_cq = dev.cq,
        .recv_cq = dev.cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = 128,
            .max_recv_wr = 128,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
    };
    dev.qp = ibv_create_qp(dev.pd, &init_attr);
    if (!dev.qp) {
        perror("Failed to create QP");
        return 1;
    }

    /* Print initial QP state (RESET) */
    printf("--- Initial state (after creation) ---\n");
    query_and_print_qp_full(dev.qp);

    /* ========== State transition 1: RESET -> INIT ========== */
    printf("\n[Step 1] RESET -> INIT\n");
    if (qp_reset_to_init(dev.qp)) {
        return 1;
    }
    printf("  Transition succeeded!\n");
    query_and_print_qp_full(dev.qp);

    /* ========== Prepare loopback parameters ========== */
    /*
     * Get local port info for loopback testing.
     * IB mode: needs LID
     * RoCE mode: needs GID
     * In production, these need to be exchanged with the remote peer via TCP socket or RDMA CM.
     */
    struct ibv_port_attr port_attr;
    if (ibv_query_port(dev.context, PORT_NUM, &port_attr) != 0) {
        perror("Failed to query port");
        return 1;
    }
    uint16_t local_lid = port_attr.lid;
    uint32_t local_qp_num = dev.qp->qp_num;

    /* Query local GID (needed for RoCE mode) */
    union ibv_gid local_gid;
    int gid_index = RDMA_DEFAULT_GID_INDEX;
    if (ibv_query_gid(dev.context, PORT_NUM, gid_index, &local_gid) != 0) {
        fprintf(stderr, "Failed to query GID[%d]\n", gid_index);
        /* Can continue in IB mode, will fail in RoCE mode */
    } else {
        char gid_str[46];
        gid_to_str(&local_gid, gid_str, sizeof(gid_str));
        printf("\nLoopback parameters:\n");
        printf("  local_qp_num = %u\n", local_qp_num);
        printf("  local_lid    = %u\n", local_lid);
        printf("  local_gid[%d] = %s\n\n", gid_index, gid_str);
    }

    /* ========== State transition 2: INIT -> RTR ========== */
    printf("[Step 2] INIT -> RTR (loopback: remote_qp=%u)\n", local_qp_num);
    if (qp_init_to_rtr(dev.qp, dev.context, local_qp_num, local_lid,
                        &local_gid, gid_index, is_roce)) {
        return 1;
    }
    printf("  Transition succeeded!\n");
    query_and_print_qp_full(dev.qp);

    /* ========== State transition 3: RTR -> RTS ========== */
    printf("\n[Step 3] RTR -> RTS\n");
    if (qp_rtr_to_rts(dev.qp)) {
        return 1;
    }
    printf("  Transition succeeded!\n");
    query_and_print_qp_full(dev.qp);

    printf("\n=== QP state machine complete: RESET -> INIT -> RTR -> RTS ===\n");
    printf("QP is ready for data transfer.\n");
    printf("Transport mode: %s\n\n", is_roce ? "RoCE (GID addressing)" : "IB (LID addressing)");

    /* Cleanup */
    printf("[Cleanup] Releasing resources...\n");
    ibv_destroy_qp(dev.qp);
    ibv_destroy_cq(dev.cq);
    ibv_dealloc_pd(dev.pd);
    ibv_close_device(dev.context);
    ibv_free_device_list(list);
    printf("  Done\n");

    return 0;
}
