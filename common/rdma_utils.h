/**
 * RDMA Common Utility Library - Header File
 *
 * Provides unified abstraction with IB/RoCE dual-mode support:
 *   - Automatic transport layer detection (IB vs RoCE)
 *   - QP state transitions (supports both GID/LID addressing)
 *   - TCP out-of-band information exchange
 *   - Device/port/GID query and printing
 *   - Error handling macros
 *
 * Build: first compile this library (make -C common), then link librdma_utils.a in other programs
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

/* ========== Constant Definitions ========== */

#define RDMA_DEFAULT_PORT_NUM   1       /* Default port number 1 */
#define RDMA_DEFAULT_GID_INDEX  0       /* Default GID index (RoCE v2 typically uses 1 or 3) */
#define RDMA_DEFAULT_PSN        0       /* Default Packet Sequence Number */
#define RDMA_DEFAULT_PKEY_INDEX 0       /* Default P_Key index */
#define RDMA_DEFAULT_SL         0       /* Default Service Level */
#define RDMA_DEFAULT_MTU        IBV_MTU_1024  /* Default MTU */

/* Transport layer type */
enum rdma_transport {
    RDMA_TRANSPORT_IB    = 0,   /* InfiniBand (uses LID addressing) */
    RDMA_TRANSPORT_ROCE  = 1,   /* RoCE (uses GID addressing, is_global=1) */
    RDMA_TRANSPORT_IWARP = 2,   /* iWARP */
    RDMA_TRANSPORT_UNKNOWN = -1,
};

/* ========== Core Data Structures ========== */

/**
 * RDMA endpoint information - all info needed for connection establishment
 *
 * Used for TCP out-of-band info exchange. IB mode uses lid addressing,
 * RoCE mode uses gid addressing.
 */
struct rdma_endpoint {
    uint32_t        qp_num;     /* QP number */
    uint16_t        lid;        /* Local port LID (IB mode) */
    uint8_t         gid_index;  /* GID table index (RoCE mode) */
    uint8_t         port_num;   /* Port number */
    union ibv_gid   gid;        /* Global Identifier (RoCE mode) */
    uint32_t        psn;        /* Packet Sequence Number */
    uint64_t        buf_addr;   /* Remote MR virtual address (needed for RDMA Read/Write) */
    uint32_t        buf_rkey;   /* Remote MR's rkey (needed for RDMA Read/Write) */
};

/* ========== Error Handling Macros ========== */

/**
 * CHECK_NULL - Check if pointer is NULL, print error and goto cleanup on failure
 *
 * Usage:
 *   pd = ibv_alloc_pd(ctx);
 *   CHECK_NULL(pd, "Failed to allocate Protection Domain");
 */
#define CHECK_NULL(ptr, msg) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[Error] %s: %s (errno=%d: %s)\n", \
                (msg), #ptr, errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

/**
 * CHECK_ERRNO - Check if return value is non-zero, print error and goto cleanup on failure
 *
 * Usage:
 *   ret = ibv_modify_qp(qp, &attr, mask);
 *   CHECK_ERRNO(ret, "QP INIT->RTR transition failed");
 */
#define CHECK_ERRNO(ret, msg) do { \
    if ((ret) != 0) { \
        fprintf(stderr, "[Error] %s: ret=%d (errno=%d: %s)\n", \
                (msg), (ret), errno, strerror(errno)); \
        goto cleanup; \
    } \
} while (0)

/**
 * CHECK_ERRNO_RETURN - Same as CHECK_ERRNO but returns the error code directly
 */
#define CHECK_ERRNO_RETURN(ret, msg) do { \
    if ((ret) != 0) { \
        fprintf(stderr, "[Error] %s: ret=%d (errno=%d: %s)\n", \
                (msg), (ret), errno, strerror(errno)); \
        return (ret); \
    } \
} while (0)

/* ========== Transport Layer Detection ========== */

/**
 * detect_transport - Detect transport layer type of the specified port
 *
 * @ctx:  Device context
 * @port: Port number (usually 1)
 *
 * Returns: RDMA_TRANSPORT_IB / RDMA_TRANSPORT_ROCE / RDMA_TRANSPORT_UNKNOWN
 *
 * Principle: Determined by the link_layer field from ibv_query_port()
 *   - IBV_LINK_LAYER_INFINIBAND -> IB
 *   - IBV_LINK_LAYER_ETHERNET   -> RoCE (or iWARP, requires further check)
 */
enum rdma_transport detect_transport(struct ibv_context *ctx, uint8_t port);

/**
 * transport_str - Convert transport layer type to readable string
 */
const char *transport_str(enum rdma_transport t);

/* ========== Device Query and Printing ========== */

/**
 * query_and_print_device - Query and print all device capability parameters
 *
 * Uses ibv_query_device() to print: fw_ver, max_qp, max_cq, max_mr,
 * max_mr_size, max_sge, max_qp_wr, max_cqe, atomic_cap, etc. (~20 key fields).
 */
int query_and_print_device(struct ibv_context *ctx);

/**
 * query_and_print_port - Query and print all port attributes
 *
 * Uses ibv_query_port() to print: state, max_mtu, active_mtu, lid,
 * sm_lid, gid_tbl_len, pkey_tbl_len, link_layer, etc.
 */
int query_and_print_port(struct ibv_context *ctx, uint8_t port);

/**
 * query_and_print_all_gids - Enumerate and print all GID entries for a port
 */
int query_and_print_all_gids(struct ibv_context *ctx, uint8_t port);

/**
 * print_gid - Format and print a GID (128-bit)
 *
 * Output format: fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx
 */
void print_gid(const union ibv_gid *gid);

/**
 * gid_to_str - Convert GID to string (IPv6 format)
 *
 * @gid: Input GID
 * @buf: Output buffer, at least 46 bytes
 */
void gid_to_str(const union ibv_gid *gid, char *buf, size_t buflen);

/* ========== QP State Transitions ========== */

/**
 * qp_to_init - Transition QP from RESET to INIT
 *
 * @qp:           Queue Pair
 * @port:         Port number
 * @access_flags: Remote access permissions (IBV_ACCESS_* combination)
 */
int qp_to_init(struct ibv_qp *qp, uint8_t port, int access_flags);

/**
 * qp_to_rtr - Transition QP from INIT to RTR (Ready to Receive)
 *
 * Automatically selects addressing mode based on is_roce parameter:
 *   - IB:   Uses remote->lid to set ah_attr.dlid
 *   - RoCE: Uses remote->gid to set ah_attr.is_global=1 + grh.dgid
 *
 * @qp:      Queue Pair
 * @remote:  Remote endpoint information
 * @port:    Local port number
 * @is_roce: Whether in RoCE mode
 */
int qp_to_rtr(struct ibv_qp *qp, const struct rdma_endpoint *remote,
              uint8_t port, int is_roce);

/**
 * qp_to_rts - Transition QP from RTR to RTS (Ready to Send)
 */
int qp_to_rts(struct ibv_qp *qp);

/**
 * qp_to_reset - Reset QP to RESET state (for error recovery)
 */
int qp_to_reset(struct ibv_qp *qp);

/**
 * qp_full_connect - One-call complete RESET->INIT->RTR->RTS transition
 *
 * @qp:           Queue Pair
 * @remote:       Remote endpoint information
 * @port:         Local port number
 * @is_roce:      Whether in RoCE mode
 * @access_flags: Remote access permissions
 */
int qp_full_connect(struct ibv_qp *qp, const struct rdma_endpoint *remote,
                    uint8_t port, int is_roce, int access_flags);

/**
 * print_qp_state - Query and print QP's current state
 */
void print_qp_state(struct ibv_qp *qp);

/**
 * qp_state_str - Convert QP state enum to readable string
 */
const char *qp_state_str(enum ibv_qp_state state);

/* ========== TCP Out-of-Band Information Exchange ========== */

/**
 * exchange_endpoint_tcp - Exchange RDMA endpoint info via TCP socket
 *
 * @server_ip: Server IP. NULL means this side acts as server (listen).
 * @tcp_port:  TCP port number
 * @local:     Local endpoint info (input)
 * @remote:    Remote endpoint info (output)
 *
 * Returns: 0 on success, -1 on failure
 */
int exchange_endpoint_tcp(const char *server_ip, int tcp_port,
                          const struct rdma_endpoint *local,
                          struct rdma_endpoint *remote);

/* ========== WC Completion Event Handling ========== */

/**
 * print_wc_detail - Print all fields of a Work Completion
 *
 * Includes: wr_id, status (text description), opcode, byte_len, qp_num,
 *           src_qp, imm_data (if present), vendor_err
 */
void print_wc_detail(const struct ibv_wc *wc);

/**
 * wc_opcode_str - Convert WC opcode to readable string
 */
const char *wc_opcode_str(enum ibv_wc_opcode opcode);

/**
 * poll_cq_blocking - Block-poll CQ until one completion event is received
 *
 * @cq:  Completion Queue
 * @wc:  Output completion event
 *
 * Returns: 0 on success, -1 on failure (with error message printed)
 */
int poll_cq_blocking(struct ibv_cq *cq, struct ibv_wc *wc);

/* ========== Endpoint Info Fill Helper ========== */

/**
 * fill_local_endpoint - Fill local endpoint information
 *
 * @ctx:      Device context
 * @qp:       Local QP
 * @port:     Port number
 * @gid_index: GID index
 * @ep:       Output endpoint info
 */
int fill_local_endpoint(struct ibv_context *ctx, struct ibv_qp *qp,
                        uint8_t port, int gid_index,
                        struct rdma_endpoint *ep);

#endif /* RDMA_UTILS_H */
