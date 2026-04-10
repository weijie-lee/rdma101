/**
 * RDMA Write Example - Client pushes data to server
 *
 * Supports both InfiniBand and RoCE: automatically detects transport layer type
 *
 * Flow:
 * 1. Server: start, create resources, wait for connection
 * 2. Client: connect to Server, exchange MR info (including GID), perform Write
 * 3. Server: verify data
 *
 * Build: gcc -o rdma_write rdma_write.c -I../../common ../../common/librdma_utils.a -libverbs
 * Run:
 *   Terminal 1: ./rdma_write server
 *   Terminal 2: ./rdma_write client 127.0.0.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

/* Import common utility library for IB/RoCE auto-detection capability */
#include "../../common/rdma_utils.h"

#define BUFFER_SIZE 1024
#define MSG_SIZE 256
#define TCP_PORT 9876

/* RDMA resource structure */
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buffer;
    uint16_t lid;           /* Local port LID (IB mode) */
    union ibv_gid gid;      /* Local GID (RoCE mode) */
    int is_roce;            /* Whether in RoCE mode */
};

/* Connection info for TCP exchange (extended to support GID) */
struct connection_info {
    uint32_t qp_num;
    uint16_t lid;
    union ibv_gid gid;      /* GID used for addressing in RoCE mode */
    uint64_t buf_addr;       /* Virtual address of Server MR (for client to write to) */
    uint32_t buf_rkey;       /* rkey of Server MR */
};

/* Initialize RDMA resources */
int init_rdma_resources(struct rdma_resources *res)
{
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_port_attr port_attr;
    int num_devices;

    /* Get device */
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return -1;
    }
    device = device_list[0];

    /* Open device */
    res->context = ibv_open_device(device);
    if (!res->context) {
        fprintf(stderr, "Failed to open device\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* Query local LID */
    if (ibv_query_port(res->context, 1, &port_attr) != 0) {
        fprintf(stderr, "Failed to query port\n");
        ibv_free_device_list(device_list);
        return -1;
    }
    res->lid = port_attr.lid;

    /* Detect transport layer type: IB or RoCE */
    enum rdma_transport transport = detect_transport(res->context, 1);
    res->is_roce = (transport == RDMA_TRANSPORT_ROCE);
    printf("Transport layer type: %s\n", transport_str(transport));

    /* Query GID in RoCE mode */
    if (res->is_roce) {
        if (ibv_query_gid(res->context, 1, RDMA_DEFAULT_GID_INDEX, &res->gid) != 0) {
            fprintf(stderr, "Failed to query GID\n");
        }
    }

    /* Allocate PD */
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* Create CQ */
    res->cq = ibv_create_cq(res->context, 128, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* Create QP */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 128;
    qp_init_attr.cap.max_recv_wr = 128;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        fprintf(stderr, "Failed to create QP\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* Register memory */
    res->buffer = malloc(BUFFER_SIZE);
    if (!res->buffer) {
        fprintf(stderr, "Failed to allocate buffer\n");
        ibv_free_device_list(device_list);
        return -1;
    }
    memset(res->buffer, 0, BUFFER_SIZE);

    res->mr = ibv_reg_mr(res->pd, res->buffer, BUFFER_SIZE,
                          IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
    if (!res->mr) {
        fprintf(stderr, "Failed to register MR\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    ibv_free_device_list(device_list);
    printf("RDMA init: QP num=%u, LID=%u, MR addr=%p, rkey=0x%x\n",
           res->qp->qp_num, res->lid, res->buffer, res->mr->rkey);
    return 0;
}

/* Modify QP to INIT state */
int modify_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE,
    };
    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                         IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

/* Modify QP to RTR state - supports both IB (LID) and RoCE (GID) modes */
int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qp_num,
                     uint16_t remote_lid, union ibv_gid *remote_gid,
                     uint8_t port, int is_roce)
{
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));

    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.sl = 0;
    attr.ah_attr.port_num = port;

    if (is_roce) {
        /* RoCE mode: must set is_global=1 + GRH */
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = *remote_gid;
        attr.ah_attr.grh.sgid_index = RDMA_DEFAULT_GID_INDEX;
        attr.ah_attr.grh.hop_limit = 64;
        attr.ah_attr.dlid = 0;
    } else {
        /* IB mode: use LID addressing */
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid = remote_lid;
    }

    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                         IBV_QP_AV);
}

/* Modify QP to RTS state */
int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = 0,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .max_rd_atomic = 1,
    };

    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_SQ_PSN |
                         IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
}

/* Perform RDMA Write */
int rdma_write(struct rdma_resources *res, uint64_t remote_addr,
               uint32_t remote_rkey, const char *data, size_t size)
{
    /* Copy data to locally registered memory */
    memcpy(res->buffer, data, size);

    struct ibv_sge sge = {
        .addr = (uint64_t)res->buffer,
        .length = (uint32_t)size,
        .lkey = res->mr->lkey,
    };

    struct ibv_send_wr wr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = remote_addr,
            .rkey = remote_rkey,
        },
    };

    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(res->qp, &wr, &bad_wr)) {
        fprintf(stderr, "RDMA Write failed\n");
        return -1;
    }

    /* Wait for completion */
    struct ibv_wc wc;
    while (1) {
        int ne = ibv_poll_cq(res->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Poll CQ failed\n");
            return -1;
        }
        if (ne > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC status: %s\n", ibv_wc_status_str(wc.status));
                return -1;
            }
            break;
        }
    }

    return 0;
}

/*
 * Exchange connection info via TCP socket
 * server_ip == NULL means this side is server, otherwise client connects to server_ip
 */
void exchange_connection(const char *server_ip,
                         struct connection_info *local,
                         struct connection_info *remote)
{
    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_PORT);

    if (server_ip) {
        /* Client: connect to server and exchange info */
        if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
            fprintf(stderr, "Invalid server IP: %s\n", server_ip);
            close(sock);
            return;
        }
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(sock);
            return;
        }
        /* Send local info first, then receive remote info */
        send(sock, local, sizeof(*local), 0);
        recv(sock, remote, sizeof(*remote), 0);
    } else {
        /* Server: listen and exchange info */
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            close(sock);
            return;
        }
        if (listen(sock, 1) < 0) {
            perror("listen");
            close(sock);
            return;
        }
        printf("Listening on port %d, waiting for client...\n", TCP_PORT);
        int client = accept(sock, NULL, NULL);
        if (client < 0) {
            perror("accept");
            close(sock);
            return;
        }
        /* Receive client info first, then send local info */
        recv(client, remote, sizeof(*remote), 0);
        send(client, local, sizeof(*local), 0);
        close(client);
    }
    close(sock);
}

/* Cleanup resources */
void cleanup_rdma_resources(struct rdma_resources *res)
{
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->buffer) free(res->buffer);
    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->context) ibv_close_device(res->context);
}

int main(int argc, char *argv[])
{
    struct rdma_resources res = {0};
    int is_server = (argc > 1 && strcmp(argv[1], "server") == 0);
    const char *peer_ip = is_server ? NULL : (argc > 2 ? argv[2] : "127.0.0.1");

    printf("Starting as %s\n", is_server ? "server" : "client");

    /* 1. Initialize RDMA resources */
    if (init_rdma_resources(&res) != 0) {
        return 1;
    }

    /* 2. Fill in local connection info */
    struct connection_info local_info = {
        .qp_num   = res.qp->qp_num,
        .lid      = res.lid,
        .gid      = res.gid,
        .buf_addr = (uint64_t)res.buffer,
        .buf_rkey = res.mr->rkey,
    };
    struct connection_info remote_info = {0};

    /* 3. Exchange connection info via TCP */
    exchange_connection(peer_ip, &local_info, &remote_info);
    printf("Exchanged info: remote QP=%u, LID=%u, addr=%lu, rkey=0x%x\n",
           remote_info.qp_num, remote_info.lid,
           (unsigned long)remote_info.buf_addr, remote_info.buf_rkey);

    /* 4. QP state transition: RESET -> INIT -> RTR -> RTS */
    if (modify_qp_to_init(res.qp) != 0) {
        fprintf(stderr, "Failed to move QP to INIT\n");
        cleanup_rdma_resources(&res);
        return 1;
    }
    if (modify_qp_to_rtr(res.qp, remote_info.qp_num, remote_info.lid,
                         &remote_info.gid, 1, res.is_roce) != 0) {
        fprintf(stderr, "Failed to move QP to RTR\n");
        cleanup_rdma_resources(&res);
        return 1;
    }
    if (modify_qp_to_rts(res.qp) != 0) {
        fprintf(stderr, "Failed to move QP to RTS\n");
        cleanup_rdma_resources(&res);
        return 1;
    }
    printf("QP state: RESET -> INIT -> RTR -> RTS (ready)\n");

    /* 5. Perform RDMA Write / Wait for data */
    if (is_server) {
        printf("Server: MR ready. addr=%lu, rkey=0x%x\n",
               (unsigned long)local_info.buf_addr, local_info.buf_rkey);
        printf("Server: waiting for client to write data...\n");

        /* Server waits for client to write: simple sleep then check memory */
        sleep(5);

        printf("Server: received data = \"%.*s\"\n", MSG_SIZE, res.buffer);
    } else {
        /* Client: perform RDMA Write to server's MR */
        const char *msg = "Hello RDMA Write! From client.";
        printf("Client: writing \"%s\" to server...\n", msg);

        if (rdma_write(&res, remote_info.buf_addr, remote_info.buf_rkey,
                       msg, strlen(msg) + 1) == 0) {
            printf("Client: RDMA Write succeeded.\n");
        } else {
            fprintf(stderr, "Client: RDMA Write failed.\n");
        }
    }

    cleanup_rdma_resources(&res);
    return 0;
}
