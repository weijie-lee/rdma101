/**
 * RDMA Read 示例
 * 客户端从服务器拉取数据
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 4096

/* 交换的数据结构 */
struct connection_info {
    uint32_t qp_num;
    uint16_t lid;
    uint64_t buf_addr;
    uint32_t buf_rkey;
};

struct rdma_ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *send_buf;
    char *recv_buf;
};

int init_ctx(struct rdma_ctx *ctx)
{
    struct ibv_device **list;
    int num;
    
    list = ibv_get_device_list(&num);
    if (!list) return -1;
    
    ctx->ctx = ibv_open_device(list[0]);
    ctx->pd = ibv_alloc_pd(ctx->ctx);
    ctx->cq = ibv_create_cq(ctx->ctx, 256, NULL, NULL, 0);
    
    struct ibv_qp_init_attr attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 128, .max_recv_wr = 128,
                 .max_send_sge = 1, .max_recv_sge = 1 },
    };
    ctx->qp = ibv_create_qp(ctx->pd, &attr);
    
    ctx->send_buf = malloc(BUFFER_SIZE);
    ctx->recv_buf = malloc(BUFFER_SIZE);
    
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ);
    
    ibv_free_device_list(list);
    return 0;
}

int qp_connect(struct rdma_ctx *ctx, uint32_t remote_qp, uint16_t remote_lid)
{
    struct ibv_qp_attr attr;
    
    /* INIT */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE);
    
    /* RTR */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = remote_qp;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.port_num = 1;
    ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE);
    
    /* RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE);
    
    return 0;
}

/* 执行RDMA Read */
int rdma_read(struct rdma_ctx *ctx, uint64_t remote_addr, uint32_t remote_rkey)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->send_buf,
        .length = BUFFER_SIZE,
        .lkey = ctx->mr->lkey,
    };
    
    struct ibv_send_wr wr = {
        .opcode = IBV_WR_RDMA_READ,
        .wr.rdma = { .remote_addr = remote_addr, .rkey = remote_rkey },
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED,
    };
    
    struct ibv_send_wr *bad;
    ibv_post_send(ctx->qp, &wr, &bad);
    
    /* 等待完成 */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);
    
    return (wc.status == IBV_WC_SUCCESS) ? 0 : -1;
}

/* 通过socket交换连接信息 */
void exchange_connection(struct rdma_ctx *ctx, const char *server_ip,
                        struct connection_info *local, struct connection_info *remote)
{
    local->qp_num = ctx->qp->qp_num;
    local->buf_addr = (uint64_t)ctx->recv_buf;
    local->buf_rkey = ctx->mr->rkey;
    
    struct ibv_port_attr attr;
    ibv_query_port(ctx->ctx, 1, &attr);
    local->lid = attr.lid;
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8888) };
    
    if (server_ip) {
        /* Client */
        inet_pton(AF_INET, server_ip, &addr.sin_addr);
        connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        send(sock, local, sizeof(*local), 0);
        recv(sock, remote, sizeof(*remote), 0);
    } else {
        /* Server */
        bind(sock, (struct sockaddr*)&addr, sizeof(addr));
        listen(sock, 1);
        int c = accept(sock, NULL, NULL);
        recv(c, remote, sizeof(*remote), 0);
        send(c, local, sizeof(*local), 0);
        close(c);
    }
    close(sock);
}

int main(int argc, char *argv[])
{
    int is_server = (argc > 1 && strcmp(argv[1], "server") == 0);
    const char *peer_ip = is_server ? NULL : argv[2];
    
    struct rdma_ctx ctx = {0};
    init_ctx(&ctx);
    
    struct connection_info local_info = {0}, remote_info = {0};
    exchange_connection(&ctx, peer_ip, &local_info, &remote_info);
    
    qp_connect(&ctx, remote_info.qp_num, remote_info.lid);
    
    if (is_server) {
        /* Server: 填充数据供Client读取 */
        printf("Server: filling data...\n");
        sprintf(ctx.recv_buf, "Hello from server! Time: %ld", time(NULL));
        
        printf("Server: data ready at addr=%lu, rkey=%u\n",
               (unsigned long)local_info.buf_addr, local_info.buf_rkey);
        printf("Server: waiting...\n");
        sleep(10);
    } else {
        /* Client: 执行RDMA Read */
        printf("Client: reading from server...\n");
        printf("Remote: addr=%lu, rkey=%u\n",
               (unsigned long)remote_info.buf_addr, remote_info.buf_rkey);
        
        sleep(1);  /* 等待Server准备好 */
        
        if (rdma_read(&ctx, remote_info.buf_addr, remote_info.buf_rkey) == 0) {
            printf("Client: read data = \"%s\"\n", ctx.send_buf);
        } else {
            printf("Client: read failed\n");
        }
    }
    
    return 0;
}
