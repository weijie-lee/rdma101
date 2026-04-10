/**
 * RDMA 公共工具库 - 头文件
 *
 * 提供 IB/RoCE 双模支持的统一抽象：
 *   - 传输层自动检测 (IB vs RoCE)
 *   - QP 状态转换 (支持 GID/LID 两种寻址)
 *   - TCP 带外信息交换
 *   - 设备/端口/GID 查询与打印
 *   - 错误处理宏
 *
 * 编译: 先编译本库 (make -C common)，再在其他程序中链接 librdma_utils.a
 */

#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

/* ========== 常量定义 ========== */

#define RDMA_DEFAULT_PORT_NUM   1       /* 默认使用端口号 1 */
#define RDMA_DEFAULT_GID_INDEX  0       /* 默认 GID 索引 (RoCE v2 通常用 1 或 3) */
#define RDMA_DEFAULT_PSN        0       /* 默认 Packet Sequence Number */
#define RDMA_DEFAULT_PKEY_INDEX 0       /* 默认 P_Key 索引 */
#define RDMA_DEFAULT_SL         0       /* 默认 Service Level */
#define RDMA_DEFAULT_MTU        IBV_MTU_1024  /* 默认 MTU */

/* 传输层类型 */
enum rdma_transport {
    RDMA_TRANSPORT_IB    = 0,   /* InfiniBand (使用 LID 寻址) */
    RDMA_TRANSPORT_ROCE  = 1,   /* RoCE (使用 GID 寻址, is_global=1) */
    RDMA_TRANSPORT_IWARP = 2,   /* iWARP */
    RDMA_TRANSPORT_UNKNOWN = -1,
};

/* ========== 核心数据结构 ========== */

/**
 * RDMA 端点信息 —— 建连时需要交换的所有信息
 *
 * 用于 TCP 带外信息交换。IB 模式用 lid 寻址，RoCE 模式用 gid 寻址。
 */
struct rdma_endpoint {
    uint32_t        qp_num;     /* QP 编号 */
    uint16_t        lid;        /* 本地端口 LID (IB 模式) */
    uint8_t         gid_index;  /* GID 表索引 (RoCE 模式) */
    uint8_t         port_num;   /* 端口号 */
    union ibv_gid   gid;        /* 全局标识符 (RoCE 模式) */
    uint32_t        psn;        /* Packet Sequence Number */
    uint64_t        buf_addr;   /* 远端 MR 虚拟地址 (RDMA Read/Write 需要) */
    uint32_t        buf_rkey;   /* 远端 MR 的 rkey (RDMA Read/Write 需要) */
};

/* ========== 错误处理宏 ========== */

/**
 * CHECK_NULL - 检查指针是否为 NULL，失败则打印错误信息并跳转 cleanup
 *
 * 用法:
 *   pd = ibv_alloc_pd(ctx);
 *   CHECK_NULL(pd, "分配保护域失败");
 */
#define CHECK_NULL(ptr, msg) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[错误] %s: %s (errno=%d: %s)\n", \
                (msg), #ptr, errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

/**
 * CHECK_ERRNO - 检查返回值是否非零，失败则打印错误信息并跳转 cleanup
 *
 * 用法:
 *   ret = ibv_modify_qp(qp, &attr, mask);
 *   CHECK_ERRNO(ret, "QP INIT->RTR 转换失败");
 */
#define CHECK_ERRNO(ret, msg) do { \
    if ((ret) != 0) { \
        fprintf(stderr, "[错误] %s: 返回值=%d (errno=%d: %s)\n", \
                (msg), (ret), errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

/**
 * CHECK_ERRNO_RETURN - 同 CHECK_ERRNO 但直接返回错误码
 */
#define CHECK_ERRNO_RETURN(ret, msg) do { \
    if ((ret) != 0) { \
        fprintf(stderr, "[错误] %s: 返回值=%d (errno=%d: %s)\n", \
                (msg), (ret), errno, strerror(errno)); \
        return (ret); \
    } \
} while (0)

/* ========== 传输层检测 ========== */

/**
 * detect_transport - 检测指定端口的传输层类型
 *
 * @ctx:  设备上下文
 * @port: 端口号 (通常为 1)
 *
 * 返回: RDMA_TRANSPORT_IB / RDMA_TRANSPORT_ROCE / RDMA_TRANSPORT_UNKNOWN
 *
 * 原理: 通过 ibv_query_port() 的 link_layer 字段判断
 *   - IBV_LINK_LAYER_INFINIBAND → IB
 *   - IBV_LINK_LAYER_ETHERNET   → RoCE (或 iWARP，需进一步判断)
 */
enum rdma_transport detect_transport(struct ibv_context *ctx, uint8_t port);

/**
 * transport_str - 将传输层类型转为可读字符串
 */
const char *transport_str(enum rdma_transport t);

/* ========== 设备查询与打印 ========== */

/**
 * query_and_print_device - 查询并打印设备的全部能力参数
 *
 * 使用 ibv_query_device() 打印: fw_ver, max_qp, max_cq, max_mr,
 * max_mr_size, max_sge, max_qp_wr, max_cqe, atomic_cap 等 ~20 个关键字段。
 */
int query_and_print_device(struct ibv_context *ctx);

/**
 * query_and_print_port - 查询并打印端口的全部属性
 *
 * 使用 ibv_query_port() 打印: state, max_mtu, active_mtu, lid,
 * sm_lid, gid_tbl_len, pkey_tbl_len, link_layer 等字段。
 */
int query_and_print_port(struct ibv_context *ctx, uint8_t port);

/**
 * query_and_print_all_gids - 遍历并打印指定端口的所有 GID 条目
 */
int query_and_print_all_gids(struct ibv_context *ctx, uint8_t port);

/**
 * print_gid - 格式化打印一个 GID (128-bit)
 *
 * 输出格式: fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx
 */
void print_gid(const union ibv_gid *gid);

/**
 * gid_to_str - 将 GID 转为字符串 (IPv6 格式)
 *
 * @gid: 输入 GID
 * @buf: 输出缓冲区，至少 46 字节
 */
void gid_to_str(const union ibv_gid *gid, char *buf, size_t buflen);

/* ========== QP 状态转换 ========== */

/**
 * qp_to_init - 将 QP 从 RESET 转到 INIT
 *
 * @qp:           队列对
 * @port:         端口号
 * @access_flags: 远端访问权限 (IBV_ACCESS_* 组合)
 */
int qp_to_init(struct ibv_qp *qp, uint8_t port, int access_flags);

/**
 * qp_to_rtr - 将 QP 从 INIT 转到 RTR (Ready to Receive)
 *
 * 自动根据 is_roce 参数选择寻址方式:
 *   - IB:   使用 remote->lid 设置 ah_attr.dlid
 *   - RoCE: 使用 remote->gid 设置 ah_attr.is_global=1 + grh.dgid
 *
 * @qp:      队列对
 * @remote:  对端端点信息
 * @port:    本地端口号
 * @is_roce: 是否为 RoCE 模式
 */
int qp_to_rtr(struct ibv_qp *qp, const struct rdma_endpoint *remote,
              uint8_t port, int is_roce);

/**
 * qp_to_rts - 将 QP 从 RTR 转到 RTS (Ready to Send)
 */
int qp_to_rts(struct ibv_qp *qp);

/**
 * qp_to_reset - 将 QP 重置到 RESET 状态 (用于错误恢复)
 */
int qp_to_reset(struct ibv_qp *qp);

/**
 * qp_full_connect - 一键完成 RESET→INIT→RTR→RTS 全部转换
 *
 * @qp:           队列对
 * @remote:       对端端点信息
 * @port:         本地端口号
 * @is_roce:      是否为 RoCE 模式
 * @access_flags: 远端访问权限
 */
int qp_full_connect(struct ibv_qp *qp, const struct rdma_endpoint *remote,
                    uint8_t port, int is_roce, int access_flags);

/**
 * print_qp_state - 查询并打印 QP 的当前状态
 */
void print_qp_state(struct ibv_qp *qp);

/**
 * qp_state_str - 将 QP 状态枚举值转为可读字符串
 */
const char *qp_state_str(enum ibv_qp_state state);

/* ========== TCP 带外信息交换 ========== */

/**
 * exchange_endpoint_tcp - 通过 TCP socket 交换 RDMA 端点信息
 *
 * @server_ip: 服务器 IP。NULL 表示本端作为服务器监听。
 * @tcp_port:  TCP 端口号
 * @local:     本地端点信息 (输入)
 * @remote:    对端端点信息 (输出)
 *
 * 返回: 0 成功, -1 失败
 */
int exchange_endpoint_tcp(const char *server_ip, int tcp_port,
                          const struct rdma_endpoint *local,
                          struct rdma_endpoint *remote);

/* ========== WC 完成事件处理 ========== */

/**
 * print_wc_detail - 打印 Work Completion 的完整字段信息
 *
 * 包括: wr_id, status (文字描述), opcode, byte_len, qp_num,
 *       src_qp, imm_data (如有), vendor_err
 */
void print_wc_detail(const struct ibv_wc *wc);

/**
 * wc_opcode_str - 将 WC opcode 转为可读字符串
 */
const char *wc_opcode_str(enum ibv_wc_opcode opcode);

/**
 * poll_cq_blocking - 阻塞轮询 CQ 直到收到一个完成事件
 *
 * @cq:  完成队列
 * @wc:  输出的完成事件
 *
 * 返回: 0 成功, -1 失败 (含打印错误信息)
 */
int poll_cq_blocking(struct ibv_cq *cq, struct ibv_wc *wc);

/* ========== 端点信息填充辅助 ========== */

/**
 * fill_local_endpoint - 填充本地端点信息
 *
 * @ctx:      设备上下文
 * @qp:       本地 QP
 * @port:     端口号
 * @gid_index: GID 索引
 * @ep:       输出的端点信息
 */
int fill_local_endpoint(struct ibv_context *ctx, struct ibv_qp *qp,
                        uint8_t port, int gid_index,
                        struct rdma_endpoint *ep);

#endif /* RDMA_UTILS_H */
