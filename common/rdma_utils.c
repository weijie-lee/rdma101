/**
 * RDMA 公共工具库 - 实现文件
 *
 * 提供 IB/RoCE 双模支持的统一抽象函数实现。
 *
 * 编译: gcc -c rdma_utils.c -o rdma_utils.o -Wall -O2
 *       ar rcs librdma_utils.a rdma_utils.o
 */

#include "rdma_utils.h"

/* ========== 传输层检测 ========== */

enum rdma_transport detect_transport(struct ibv_context *ctx, uint8_t port)
{
    struct ibv_port_attr port_attr;

    if (ibv_query_port(ctx, port, &port_attr) != 0) {
        fprintf(stderr, "[detect_transport] 查询端口 %d 失败: %s\n",
                port, strerror(errno));
        return RDMA_TRANSPORT_UNKNOWN;
    }

    switch (port_attr.link_layer) {
    case IBV_LINK_LAYER_INFINIBAND:
        return RDMA_TRANSPORT_IB;
    case IBV_LINK_LAYER_ETHERNET:
        /*
         * 以太网链路层可能是 RoCE 或 iWARP。
         * 进一步通过设备的 transport_type 判断。
         * 注: ibv_get_device_name 对应的设备若 transport == IB 则为 RoCE，
         *     transport == IWARP 则为 iWARP。但在 verbs API 中,
         *     RoCE 设备的 node_type 是 IBV_NODE_CA, transport_type 是 IBV_TRANSPORT_IB。
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

/* ========== 设备查询与打印 ========== */

int query_and_print_device(struct ibv_context *ctx)
{
    struct ibv_device_attr dev_attr;

    if (ibv_query_device(ctx, &dev_attr) != 0) {
        fprintf(stderr, "[query_and_print_device] ibv_query_device 失败: %s\n",
                strerror(errno));
        return -1;
    }

    printf("=== 设备能力参数 ===\n");
    printf("  固件版本 (fw_ver):          %s\n",   dev_attr.fw_ver);
    printf("  节点 GUID (node_guid):      0x%016lx\n", (unsigned long)dev_attr.node_guid);
    printf("  系统镜像 GUID:              0x%016lx\n", (unsigned long)dev_attr.sys_image_guid);
    printf("  最大 MR 大小:               %lu bytes\n", (unsigned long)dev_attr.max_mr_size);
    printf("  页面大小掩码:               0x%lx\n", (unsigned long)dev_attr.page_size_cap);
    printf("  厂商 ID:                    0x%x\n",  dev_attr.vendor_id);
    printf("  厂商 Part ID:               %d\n",    dev_attr.vendor_part_id);
    printf("  硬件版本:                   %d\n",    dev_attr.hw_ver);
    printf("  物理端口数:                 %d\n",    dev_attr.phys_port_cnt);
    printf("  --- 队列和完成队列 ---\n");
    printf("  最大 QP 数量 (max_qp):      %d\n",    dev_attr.max_qp);
    printf("  每个 QP 最大 WR (max_qp_wr): %d\n",   dev_attr.max_qp_wr);
    printf("  每个 WR 最大 SGE (max_sge):  %d\n",   dev_attr.max_sge);
    printf("  最大 SRQ 数量:              %d\n",    dev_attr.max_srq);
    printf("  每个 SRQ 最大 WR:           %d\n",    dev_attr.max_srq_wr);
    printf("  最大 CQ 数量 (max_cq):      %d\n",    dev_attr.max_cq);
    printf("  每个 CQ 最大 CQE (max_cqe): %d\n",    dev_attr.max_cqe);
    printf("  --- 内存区域 ---\n");
    printf("  最大 MR 数量 (max_mr):      %d\n",    dev_attr.max_mr);
    printf("  最大 PD 数量 (max_pd):      %d\n",    dev_attr.max_pd);
    printf("  --- 原子操作 ---\n");
    printf("  原子操作能力 (atomic_cap):  ");
    switch (dev_attr.atomic_cap) {
    case IBV_ATOMIC_NONE:  printf("NONE (不支持)\n"); break;
    case IBV_ATOMIC_HCA:   printf("HCA (仅本 HCA 内原子)\n"); break;
    case IBV_ATOMIC_GLOB:  printf("GLOBAL (全局原子)\n"); break;
    default:               printf("%d (未知)\n", dev_attr.atomic_cap); break;
    }
    printf("  最大 QP RD 原子操作数:      %d\n",    dev_attr.max_qp_rd_atom);
    printf("  最大 QP INIT RD 原子操作数: %d\n",    dev_attr.max_qp_init_rd_atom);
    printf("  --- 多播 ---\n");
    printf("  最大多播组数:               %d\n",    dev_attr.max_mcast_grp);
    printf("  每个多播组最大 QP 数:       %d\n",    dev_attr.max_mcast_qp_attach);
    printf("  最大 AH 数量:               %d\n",    dev_attr.max_ah);
    printf("========================\n");

    return 0;
}

int query_and_print_port(struct ibv_context *ctx, uint8_t port)
{
    struct ibv_port_attr attr;

    if (ibv_query_port(ctx, port, &attr) != 0) {
        fprintf(stderr, "[query_and_print_port] 查询端口 %d 失败: %s\n",
                port, strerror(errno));
        return -1;
    }

    printf("=== 端口 %d 属性 ===\n", port);

    /* 端口状态 */
    printf("  状态 (state):     ");
    switch (attr.state) {
    case IBV_PORT_NOP:          printf("NOP (未激活)\n"); break;
    case IBV_PORT_DOWN:         printf("DOWN (链路断开)\n"); break;
    case IBV_PORT_INIT:         printf("INIT (初始化中)\n"); break;
    case IBV_PORT_ARMED:        printf("ARMED (准备就绪)\n"); break;
    case IBV_PORT_ACTIVE:       printf("ACTIVE (活动)\n"); break;
    case IBV_PORT_ACTIVE_DEFER: printf("ACTIVE_DEFER\n"); break;
    default:                    printf("%d (未知)\n", attr.state); break;
    }

    /* MTU */
    printf("  最大 MTU (max_mtu):    ");
    switch (attr.max_mtu) {
    case IBV_MTU_256:  printf("256\n"); break;
    case IBV_MTU_512:  printf("512\n"); break;
    case IBV_MTU_1024: printf("1024\n"); break;
    case IBV_MTU_2048: printf("2048\n"); break;
    case IBV_MTU_4096: printf("4096\n"); break;
    default:           printf("%d\n", attr.max_mtu); break;
    }

    printf("  活动 MTU (active_mtu): ");
    switch (attr.active_mtu) {
    case IBV_MTU_256:  printf("256\n"); break;
    case IBV_MTU_512:  printf("512\n"); break;
    case IBV_MTU_1024: printf("1024\n"); break;
    case IBV_MTU_2048: printf("2048\n"); break;
    case IBV_MTU_4096: printf("4096\n"); break;
    default:           printf("%d\n", attr.active_mtu); break;
    }

    /* 链路层 */
    printf("  链路层 (link_layer): ");
    switch (attr.link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED:  printf("未指定\n"); break;
    case IBV_LINK_LAYER_INFINIBAND:   printf("InfiniBand\n"); break;
    case IBV_LINK_LAYER_ETHERNET:     printf("Ethernet (RoCE)\n"); break;
    default:                          printf("%d (未知)\n", attr.link_layer); break;
    }

    /* 地址信息 */
    printf("  LID:                   %u", attr.lid);
    if (attr.lid == 0) {
        printf("  (LID=0 表示 RoCE 模式，需使用 GID 寻址)");
    }
    printf("\n");
    printf("  SM LID:                %u\n",   attr.sm_lid);
    printf("  GID 表大小:            %d\n",    attr.gid_tbl_len);
    printf("  P_Key 表大小:          %u\n",   attr.pkey_tbl_len);

    /* 速率 */
    printf("  活动速率:              %u Gbps (width=%d, speed=%d)\n",
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
        fprintf(stderr, "[query_and_print_all_gids] 查询端口 %d 失败\n", port);
        return -1;
    }

    printf("=== 端口 %d GID 表 (共 %d 个条目) ===\n", port, port_attr.gid_tbl_len);

    for (i = 0; i < port_attr.gid_tbl_len; i++) {
        if (ibv_query_gid(ctx, port, i, &gid) != 0) {
            continue;
        }

        /* 跳过全零 GID (未配置的条目) */
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

/* ========== QP 状态转换 ========== */

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
        fprintf(stderr, "[qp_to_init] RESET→INIT 失败: %s (errno=%d)\n",
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
    attr.min_rnr_timer      = 12;  /* 12 ≈ 0.01ms，合理的 RNR NAK 超时 */

    /* 地址向量 (Address Vector) */
    attr.ah_attr.sl         = RDMA_DEFAULT_SL;
    attr.ah_attr.port_num   = port;

    if (is_roce) {
        /*
         * RoCE 模式: 必须设置 is_global=1，并填充 GRH (Global Route Header)。
         * dgid 填对端的 GID，sgid_index 填本地 GID 表中的索引。
         */
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = remote->gid;
        attr.ah_attr.grh.sgid_index = remote->gid_index;
        attr.ah_attr.grh.hop_limit  = 64;  /* 允许跨路由器 */
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.traffic_class = 0;
        /* RoCE 模式下 dlid 可设为 0 或任意值，不影响寻址 */
        attr.ah_attr.dlid = 0;
    } else {
        /*
         * IB 模式: 使用 LID 寻址，is_global=0 (子网内通信不需要 GRH)。
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
        fprintf(stderr, "[qp_to_rtr] INIT→RTR 失败: %s (errno=%d)\n",
                strerror(errno), errno);
        if (is_roce) {
            char gid_str[46];
            gid_to_str(&remote->gid, gid_str, sizeof(gid_str));
            fprintf(stderr, "  提示: RoCE 模式下请检查 GID 是否正确: %s\n", gid_str);
        } else {
            fprintf(stderr, "  提示: IB 模式下请检查 LID 是否正确: %u\n", remote->lid);
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
    attr.timeout        = 14;   /* 约 8.4 秒 (4.096us * 2^14) */
    attr.retry_cnt      = 7;    /* 最大重传次数 */
    attr.rnr_retry      = 7;    /* RNR NAK 最大重试次数 (7 = 无限重试) */
    attr.max_rd_atomic  = 1;    /* 未完成的 RDMA Read/Atomic 操作数 */

    int ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_SQ_PSN |
                            IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret != 0) {
        fprintf(stderr, "[qp_to_rts] RTR→RTS 失败: %s (errno=%d)\n",
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
        fprintf(stderr, "[qp_to_reset] →RESET 失败: %s (errno=%d)\n",
                strerror(errno), errno);
    }
    return ret;
}

int qp_full_connect(struct ibv_qp *qp, const struct rdma_endpoint *remote,
                    uint8_t port, int is_roce, int access_flags)
{
    int ret;

    /* RESET → INIT */
    ret = qp_to_init(qp, port, access_flags);
    if (ret != 0) return ret;

    /* INIT → RTR */
    ret = qp_to_rtr(qp, remote, port, is_roce);
    if (ret != 0) return ret;

    /* RTR → RTS */
    ret = qp_to_rts(qp);
    if (ret != 0) return ret;

    return 0;
}

void print_qp_state(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;

    if (ibv_query_qp(qp, &attr, IBV_QP_STATE | IBV_QP_CUR_STATE, &init_attr) != 0) {
        fprintf(stderr, "[print_qp_state] ibv_query_qp 失败\n");
        return;
    }

    printf("  QP #%u 当前状态: %s (%d)\n",
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

/* ========== TCP 带外信息交换 ========== */

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
        /* ---- 客户端模式：连接到服务器 ---- */
        if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
            fprintf(stderr, "[exchange_endpoint_tcp] 无效的服务器 IP: %s\n", server_ip);
            close(sock);
            return -1;
        }

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[exchange_endpoint_tcp] connect");
            close(sock);
            return -1;
        }

        /* 先发后收 */
        if (send(sock, local, sizeof(*local), 0) != sizeof(*local)) {
            perror("[exchange_endpoint_tcp] send local");
            goto out;
        }
        if (recv(sock, remote, sizeof(*remote), MSG_WAITALL) != sizeof(*remote)) {
            perror("[exchange_endpoint_tcp] recv remote");
            goto out;
        }
    } else {
        /* ---- 服务器模式：监听并等待连接 ---- */
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

        printf("[信息交换] 监听端口 %d，等待客户端连接...\n", tcp_port);

        int client_fd = accept(sock, NULL, NULL);
        if (client_fd < 0) {
            perror("[exchange_endpoint_tcp] accept");
            close(sock);
            return -1;
        }

        /* 先收后发 */
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

/* ========== WC 完成事件处理 ========== */

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
    printf("=== Work Completion 详情 ===\n");
    printf("  wr_id:       %lu\n",   (unsigned long)wc->wr_id);
    printf("  status:      %s (%d)\n", ibv_wc_status_str(wc->status), wc->status);
    printf("  opcode:      %s (%d)\n", wc_opcode_str(wc->opcode), wc->opcode);
    printf("  byte_len:    %u\n",     wc->byte_len);
    printf("  qp_num:      %u\n",     wc->qp_num);
    printf("  src_qp:      %u\n",     wc->src_qp);
    printf("  vendor_err:  0x%x\n",   wc->vendor_err);

    /* 如果有 Immediate Data */
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
            fprintf(stderr, "[poll_cq_blocking] ibv_poll_cq 失败\n");
            return -1;
        }
        if (ne > 0) {
            if (wc->status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[poll_cq_blocking] 完成事件失败: %s (status=%d)\n",
                        ibv_wc_status_str(wc->status), wc->status);
                return -1;
            }
            return 0;
        }
        /* ne == 0: 继续轮询 */
    }
}

/* ========== 端点信息填充辅助 ========== */

int fill_local_endpoint(struct ibv_context *ctx, struct ibv_qp *qp,
                        uint8_t port, int gid_index,
                        struct rdma_endpoint *ep)
{
    struct ibv_port_attr port_attr;

    memset(ep, 0, sizeof(*ep));

    if (ibv_query_port(ctx, port, &port_attr) != 0) {
        fprintf(stderr, "[fill_local_endpoint] 查询端口 %d 失败\n", port);
        return -1;
    }

    ep->qp_num    = qp->qp_num;
    ep->lid       = port_attr.lid;
    ep->port_num  = port;
    ep->gid_index = gid_index;
    ep->psn       = RDMA_DEFAULT_PSN;

    /* 查询本地 GID */
    if (ibv_query_gid(ctx, port, gid_index, &ep->gid) != 0) {
        fprintf(stderr, "[fill_local_endpoint] 查询 GID[%d] 失败\n", gid_index);
        /* 非致命错误: IB 模式下 GID 可能不需要 */
    }

    return 0;
}
