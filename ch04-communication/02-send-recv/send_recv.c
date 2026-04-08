/**
 * Send/Recv 通信示例
 * 完整的 Client-Server 实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024
#define MSG_HELLO 1
#define MSG_DATA 2
#define MSG_DONE 3

/* 消息结构 */
struct msg_header {
    uint32_t type;
    uint32_t size;
};

/* RDMA 资源 */
struct rdma_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buf;
    uint32_t qp_num;
    uint16_t lid;
};

/* 初始化 */
int init_rdma(struct rdma_context *ctx)
{
    struct ibv_device **list;
    struct ibv_qp_init_attr qp_init;
    int num;
    
    list = ibv_get_device_list(&num);
    if (!list || num == 0) {
        fprintf(stderr, "No device\n");
        return -1;
    }
    
    ctx->ctx = ibv_open_device(list[0]);
    if (!ctx->ctx) {
        perror("open device");
        return -1;
    }
    
    /* 获取本地LID */
    struct ibv_port_attr attr;
    ibv_query_port(ctx->ctx, 1, &attr);
    ctx->lid = attr.lid;
    
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    ctx->cq = ibv_create_cq(ctx->ctx, 256, NULL, NULL, 0);
    
    memset(&qp_init, 0, sizeof(qp_init));
    qp_init.send_cq = ctx->cq;
    qp_init.recv_cq = ctx->cq;
    qp_init.qp_type = IBV_QPT_RC;
    qp_init.cap.max_send_wr = 128;
    qp_init.cap.max_recv_wr = 128;
    qp_init.cap.max_send_sge = 1;
    qp_init.cap.max_recv_sge = 1;
    
    ctx->qp = ibv_create_qp(ctx->pd, &qp_init);
    ctx->qp_num = ctx->qp->qp_num;
    
    ctx->buf = malloc(BUFFER_SIZE);
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE | 
                         IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    
    ibv_free_device_list(list);
    return 0;
}

/* 修改QP状态 */
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
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qp, uint16_t remote_lid)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_256,
        .dest_qp_num = remote_qp,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {.dlid = remote_lid, .sl = 0, .port_num = 1},
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

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
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

/* 交换信息（通过socket） */
void exchange_info(const char *server_ip, uint32_t *local_qp, uint16_t *local_lid,
                   uint32_t *remote_qp, uint16_t *remote_lid)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    
    *local_qp = 0;
    *local_lid = 0;
    
    if (server_ip) {
        /* Client: connect and exchange */
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9999);
        inet_pton(AF_INET, server_ip, &addr.sin_addr);
        
        connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        
        /* Send local info */
        send(sock, local_qp, sizeof(*local_qp), 0);
        send(sock, local_lid, sizeof(*local_lid), 0);
        
        /* Recv remote info */
        recv(sock, remote_qp, sizeof(*remote_qp), 0);
        recv(sock, remote_lid, sizeof(*remote_lid), 0);
    } else {
        /* Server: listen and exchange */
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9999);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (struct sockaddr*)&addr, sizeof(addr));
        listen(sock, 1);
        
        int client = accept(sock, NULL, NULL);
        
        recv(client, remote_qp, sizeof(*remote_qp), 0);
        recv(client, remote_lid, sizeof(*remote_lid), 0);
        
        send(client, local_qp, sizeof(*local_qp), 0);
        send(client, local_lid, sizeof(*local_lid), 0);
        
        close(client);
    }
    close(sock);
}

/* 发送消息 */
int send_message(struct rdma_context *ctx, void *data, size_t len, int opcode)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)data,
        .length = len,
        .lkey = ctx->mr->lkey,
    };
    
    struct ibv_send_wr wr = {
        .wr_id = opcode,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = opcode == MSG_DONE ? IBV_WR_SEND_WITH_IMM : IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    
    if (opcode == MSG_DONE) {
        wr.imm_data = htobe32(opcode);
    }
    
    struct ibv_send_wr *bad;
    if (ibv_post_send(ctx->qp, &wr, &bad)) {
        return -1;
    }
    
    /* 等待完成 */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);
    
    return (wc.status == IBV_WC_SUCCESS) ? 0 : -1;
}

/* 接收消息 */
int recv_message(struct rdma_context *ctx)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->buf,
        .length = BUFFER_SIZE,
        .lkey = ctx->mr->lkey,
    };
    
    struct ibv_recv_wr wr = {
        .wr_id = 0,
        .sg_list = &sge,
        .num_sge = 1,
    };
    
    struct ibv_recv_wr *bad;
    if (ibv_post_recv(ctx->qp, &wr, &bad)) {
        return -1;
    }
    
    /* 等待完成 */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);
    
    if (wc.status != IBV_WC_SUCCESS) {
        return -1;
    }
    
    return wc.byte_len;
}

int main(int argc, char *argv[])
{
    struct rdma_context ctx = {0};
    int is_server = (argc > 1 && strcmp(argv[1], "server") == 0);
    const char *peer_ip = is_server ? NULL : argv[2];
    
    printf("Running as %s\n", is_server ? "server" : "client");
    
    init_rdma(&ctx);
    modify_qp_to_init(ctx.qp);
    
    uint32_t local_qp = ctx.qp_num, remote_qp;
    uint16_t local_lid = ctx.lid, remote_lid;
    
    exchange_info(peer_ip, &local_qp, &local_lid, &remote_qp, &remote_lid);
    
    modify_qp_to_rtr(ctx.qp, remote_qp, remote_lid);
    modify_qp_to_rts(ctx.qp);
    
    if (is_server) {
        printf("Server ready, waiting for messages...\n");
        
        /* 预post recv */
        for (int i = 0; i < 10; i++) {
            recv_message(&ctx);
        }
        
        /* 处理消息 */
        for (int i = 0; i < 3; i++) {
            int len = recv_message(&ctx);
            struct msg_header *hdr = (struct msg_header*)ctx.buf;
            printf("Received: type=%u, size=%u, len=%d\n", 
                   hdr->type, hdr->size, len);
            
            if (hdr->type == MSG_DONE) break;
            
            /* 回复 */
            char reply[] = "ACK";
            send_message(&ctx, reply, strlen(reply), MSG_DATA);
        }
    } else {
        printf("Client sending messages...\n");
        
        /* 预post recv */
        for (int i = 0; i < 10; i++) {
            recv_message(&ctx);
        }
        
        /* 发送消息 */
        struct msg_header msg = {.type = MSG_HELLO, .size = 13};
        memcpy(ctx.buf, &msg, sizeof(msg));
        memcpy(ctx.buf + sizeof(msg), "Hello Server!", 13);
        send_message(&ctx, ctx.buf, sizeof(msg) + 13, MSG_DATA);
        
        /* 等待ACK */
        recv_message(&ctx);
        printf("Got ACK\n");
        
        /* 发送完成 */
        send_message(&ctx, NULL, 0, MSG_DONE);
    }
    
    printf("Done\n");
    return 0;
}
