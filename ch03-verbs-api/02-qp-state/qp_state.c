/**
 * QP 状态转换示例 (增强版 - 支持 IB/RoCE 双模)
 *
 * 演示如何将 QP 从 RESET -> INIT -> RTR -> RTS
 * 自动检测传输类型 (IB 使用 LID, RoCE 使用 GID)
 * 每次状态转换后调用 ibv_query_qp() 打印完整 QP 属性
 *
 * 编译: gcc -o qp_state qp_state.c -I../../common ../../common/librdma_utils.a -libverbs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

#define PORT_NUM RDMA_DEFAULT_PORT_NUM

/* 资源结构 */
struct rdma_dev {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
};

/**
 * 查询并打印 QP 的详细属性 (增强版)
 *
 * 在每次状态转换后调用, 展示 QP 的完整参数变化。
 * 根据当前 QP 状态选择合适的查询掩码。
 */
void query_and_print_qp_full(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init_attr;

    /* 根据 QP 状态, 查询尽可能多的属性 */
    int mask = IBV_QP_STATE | IBV_QP_CUR_STATE | IBV_QP_PKEY_INDEX |
               IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PATH_MTU |
               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_SQ_PSN |
               IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
               IBV_QP_MAX_QP_RD_ATOMIC | IBV_QP_MAX_DEST_RD_ATOMIC |
               IBV_QP_MIN_RNR_TIMER | IBV_QP_AV;

    memset(&attr, 0, sizeof(attr));
    memset(&init_attr, 0, sizeof(init_attr));

    if (ibv_query_qp(qp, &attr, mask, &init_attr) != 0) {
        /* 某些属性在低状态下无法查询, 用最小掩码重试 */
        mask = IBV_QP_STATE | IBV_QP_CUR_STATE;
        memset(&attr, 0, sizeof(attr));
        if (ibv_query_qp(qp, &attr, mask, &init_attr) != 0) {
            fprintf(stderr, "  [错误] ibv_query_qp 失败\n");
            return;
        }
    }

    printf("=== QP #%u 完整属性 ===\n", qp->qp_num);
    printf("  QP 编号:                   %u\n", qp->qp_num);
    printf("  QP 类型:                   %s\n",
           qp->qp_type == IBV_QPT_RC ? "RC" :
           qp->qp_type == IBV_QPT_UC ? "UC" :
           qp->qp_type == IBV_QPT_UD ? "UD" : "Other");
    printf("  状态 (qp_state):           %s (%d)\n",
           qp_state_str(attr.qp_state), attr.qp_state);
    printf("  当前状态 (cur_qp_state):   %s (%d)\n",
           qp_state_str(attr.cur_qp_state), attr.cur_qp_state);

    /* INIT 及以上状态的属性 */
    if (attr.qp_state >= IBV_QPS_INIT) {
        printf("  端口号 (port_num):         %d\n", attr.port_num);
        printf("  PKey 索引:                 %d\n", attr.pkey_index);
        printf("  访问标志 (access_flags):   0x%x", attr.qp_access_flags);
        if (attr.qp_access_flags & IBV_ACCESS_LOCAL_WRITE)   printf(" LOCAL_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_READ)   printf(" REMOTE_READ");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_WRITE)  printf(" REMOTE_WRITE");
        if (attr.qp_access_flags & IBV_ACCESS_REMOTE_ATOMIC) printf(" REMOTE_ATOMIC");
        printf("\n");
    }

    /* RTR 及以上状态的属性 */
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
        printf("  目标 QP 号 (dest_qp_num): %u\n", attr.dest_qp_num);
        printf("  RQ PSN:                    %u\n", attr.rq_psn);
        printf("  最大目标 RD 原子数:        %d\n", attr.max_dest_rd_atomic);
        printf("  最小 RNR 定时器:           %d\n", attr.min_rnr_timer);

        /* 地址向量 (AH) 信息 */
        printf("  --- 地址向量 (AH Attr) ---\n");
        printf("    DLID:                    %u\n", attr.ah_attr.dlid);
        printf("    SL:                      %d\n", attr.ah_attr.sl);
        printf("    端口号:                  %d\n", attr.ah_attr.port_num);
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

    /* RTS 及以上状态的属性 */
    if (attr.qp_state >= IBV_QPS_RTS) {
        printf("  SQ PSN:                    %u\n", attr.sq_psn);
        printf("  超时 (timeout):            %d\n", attr.timeout);
        printf("  重试次数 (retry_cnt):      %d\n", attr.retry_cnt);
        printf("  RNR 重试 (rnr_retry):      %d\n", attr.rnr_retry);
        printf("  最大 RD 原子数:            %d\n", attr.max_rd_atomic);
    }

    /* QP 初始属性 */
    printf("  --- QP Init Attr ---\n");
    printf("    max_send_wr:             %d\n", init_attr.cap.max_send_wr);
    printf("    max_recv_wr:             %d\n", init_attr.cap.max_recv_wr);
    printf("    max_send_sge:            %d\n", init_attr.cap.max_send_sge);
    printf("    max_recv_sge:            %d\n", init_attr.cap.max_recv_sge);
    printf("    max_inline_data:         %d\n", init_attr.cap.max_inline_data);
    printf("=============================\n");
}

/* 获取端口信息 (保留原版, 增加 GID 打印) */
void print_port_info(struct ibv_context *context, uint8_t port)
{
    struct ibv_port_attr attr;

    if (ibv_query_port(context, port, &attr) == 0) {
        printf("=== 端口 %d 信息 ===\n", port);
        printf("链路层: %s\n",
               attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" : "Ethernet (RoCE)");
        printf("状态: %s\n",
               attr.state == IBV_PORT_ACTIVE ? "ACTIVE" : "非 ACTIVE");
        printf("LID: %u\n", attr.lid);
        printf("SM LID: %u\n", attr.sm_lid);
        printf("GID 表大小: %d\n", attr.gid_tbl_len);

        /* 打印第一个非零 GID */
        union ibv_gid gid;
        if (ibv_query_gid(context, port, 0, &gid) == 0) {
            char gid_str[46];
            gid_to_str(&gid, gid_str, sizeof(gid_str));
            printf("GID[0]: %s\n", gid_str);
        }
        printf("===================\n");
    }
}

/* RESET -> INIT (与之前一致) */
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
        perror("QP RESET->INIT 失败");
    }
    return ret;
}

/*
 * INIT -> RTR (增强版 - 自动检测 IB/RoCE)
 *
 * 通过 port 的 link_layer 自动选择寻址方式:
 *   - IB:   使用 remote_lid (LID 寻址)
 *   - RoCE: 使用 remote_gid (GID 寻址, is_global=1)
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

    /* 地址向量: 根据传输类型选择寻址方式 */
    attr.ah_attr.sl       = 0;
    attr.ah_attr.port_num = PORT_NUM;

    if (is_roce) {
        /*
         * RoCE 模式: 必须设置 is_global=1 并填充 GRH
         * dgid = 对端的 GID (loopback 时就是本机 GID)
         * sgid_index = 本地 GID 表中的索引
         */
        printf("  [RoCE 模式] 使用 GID 寻址 (is_global=1)\n");
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = *remote_gid;
        attr.ah_attr.grh.sgid_index = gid_index;
        attr.ah_attr.grh.hop_limit  = 64;
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.traffic_class = 0;
        attr.ah_attr.dlid = 0;     /* RoCE 下 dlid 不用 */

        char gid_str[46];
        gid_to_str(remote_gid, gid_str, sizeof(gid_str));
        printf("  dgid = %s, sgid_index = %d\n", gid_str, gid_index);
    } else {
        /*
         * IB 模式: 使用 LID 寻址, is_global=0
         */
        printf("  [IB 模式] 使用 LID 寻址 (dlid=%u)\n", remote_lid);
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid      = remote_lid;
    }

    int ret = ibv_modify_qp(qp, &attr,
                             IBV_QP_STATE | IBV_QP_PATH_MTU |
                             IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                             IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                             IBV_QP_AV);
    if (ret) {
        perror("QP INIT->RTR 失败");
        if (is_roce) {
            fprintf(stderr, "  提示: RoCE 模式下检查 GID 是否正确, gid_index 是否有效\n");
        }
    }
    return ret;
}

/* RTR -> RTS (与之前一致) */
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
        perror("QP RTR->RTS 失败");
    }
    return ret;
}

int main(int argc, char *argv[])
{
    struct rdma_dev dev = {0};
    struct ibv_device **list;
    int num;

    printf("=== QP 状态转换示例 (IB/RoCE 双模支持) ===\n");
    printf("=============================================\n\n");

    /* 获取设备 */
    list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "未发现 RDMA 设备\n");
        return 1;
    }

    /* 打开设备 */
    dev.context = ibv_open_device(list[0]);
    if (!dev.context) {
        perror("打开设备失败");
        return 1;
    }
    printf("设备: %s\n\n", ibv_get_device_name(list[0]));

    /* 打印端口信息 */
    print_port_info(dev.context, PORT_NUM);

    /* 自动检测传输类型 */
    enum rdma_transport transport = detect_transport(dev.context, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("\n检测到传输类型: %s\n", transport_str(transport));
    if (is_roce) {
        printf("→ 将使用 GID 寻址 (RoCE 模式)\n\n");
    } else {
        printf("→ 将使用 LID 寻址 (IB 模式)\n\n");
    }

    /* 分配PD */
    dev.pd = ibv_alloc_pd(dev.context);
    if (!dev.pd) {
        perror("分配 PD 失败");
        return 1;
    }

    /* 创建CQ */
    dev.cq = ibv_create_cq(dev.context, 128, NULL, NULL, 0);
    if (!dev.cq) {
        perror("创建 CQ 失败");
        return 1;
    }

    /* 创建QP */
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
        perror("创建 QP 失败");
        return 1;
    }

    /* 打印初始 QP 状态 (RESET) */
    printf("--- 初始状态 (创建后) ---\n");
    query_and_print_qp_full(dev.qp);

    /* ========== 状态转换 1: RESET -> INIT ========== */
    printf("\n[步骤1] RESET -> INIT\n");
    if (qp_reset_to_init(dev.qp)) {
        return 1;
    }
    printf("  转换成功!\n");
    query_and_print_qp_full(dev.qp);

    /* ========== 准备 loopback 参数 ========== */
    /*
     * 获取本机端口信息用于 loopback 测试。
     * IB 模式: 需要 LID
     * RoCE 模式: 需要 GID
     * 实际生产中需要通过 TCP socket 或 RDMA CM 与对端交换这些信息。
     */
    struct ibv_port_attr port_attr;
    if (ibv_query_port(dev.context, PORT_NUM, &port_attr) != 0) {
        perror("查询端口失败");
        return 1;
    }
    uint16_t local_lid = port_attr.lid;
    uint32_t local_qp_num = dev.qp->qp_num;

    /* 查询本地 GID (RoCE 模式需要) */
    union ibv_gid local_gid;
    int gid_index = RDMA_DEFAULT_GID_INDEX;
    if (ibv_query_gid(dev.context, PORT_NUM, gid_index, &local_gid) != 0) {
        fprintf(stderr, "查询 GID[%d] 失败\n", gid_index);
        /* IB 模式下可继续, RoCE 模式下将失败 */
    } else {
        char gid_str[46];
        gid_to_str(&local_gid, gid_str, sizeof(gid_str));
        printf("\nLoopback 参数:\n");
        printf("  local_qp_num = %u\n", local_qp_num);
        printf("  local_lid    = %u\n", local_lid);
        printf("  local_gid[%d] = %s\n\n", gid_index, gid_str);
    }

    /* ========== 状态转换 2: INIT -> RTR ========== */
    printf("[步骤2] INIT -> RTR (loopback: remote_qp=%u)\n", local_qp_num);
    if (qp_init_to_rtr(dev.qp, dev.context, local_qp_num, local_lid,
                        &local_gid, gid_index, is_roce)) {
        return 1;
    }
    printf("  转换成功!\n");
    query_and_print_qp_full(dev.qp);

    /* ========== 状态转换 3: RTR -> RTS ========== */
    printf("\n[步骤3] RTR -> RTS\n");
    if (qp_rtr_to_rts(dev.qp)) {
        return 1;
    }
    printf("  转换成功!\n");
    query_and_print_qp_full(dev.qp);

    printf("\n=== QP 状态机完成: RESET -> INIT -> RTR -> RTS ===\n");
    printf("QP 已准备就绪, 可以进行数据传输。\n");
    printf("传输模式: %s\n\n", is_roce ? "RoCE (GID 寻址)" : "IB (LID 寻址)");

    /* 清理 */
    printf("[清理] 释放资源...\n");
    ibv_destroy_qp(dev.qp);
    ibv_destroy_cq(dev.cq);
    ibv_dealloc_pd(dev.pd);
    ibv_close_device(dev.context);
    ibv_free_device_list(list);
    printf("  完成\n");

    return 0;
}
