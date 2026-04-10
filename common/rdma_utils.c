/**
 * RDMA Common Utility Library - Implementation File
 *
 * Provides unified abstraction function implementations with IB/RoCE dual-mode support.
 *
 * Build: gcc -c rdma_utils.c -o rdma_utils.o -Wall -O2
 *        ar rcs librdma_utils.a rdma_utils.o
 */

#include "rdma_utils.h"

/* ========== Transport Layer Detection ========== */

enum rdma_transport detect_transport(struct ibv_context *ctx, uint8_t port)
{
    struct ibv_port_attr port_attr;

    if (ibv_query_port(ctx, port, &port_attr) != 0) {
        fprintf(stderr, "[detect_transport] Failed to query port %d: %s\n",
                port, strerror(errno));
        return RDMA_TRANSPORT_UNKNOWN;
    }

    switch (port_attr.link_layer) {
    case IBV_LINK_LAYER_INFINIBAND:
        return RDMA_TRANSPORT_IB;
    case IBV_LINK_LAYER_ETHERNET:
        /*
         * Ethernet link layer could be RoCE or iWARP.
         * Further distinguish via device's transport_type.
         * Note: In verbs API, RoCE device's node_type is IBV_NODE_CA,
         *       transport_type is IBV_TRANSPORT_IB.
         */
        return RDMA_TRANSPORT_ROCE;
    default:
        return RDMA_TRANSPORT_UNKNOWN;
    }
}

const char *transport_str(enum rdma_transport t)
{
    switch (t) {
    case RDMA_TRANSPORT_IB:      return "InfiniBand";
    case RDMA_TRANSPORT_ROCE:    return "RoCE (RDMA over Converged Ethernet)";
    case RDMA_TRANSPORT_IWARP:   return "iWARP";
    case RDMA_TRANSPORT_UNKNOWN: return "Unknown";
    default:                     return "Invalid";
    }
}

/* ========== Device Query and Printing ========== */

int query_and_print_device(struct ibv_context *ctx)
{
    struct ibv_device_attr dev_attr;

    if (ibv_query_device(ctx, &dev_attr) != 0) {
        fprintf(stderr, "[query_and_print_device] ibv_query_device failed: %s\n",
                strerror(errno));
        return -1;
    }

    printf("=== Device Capability Parameters ===\n");
    printf("  Firmware version (fw_ver):    %s\n",   dev_attr.fw_ver);
    printf("  Node GUID (node_guid):        0x%016lx\n", (unsigned long)dev_attr.node_guid);
    printf("  System image GUID:            0x%016lx\n", (unsigned long)dev_attr.sys_image_guid);
    printf("  Max MR size:                  %lu bytes\n", (unsigned long)dev_attr.max_mr_size);
    printf("  Page size mask:               0x%lx\n", (unsigned long)dev_attr.page_size_cap);
    printf("  Vendor ID:                    0x%x\n",  dev_attr.vendor_id);
    printf("  Vendor Part ID:               %d\n",    dev_attr.vendor_part_id);
    printf("  Hardware version:             %d\n",    dev_attr.hw_ver);
    printf("  Physical port count:          %d\n",    dev_attr.phys_port_cnt);
    printf("  --- Queues and Completion Queues ---\n");
    printf("  Max QP count (max_qp):        %d\n",    dev_attr.max_qp);
    printf("  Max WR per QP (max_qp_wr):    %d\n",   dev_attr.max_qp_wr);
    printf("  Max SGE per WR (max_sge):     %d\n",   dev_attr.max_sge);
    printf("  Max SRQ count:                %d\n",    dev_attr.max_srq);
    printf("  Max WR per SRQ:               %d\n",    dev_attr.max_srq_wr);
    printf("  Max CQ count (max_cq):        %d\n",    dev_attr.max_cq);
    printf("  Max CQE per CQ (max_cqe):     %d\n",    dev_attr.max_cqe);
    printf("  --- Memory Regions ---\n");
    printf("  Max MR count (max_mr):        %d\n",    dev_attr.max_mr);
    printf("  Max PD count (max_pd):        %d\n",    dev_attr.max_pd);
    printf("  --- Atomic Operations ---\n");
    printf("  Atomic capability (atomic_cap): ");
    switch (dev_attr.atomic_cap) {
    case IBV_ATOMIC_NONE:  printf("NONE (not supported)\n"); break;
    case IBV_ATOMIC_HCA:   printf("HCA (atomic within this HCA only)\n"); break;
    case IBV_ATOMIC_GLOB:  printf("GLOBAL (global atomic)\n"); break;
    default:               printf("%d (unknown)\n", dev_attr.atomic_cap); break;
    }
    printf("  Max QP RD atomic ops:         %d\n",    dev_attr.max_qp_rd_atom);
    printf("  Max QP INIT RD atomic ops:    %d\n",    dev_attr.max_qp_init_rd_atom);
    printf("  --- Multicast ---\n");
    printf("  Max multicast groups:         %d\n",    dev_attr.max_mcast_grp);
    printf("  Max QPs per multicast group:  %d\n",    dev_attr.max_mcast_qp_attach);
    printf("  Max AH count:                 %d\n",    dev_attr.max_ah);
    printf("========================\n");

    return 0;
}

int query_and_print_port(struct ibv_context *ctx, uint8_t port)
{
    struct ibv_port_attr attr;

    if (ibv_query_port(ctx, port, &attr) != 0) {
        fprintf(stderr, "[query_and_print_port] Failed to query port %d: %s\n",
                port, strerror(errno));
        return -1;
    }

    printf("=== Port %d Attributes ===\n", port);

    /* Port state */
    printf("  State:                 ");
    switch (attr.state) {
    case IBV_PORT_NOP:          printf("NOP (inactive)\n"); break;
    case IBV_PORT_DOWN:         printf("DOWN (link down)\n"); break;
    case IBV_PORT_INIT:         printf("INIT (initializing)\n"); break;
    case IBV_PORT_ARMED:        printf("ARMED (ready)\n"); break;
    case IBV_PORT_ACTIVE:       printf("ACTIVE\n"); break;
    case IBV_PORT_ACTIVE_DEFER: printf("ACTIVE_DEFER\n"); break;
    default:                    printf("%d (unknown)\n", attr.state); break;
    }

    /* MTU */
    printf("  Max MTU (max_mtu):     ");
    switch (attr.max_mtu) {
    case IBV_MTU_256:  printf("256\n"); break;
    case IBV_MTU_512:  printf("512\n"); break;
    case IBV_MTU_1024: printf("1024\n"); break;
    case IBV_MTU_2048: printf("2048\n"); break;
    case IBV_MTU_4096: printf("4096\n"); break;
    default:           printf("%d\n", attr.max_mtu); break;
    }

    printf("  Active MTU (active_mtu): ");
    switch (attr.active_mtu) {
    case IBV_MTU_256:  printf("256\n"); break;
    case IBV_MTU_512:  printf("512\n"); break;
    case IBV_MTU_1024: printf("1024\n"); break;
    case IBV_MTU_2048: printf("2048\n"); break;
    case IBV_MTU_4096: printf("4096\n"); break;
    default:           printf("%d\n", attr.active_mtu); break;
    }

    /* Link layer */
    printf("  Link layer:            ");
    switch (attr.link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED:  printf("Unspecified\n"); break;
    case IBV_LINK_LAYER_INFINIBAND:   printf("InfiniBand\n"); break;
    case IBV_LINK_LAYER_ETHERNET:     printf("Ethernet (RoCE)\n"); break;
    default:                          printf("%d (unknown)\n", attr.link_layer); break;
    }

    /* Address info */
    printf("  LID:                   %u", attr.lid);
    if (attr.lid == 0) {
        printf("  (LID=0 indicates RoCE mode, use GID for addressing)");
    }
    printf("\n");
    printf("  SM LID:                %u\n",   attr.sm_lid);
    printf("  GID table size:        %d\n",    attr.gid_tbl_len);
    printf("  P_Key table size:      %u\n",   attr.pkey_tbl_len);

    /* Speed */
    printf("  Active speed:          %u Gbps (width=%d, speed=%d)\n",
           (unsigned)(attr.active_width * attr.active_speed),
           attr.active_width, attr.active_speed);

    printf("======================\n");

    return 0;
}

int query_and_print_all_gids(struct ibv_context *ctx, uint8_t port)
{
    struct ibv_port_attr port_attr;
    union ibv_gid gid;
    char gid_str[46];
    int i;

    if (ibv_query_port(ctx, port, &port_attr) != 0) {
        fprintf(stderr, "[query_and_print_all_gids] Failed to query port %d\n", port);
        return -1;
    }

    printf("=== Port %d GID Table (%d entries) ===\n", port, port_attr.gid_tbl_len);

    for (i = 0; i < port_attr.gid_tbl_len; i++) {
        if (ibv_query_gid(ctx, port, i, &gid) != 0) {
            continue;
        }

        /* Skip all-zero GIDs (unconfigured entries) */
        int is_zero = 1;
        for (int j = 0; j < 16; j++) {
            if (gid.raw[j] != 0) { is_zero = 0; break; }
        }
        if (is_zero) continue;

        gid_to_str(&gid, gid_str, sizeof(gid_str));
        printf("  GID[%3d]: %s\n", i, gid_str);
    }

    printf("=======================================\n");
    return 0;
}

void print_gid(const union ibv_gid *gid)
{
    char buf[46];
    gid_to_str(gid, buf, sizeof(buf));
    printf("%s", buf);
}

void gid_to_str(const union ibv_gid *gid, char *buf, size_t buflen)
{
    snprintf(buf, buflen,
             "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             gid->raw[0],  gid->raw[1],  gid->raw[2],  gid->raw[3],
             gid->raw[4],  gid->raw[5],  gid->raw[6],  gid->raw[7],
             gid->raw[8],  gid->raw[9],  gid->raw[10], gid->raw[11],
             gid->raw[12], gid->raw[13], gid->raw[14], gid->raw[15]);
}

/* ========== QP State Transitions ========== */

int qp_to_init(struct ibv_qp *qp, uint8_t port, int access_flags)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = RDMA_DEFAULT_PKEY_INDEX;
    attr.port_num        = port;
    attr.qp_access_flags = access_flags;

    int ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                            IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret != 0) {
        fprintf(stderr, "[qp_to_init] RESET->INIT failed: %s (errno=%d)\n",
                strerror(errno), errno);
    }
    return ret;
}

int qp_to_rtr(struct ibv_qp *qp, const struct rdma_endpoint *remote,
              uint8_t port, int is_roce)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = RDMA_DEFAULT_MTU;
    attr.dest_qp_num        = remote->qp_num;
    attr.rq_psn             = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;  /* 12 ~ 0.01ms, reasonable RNR NAK timeout */

    /* Address Vector */
    attr.ah_attr.sl         = RDMA_DEFAULT_SL;
    attr.ah_attr.port_num   = port;

    if (is_roce) {
        /*
         * RoCE mode: must set is_global=1 and fill GRH (Global Route Header).
         * dgid is the remote GID, sgid_index is the index in the local GID table.
         */
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = remote->gid;
        attr.ah_attr.grh.sgid_index = remote->gid_index;
        attr.ah_attr.grh.hop_limit  = 64;  /* Allow cross-router */
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.traffic_class = 0;
        /* In RoCE mode, dlid can be set to 0 or any value; it doesn't affect addressing */
        attr.ah_attr.dlid = 0;
    } else {
        /*
         * IB mode: uses LID addressing, is_global=0 (GRH not needed for intra-subnet).
         */
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid      = remote->lid;
    }

    int ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_PATH_MTU |
                            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                            IBV_QP_AV);
    if (ret != 0) {
        fprintf(stderr, "[qp_to_rtr] INIT->RTR failed: %s (errno=%d)\n",
                strerror(errno), errno);
        if (is_roce) {
            char gid_str[46];
            gid_to_str(&remote->gid, gid_str, sizeof(gid_str));
            fprintf(stderr, "  Hint: In RoCE mode, check if GID is correct: %s\n", gid_str);
        } else {
            fprintf(stderr, "  Hint: In IB mode, check if LID is correct: %u\n", remote->lid);
        }
    }
    return ret;
}

int qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state      = IBV_QPS_RTS;
    attr.sq_psn         = RDMA_DEFAULT_PSN;
    attr.timeout        = 14;   /* About 8.4 seconds (4.096us * 2^14) */
    attr.retry_cnt      = 7;    /* Max retry count */
    attr.rnr_retry      = 7;    /* RNR NAK max retry count (7 = infinite retry) */
    attr.max_rd_atomic  = 1;    /* Outstanding RDMA Read/Atomic operations */

    int ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_SQ_PSN |
                            IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret != 0) {
        fprintf(stderr, "[qp_to_rts] RTR->RTS failed: %s (errno=%d)\n",
                strerror(errno), errno);
    }
    return ret;
}

int qp_to_reset(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state = IBV_QPS_RESET;

    int ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE);
    if (ret != 0) {
        fprintf(stderr, "[qp_to_reset] ->RESET failed: %s (errno=%d)\n",
                strerror(errno), errno);
    }
    return ret;
}

int qp_full_connect(struct ibv_qp *qp, const struct rdma_endpoint *remote,
                    uint8_t port, int is_roce, int access_flags)
{
    int ret;

    /* RESET -> INIT */
    ret = qp_to_init(qp, port, access_flags);
    if (ret != 0) return ret;

    /* INIT -> RTR */
    ret = qp_to_rtr(qp, remote, port, is_roce);
    if (ret != 0) return ret;

    /* RTR -> RTS */
    ret = qp_to_rts(qp);
    if (ret != 0) return ret;

    return 0;
}

void print_qp_state(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;

    if (ibv_query_qp(qp, &attr, IBV_QP_STATE | IBV_QP_CUR_STATE, &init_attr) != 0) {
        fprintf(stderr, "[print_qp_state] ibv_query_qp failed\n");
        return;
    }

    printf("  QP #%u current state: %s (%d)\n",
           qp->qp_num, qp_state_str(attr.qp_state), attr.qp_state);
}

const char *qp_state_str(enum ibv_qp_state state)
{
    switch (state) {
    case IBV_QPS_RESET: return "RESET";
    case IBV_QPS_INIT:  return "INIT";
    case IBV_QPS_RTR:   return "RTR (Ready to Receive)";
    case IBV_QPS_RTS:   return "RTS (Ready to Send)";
    case IBV_QPS_SQD:   return "SQD (Send Queue Drained)";
    case IBV_QPS_SQE:   return "SQE (Send Queue Error)";
    case IBV_QPS_ERR:   return "ERR (Error)";
    default:            return "UNKNOWN";
    }
}

/* ========== TCP Out-of-Band Information Exchange ========== */

int exchange_endpoint_tcp(const char *server_ip, int tcp_port,
                          const struct rdma_endpoint *local,
                          struct rdma_endpoint *remote)
{
    int sock, ret = -1;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[exchange_endpoint_tcp] socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tcp_port);

    if (server_ip) {
        /* ---- Client mode: connect to server ---- */
        if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
            fprintf(stderr, "[exchange_endpoint_tcp] Invalid server IP: %s\n", server_ip);
            close(sock);
            return -1;
        }

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[exchange_endpoint_tcp] connect");
            close(sock);
            return -1;
        }

        /* Send first, then receive */
        if (send(sock, local, sizeof(*local), 0) != sizeof(*local)) {
            perror("[exchange_endpoint_tcp] send local");
            goto out;
        }
        if (recv(sock, remote, sizeof(*remote), MSG_WAITALL) != sizeof(*remote)) {
            perror("[exchange_endpoint_tcp] recv remote");
            goto out;
        }
    } else {
        /* ---- Server mode: listen and wait for connection ---- */
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[exchange_endpoint_tcp] bind");
            close(sock);
            return -1;
        }
        if (listen(sock, 1) < 0) {
            perror("[exchange_endpoint_tcp] listen");
            close(sock);
            return -1;
        }

        printf("[Info Exchange] Listening on port %d, waiting for client connection...\n", tcp_port);

        int client_fd = accept(sock, NULL, NULL);
        if (client_fd < 0) {
            perror("[exchange_endpoint_tcp] accept");
            close(sock);
            return -1;
        }

        /* Receive first, then send */
        if (recv(client_fd, remote, sizeof(*remote), MSG_WAITALL) != sizeof(*remote)) {
            perror("[exchange_endpoint_tcp] recv remote");
            close(client_fd);
            goto out;
        }
        if (send(client_fd, local, sizeof(*local), 0) != sizeof(*local)) {
            perror("[exchange_endpoint_tcp] send local");
            close(client_fd);
            goto out;
        }

        close(client_fd);
    }

    ret = 0;

out:
    close(sock);
    return ret;
}

/* ========== WC Completion Event Handling ========== */

const char *wc_opcode_str(enum ibv_wc_opcode opcode)
{
    switch (opcode) {
    case IBV_WC_SEND:             return "SEND";
    case IBV_WC_RDMA_WRITE:       return "RDMA_WRITE";
    case IBV_WC_RDMA_READ:        return "RDMA_READ";
    case IBV_WC_COMP_SWAP:        return "COMP_SWAP";
    case IBV_WC_FETCH_ADD:        return "FETCH_ADD";
    case IBV_WC_BIND_MW:          return "BIND_MW";
    case IBV_WC_RECV:             return "RECV";
    case IBV_WC_RECV_RDMA_WITH_IMM: return "RECV_RDMA_WITH_IMM";
    default:                      return "UNKNOWN";
    }
}

void print_wc_detail(const struct ibv_wc *wc)
{
    printf("=== Work Completion Details ===\n");
    printf("  wr_id:       %lu\n",   (unsigned long)wc->wr_id);
    printf("  status:      %s (%d)\n", ibv_wc_status_str(wc->status), wc->status);
    printf("  opcode:      %s (%d)\n", wc_opcode_str(wc->opcode), wc->opcode);
    printf("  byte_len:    %u\n",     wc->byte_len);
    printf("  qp_num:      %u\n",     wc->qp_num);
    printf("  src_qp:      %u\n",     wc->src_qp);
    printf("  vendor_err:  0x%x\n",   wc->vendor_err);

    /* If Immediate Data present */
    if (wc->wc_flags & IBV_WC_WITH_IMM) {
        printf("  imm_data:    0x%x (%u)\n",
               be32toh(wc->imm_data), be32toh(wc->imm_data));
    }

    printf("============================\n");
}

int poll_cq_blocking(struct ibv_cq *cq, struct ibv_wc *wc)
{
    int ne;

    while (1) {
        ne = ibv_poll_cq(cq, 1, wc);
        if (ne < 0) {
            fprintf(stderr, "[poll_cq_blocking] ibv_poll_cq failed\n");
            return -1;
        }
        if (ne > 0) {
            if (wc->status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[poll_cq_blocking] Completion event failed: %s (status=%d)\n",
                        ibv_wc_status_str(wc->status), wc->status);
                return -1;
            }
            return 0;
        }
        /* ne == 0: continue polling */
    }
}

/* ========== Endpoint Info Fill Helper ========== */

int fill_local_endpoint(struct ibv_context *ctx, struct ibv_qp *qp,
                        uint8_t port, int gid_index,
                        struct rdma_endpoint *ep)
{
    struct ibv_port_attr port_attr;

    memset(ep, 0, sizeof(*ep));

    if (ibv_query_port(ctx, port, &port_attr) != 0) {
        fprintf(stderr, "[fill_local_endpoint] Failed to query port %d\n", port);
        return -1;
    }

    ep->qp_num    = qp->qp_num;
    ep->lid       = port_attr.lid;
    ep->port_num  = port;
    ep->gid_index = gid_index;
    ep->psn       = RDMA_DEFAULT_PSN;

    /* Query local GID */
    if (ibv_query_gid(ctx, port, gid_index, &ep->gid) != 0) {
        fprintf(stderr, "[fill_local_endpoint] Failed to query GID[%d]\n", gid_index);
        /* Non-fatal error: GID may not be needed in IB mode */
    }

    return 0;
}
