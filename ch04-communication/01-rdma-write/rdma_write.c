/**
 * RDMA Write 示例 - 客户端推送数据到服务器
 *
 * 流程：
 * 1. Server: 启动、创建资源、等待连接
 * 2. Client: 连接Server、交换MR信息、执行Write
 * 3. Server: 验证数据
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024
#define MSG_SIZE 256
#define TCP_PORT 9876

/* RDMA 资源结构 */
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buffer;
    uint16_t lid;  /* 本地端口 LID */
};

/* 用于 TCP 交换的连接信息 */
struct connection_info {
    uint32_t qp_num;
    uint16_t lid;
    uint64_t buf_addr;   /* Server MR 的虚拟地址（供 client 写入） */
    uint32_t buf_rkey;   /* Server MR 的 rkey */
};

/* 初始化RDMA资源 */
int init_rdma_resources(struct rdma_resources *res)
{
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_port_attr port_attr;
    int num_devices;

    /* 获取设备 */
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return -1;
    }
    device = device_list[0];

    /* 打开设备 */
    res->context = ibv_open_device(device);
    if (!res->context) {
        fprintf(stderr, "Failed to open device\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* 查询本地 LID */
    if (ibv_query_port(res->context, 1, &port_attr) != 0) {
        fprintf(stderr, "Failed to query port\n");
        ibv_free_device_list(device_list);
        return -1;
    }
    res->lid = port_attr.lid;

    /* 分配PD */
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* 创建CQ */
    res->cq = ibv_create_cq(res->context, 128, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        ibv_free_device_list(device_list);
        return -1;
    }

    /* 创建QP */
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

    /* 注册内存 */
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

/* 修改QP到INIT状态 */
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

/* 修改QP到RTR状态 */
int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qp_num,
                     uint16_t remote_lid, uint8_t port)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_256,
        .dest_qp_num = remote_qp_num,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .dlid = remote_lid,
            .sl = 0,
            .port_num = port,
        },
    };

    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_PATH_MTU |
                         IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                         IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                         IBV_QP_AV);
}

/* 修改QP到RTS状态 */
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

/* 执行RDMA Write */
int rdma_write(struct rdma_resources *res, uint64_t remote_addr,
               uint32_t remote_rkey, const char *data, size_t size)
{
    /* 把数据拷到本地注册内存中 */
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

    /* 等待完成 */
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
 * 通过 TCP socket 交换连接信息
 * server_ip == NULL 表示本端是 server，否则是 client 连接到 server_ip
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
        /* Client：连接 server 并交换信息 */
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
        /* 先发本地信息，再收远端信息 */
        send(sock, local, sizeof(*local), 0);
        recv(sock, remote, sizeof(*remote), 0);
    } else {
        /* Server：监听并交换信息 */
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
        /* 先收客户端信息，再发本地信息 */
        recv(client, remote, sizeof(*remote), 0);
        send(client, local, sizeof(*local), 0);
        close(client);
    }
    close(sock);
}

/* 清理资源 */
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

    /* 1. 初始化 RDMA 资源 */
    if (init_rdma_resources(&res) != 0) {
        return 1;
    }

    /* 2. 填写本地连接信息 */
    struct connection_info local_info = {
        .qp_num   = res.qp->qp_num,
        .lid      = res.lid,
        .buf_addr = (uint64_t)res.buffer,
        .buf_rkey = res.mr->rkey,
    };
    struct connection_info remote_info = {0};

    /* 3. 通过 TCP 交换连接信息 */
    exchange_connection(peer_ip, &local_info, &remote_info);
    printf("Exchanged info: remote QP=%u, LID=%u, addr=%lu, rkey=0x%x\n",
           remote_info.qp_num, remote_info.lid,
           (unsigned long)remote_info.buf_addr, remote_info.buf_rkey);

    /* 4. QP 状态转换：RESET -> INIT -> RTR -> RTS */
    if (modify_qp_to_init(res.qp) != 0) {
        fprintf(stderr, "Failed to move QP to INIT\n");
        cleanup_rdma_resources(&res);
        return 1;
    }
    if (modify_qp_to_rtr(res.qp, remote_info.qp_num, remote_info.lid, 1) != 0) {
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

    /* 5. 执行 RDMA Write / 等待数据 */
    if (is_server) {
        printf("Server: MR ready. addr=%lu, rkey=0x%x\n",
               (unsigned long)local_info.buf_addr, local_info.buf_rkey);
        printf("Server: waiting for client to write data...\n");

        /* Server 等待 client 写入：简单 sleep 后检查内存 */
        sleep(5);

        printf("Server: received data = \"%.*s\"\n", MSG_SIZE, res.buffer);
    } else {
        /* Client：向 server 的 MR 执行 RDMA Write */
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

