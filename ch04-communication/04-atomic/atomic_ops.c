/**
 * 原子操作示例
 * 使用 Fetch & Add 和 Compare & Swap 实现计数器
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 4096

struct connection_info {
    uint32_t qp_num;
    uint16_t lid;
    uint64_t counter_addr;
    uint32_t counter_rkey;
};

struct rdma_ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint64_t *counter;  /* 计数器 */
    char *result_buf;   /* 存储原子操作结果 */
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
    
    /* 分配计数器内存 - 对齐到缓存行 */
    posix_memalign((void**)&ctx->counter, 64, sizeof(uint64_t));
    posix_memalign((void**)&ctx->result_buf, 64, BUFFER_SIZE);
    *ctx->counter = 0;
    
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->counter, sizeof(uint64_t),
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                         IBV_ACCESS_REMOTE_READ | IBV_ACCESS_ATOMIC);
    
    ibv_free_device_list(list);
    return 0;
}

int qp_connect(struct rdma_ctx *ctx, uint32_t remote_qp, uint16_t remote_lid)
{
    struct ibv_qp_attr attr;
    
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | 
                          IBV_ACCESS_REMOTE_READ | IBV_ACCESS_ATOMIC;
    ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE);
    
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

/* Fetch and Add - 原子递增计数器 */
int atomic_fetch_add(struct rdma_ctx *ctx, uint64_t remote_addr, 
                     uint32_t remote_rkey, uint64_t add_value)
{
    /* 结果将返回到本地内存 */
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->result_buf,
        .length = sizeof(uint64_t),
        .lkey = ctx->mr->lkey,
    };
    
    struct ibv_send_wr wr = {
        .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
        .wr.atomic = {
            .remote_addr = remote_addr,
            .rkey = remote_rkey,
            .compare_add = add_value,
        },
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED,
    };
    
    struct ibv_send_wr *bad;
    ibv_post_send(ctx->qp, &wr, &bad);
    
    /* 等待完成 */
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);
    
    if (wc.status != IBV_WC_SUCCESS) {
        printf("Atomic FAA failed: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }
    
    /* result_buf 包含旧值 */
    return 0;
}

/* Compare and Swap - 原子条件更新 */
int atomic_cas(struct rdma_ctx *ctx, uint64_t remote_addr,
               uint32_t remote_rkey, uint64_t expected, uint64_t new_value)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)ctx->result_buf,
        .length = sizeof(uint64_t),
        .lkey = ctx->mr->lkey,
    };
    
    struct ibv_send_wr wr = {
        .opcode = IBV_WR_ATOMIC_CAS,
        .wr.atomic = {
            .remote_addr = remote_addr,
            .rkey = remote_rkey,
            .compare_add = expected,  /* 期望值 */
            .swap = new_value,        /* 新值 */
        },
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED,
    };
    
    struct ibv_send_wr *bad;
    ibv_post_send(ctx->qp, &wr, &bad);
    
    struct ibv_wc wc;
    while (ibv_poll_cq(ctx->cq, 1, &wc) == 0);
    
    return (wc.status == IBV_WC_SUCCESS) ? 0 : -1;
}

void exchange_info(struct rdma_ctx *ctx, const char *server_ip,
                   struct connection_info *local, struct connection_info *remote)
{
    local->qp_num = ctx->qp->qp_num;
    local->counter_addr = (uint64_t)ctx->counter;
    local->counter_rkey = ctx->mr->rkey;
    
    struct ibv_port_attr attr;
    ibv_query_port(ctx->ctx, 1, &attr);
    local->lid = attr.lid;
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(7777) };
    
    if (server_ip) {
        inet_pton(AF_INET, server_ip, &addr.sin_addr);
        connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        send(sock, local, sizeof(*local), 0);
        recv(sock, remote, sizeof(*remote), 0);
    } else {
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
    exchange_info(&ctx, peer_ip, &local_info, &remote_info);
    
    qp_connect(&ctx, remote_info.qp_num, remote_info.lid);
    
    if (is_server) {
        printf("Server: counter at addr=%lu, rkey=%u\n",
               (unsigned long)local_info.counter_addr, local_info.counter_rkey);
        printf("Server: initial counter = %lu\n", *ctx.counter);
        
        /* 等待客户端操作 */
        sleep(5);
        printf("Server: final counter = %lu\n", *ctx.counter);
    } else {
        printf("Client: performing atomic operations...\n");
        printf("Remote: addr=%lu, rkey=%u\n",
               (unsigned long)remote_info.counter_addr, remote_info.counter_rkey);
        
        sleep(1);
        
        /* 第一次 FAA: +1 */
        atomic_fetch_add(&ctx, remote_info.counter_addr, remote_info.counter_rkey, 1);
        printf("Client: FAA +1, old value = %lu\n", *(uint64_t*)ctx.result_buf);
        
        /* 第二次 FAA: +10 */
        atomic_fetch_add(&ctx, remote_info.counter_addr, remote_info.counter_rkey, 10);
        printf("Client: FAA +10, old value = %lu\n", *(uint64_t*)ctx.result_buf);
        
        /* CAS: 尝试将计数器从当前值改为 100 */
        uint64_t current = *(uint64_t*)ctx.result_buf;
        atomic_cas(&ctx, remote_info.counter_addr, remote_info.counter_rkey, current, 100);
        printf("Client: CAS expected=%lu, swapped=%s\n", 
               current, *(uint64_t*)ctx.result_buf == current ? "YES" : "NO");
    }
    
    return 0;
}
