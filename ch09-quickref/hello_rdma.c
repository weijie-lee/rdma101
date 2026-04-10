/**
 * RDMA Hello World — 最小完整 Send/Recv 示例
 *
 * 这是一个单机 Loopback 程序：在同一进程内创建两个 QP，
 * 一个发送 "Hello RDMA!"，另一个接收并打印。
 *
 * 特点:
 *   - 自动检测 IB/RoCE 模式
 *   - 使用 common/rdma_utils.h 工具库
 *   - 每一行都有中文注释
 *   - 可直接复制作为新项目的起始模板
 *
 * 编译:
 *   gcc -o hello_rdma hello_rdma.c \
 *       -I../common ../common/librdma_utils.a -libverbs
 *
 * 运行:
 *   ./hello_rdma
 */

#include <stdio.h>          /* printf, fprintf */
#include <stdlib.h>         /* malloc, free, exit */
#include <string.h>         /* memset, strcpy, strlen */
#include <unistd.h>         /* close */
#include <infiniband/verbs.h>   /* 所有 libibverbs API */
#include "rdma_utils.h"     /* 公共工具库 */

/* ========== 常量定义 ========== */
#define BUF_SIZE    1024    /* 收发缓冲区大小 (字节) */
#define CQ_SIZE     16      /* 完成队列容量 */
#define PORT_NUM    1       /* 使用的端口号 */
#define GID_INDEX   0       /* GID 索引 (RoCE v2 可能需要改为 1 或 3) */
#define MSG         "Hello RDMA!"   /* 要发送的消息 */

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    (void)argc;     /* 未使用 */
    (void)argv;     /* 未使用 */

    /* ---- 所有资源指针初始化为 NULL (方便 cleanup 统一释放) ---- */
    struct ibv_device **dev_list    = NULL;  /* 设备列表 */
    struct ibv_context *ctx         = NULL;  /* 设备上下文 */
    struct ibv_pd      *pd          = NULL;  /* 保护域 */
    struct ibv_cq      *cq          = NULL;  /* 完成队列 (两个 QP 共用) */
    struct ibv_qp      *sender_qp   = NULL;  /* 发送端 QP */
    struct ibv_qp      *recver_qp   = NULL;  /* 接收端 QP */
    struct ibv_mr      *send_mr     = NULL;  /* 发送缓冲区 MR */
    struct ibv_mr      *recv_mr     = NULL;  /* 接收缓冲区 MR */
    char               *send_buf    = NULL;  /* 发送缓冲区 */
    char               *recv_buf    = NULL;  /* 接收缓冲区 */
    int                 ret         = 0;     /* 返回值 */
    int                 num_devices = 0;     /* 设备数量 */

    printf("=== RDMA Hello World (Loopback Send/Recv) ===\n\n");

    /* ================================================================
     * 步骤 1: 打开 RDMA 设备
     * ================================================================ */
    printf("[步骤 1] 打开 RDMA 设备...\n");

    dev_list = ibv_get_device_list(&num_devices);   /* 获取所有 RDMA 设备 */
    CHECK_NULL(dev_list, "获取设备列表失败");        /* 检查是否成功 */

    if (num_devices == 0) {                          /* 没有设备 */
        fprintf(stderr, "[错误] 系统中没有 RDMA 设备，请检查驱动\n");
        goto cleanup;
    }

    ctx = ibv_open_device(dev_list[0]);              /* 打开第一个设备 */
    CHECK_NULL(ctx, "打开设备失败");

    printf("  设备名: %s\n", ibv_get_device_name(dev_list[0]));

    /* ---- 自动检测传输层类型 (IB 还是 RoCE) ---- */
    enum rdma_transport transport = detect_transport(ctx, PORT_NUM);
    int is_roce = (transport == RDMA_TRANSPORT_ROCE);  /* 是否为 RoCE */
    printf("  传输层: %s\n", transport_str(transport));

    /* ================================================================
     * 步骤 2: 分配保护域 (PD)
     * ================================================================ */
    printf("[步骤 2] 分配保护域 (PD)...\n");

    pd = ibv_alloc_pd(ctx);                          /* PD 是所有资源的容器 */
    CHECK_NULL(pd, "分配 PD 失败");

    printf("  PD 分配成功\n");

    /* ================================================================
     * 步骤 3: 创建完成队列 (CQ)
     * ================================================================ */
    printf("[步骤 3] 创建完成队列 (CQ)...\n");

    cq = ibv_create_cq(ctx, CQ_SIZE, NULL, NULL, 0); /* 两个 QP 共用一个 CQ */
    CHECK_NULL(cq, "创建 CQ 失败");

    printf("  CQ 创建成功 (容量=%d)\n", CQ_SIZE);

    /* ================================================================
     * 步骤 4: 创建两个 QP (sender 和 recver)
     * ================================================================ */
    printf("[步骤 4] 创建 QP...\n");

    struct ibv_qp_init_attr qp_init_attr;            /* QP 初始化属性 */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));  /* 清零 */
    qp_init_attr.send_cq = cq;                       /* 发送完成队列 */
    qp_init_attr.recv_cq = cq;                       /* 接收完成队列 */
    qp_init_attr.qp_type = IBV_QPT_RC;               /* 可靠连接 (RC) 类型 */
    qp_init_attr.cap.max_send_wr  = 4;               /* 最大发送 WR 数 */
    qp_init_attr.cap.max_recv_wr  = 4;               /* 最大接收 WR 数 */
    qp_init_attr.cap.max_send_sge = 1;               /* 每个 WR 最大 SGE 数 */
    qp_init_attr.cap.max_recv_sge = 1;               /* 每个 WR 最大 SGE 数 */

    sender_qp = ibv_create_qp(pd, &qp_init_attr);    /* 创建发送端 QP */
    CHECK_NULL(sender_qp, "创建发送端 QP 失败");

    recver_qp = ibv_create_qp(pd, &qp_init_attr);    /* 创建接收端 QP */
    CHECK_NULL(recver_qp, "创建接收端 QP 失败");

    printf("  发送端 QP 编号: %u\n", sender_qp->qp_num);
    printf("  接收端 QP 编号: %u\n", recver_qp->qp_num);

    /* ================================================================
     * 步骤 5: 注册内存区域 (MR)
     * ================================================================ */
    printf("[步骤 5] 注册内存区域 (MR)...\n");

    send_buf = (char *)malloc(BUF_SIZE);              /* 分配发送缓冲区 */
    CHECK_NULL(send_buf, "分配发送缓冲区失败");
    memset(send_buf, 0, BUF_SIZE);                    /* 清零 */
    strcpy(send_buf, MSG);                            /* 写入要发送的消息 */

    recv_buf = (char *)malloc(BUF_SIZE);              /* 分配接收缓冲区 */
    CHECK_NULL(recv_buf, "分配接收缓冲区失败");
    memset(recv_buf, 0, BUF_SIZE);                    /* 清零 */

    /* 注册发送 MR (本地写权限即可) */
    send_mr = ibv_reg_mr(pd, send_buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(send_mr, "注册发送 MR 失败");

    /* 注册接收 MR (本地写权限，因为 NIC 要写入数据) */
    recv_mr = ibv_reg_mr(pd, recv_buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    CHECK_NULL(recv_mr, "注册接收 MR 失败");

    printf("  发送 MR: lkey=0x%x\n", send_mr->lkey);
    printf("  接收 MR: lkey=0x%x\n", recv_mr->lkey);

    /* ================================================================
     * 步骤 6: QP 状态转换 (RESET → INIT → RTR → RTS)
     * ================================================================ */
    printf("[步骤 6] QP 状态转换...\n");

    /* ---- 填充端点信息 (Loopback: 每个 QP 的对端就是另一个 QP) ---- */
    struct rdma_endpoint sender_ep, recver_ep;

    /* 填充发送端端点信息 */
    ret = fill_local_endpoint(ctx, sender_qp, PORT_NUM, GID_INDEX, &sender_ep);
    CHECK_ERRNO(ret, "填充发送端端点失败");

    /* 填充接收端端点信息 */
    ret = fill_local_endpoint(ctx, recver_qp, PORT_NUM, GID_INDEX, &recver_ep);
    CHECK_ERRNO(ret, "填充接收端端点失败");

    /* ---- 发送端 QP 连接到接收端 (对端 = recver) ---- */
    int access = IBV_ACCESS_LOCAL_WRITE;              /* Send/Recv 不需要 REMOTE 权限 */
    ret = qp_full_connect(sender_qp, &recver_ep, PORT_NUM, is_roce, access);
    CHECK_ERRNO(ret, "发送端 QP 状态转换失败");

    /* ---- 接收端 QP 连接到发送端 (对端 = sender) ---- */
    ret = qp_full_connect(recver_qp, &sender_ep, PORT_NUM, is_roce, access);
    CHECK_ERRNO(ret, "接收端 QP 状态转换失败");

    printf("  两个 QP 已互相连接 (RESET → INIT → RTR → RTS)\n");

    /* ================================================================
     * 步骤 7: Post Recv (必须在 Send 之前!)
     * ================================================================ */
    printf("[步骤 7] 在接收端 Post Recv...\n");

    struct ibv_sge recv_sge;                          /* 接收 SGE */
    memset(&recv_sge, 0, sizeof(recv_sge));
    recv_sge.addr   = (uint64_t)recv_buf;             /* 接收缓冲区地址 */
    recv_sge.length = BUF_SIZE;                       /* 缓冲区大小 */
    recv_sge.lkey   = recv_mr->lkey;                  /* MR 的 lkey */

    struct ibv_recv_wr recv_wr, *bad_recv_wr;         /* 接收 WR */
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id   = 1;                              /* 自定义 ID: 1 表示接收 */
    recv_wr.sg_list = &recv_sge;                      /* 指向 SGE */
    recv_wr.num_sge = 1;                              /* 1 个 SGE */
    recv_wr.next    = NULL;                           /* 无后续 WR */

    ret = ibv_post_recv(recver_qp, &recv_wr, &bad_recv_wr); /* 提交接收请求 */
    CHECK_ERRNO(ret, "Post Recv 失败");

    printf("  已提交接收请求 (等待数据到来)\n");

    /* ================================================================
     * 步骤 8: Post Send (发送 "Hello RDMA!")
     * ================================================================ */
    printf("[步骤 8] 在发送端 Post Send...\n");

    struct ibv_sge send_sge;                          /* 发送 SGE */
    memset(&send_sge, 0, sizeof(send_sge));
    send_sge.addr   = (uint64_t)send_buf;             /* 发送缓冲区地址 */
    send_sge.length = strlen(MSG) + 1;                /* 数据长度 (含 \0) */
    send_sge.lkey   = send_mr->lkey;                  /* MR 的 lkey */

    struct ibv_send_wr send_wr, *bad_send_wr;         /* 发送 WR */
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id      = 2;                           /* 自定义 ID: 2 表示发送 */
    send_wr.sg_list    = &send_sge;                   /* 指向 SGE */
    send_wr.num_sge    = 1;                           /* 1 个 SGE */
    send_wr.opcode     = IBV_WR_SEND;                 /* 操作类型: Send */
    send_wr.send_flags = IBV_SEND_SIGNALED;           /* 完成时产生 CQE */
    send_wr.next       = NULL;                        /* 无后续 WR */

    ret = ibv_post_send(sender_qp, &send_wr, &bad_send_wr); /* 提交发送请求 */
    CHECK_ERRNO(ret, "Post Send 失败");

    printf("  已发送: \"%s\" (%zu 字节)\n", MSG, strlen(MSG) + 1);

    /* ================================================================
     * 步骤 9: 轮询 CQ 等待完成
     * ================================================================ */
    printf("[步骤 9] 轮询 CQ 等待完成事件...\n");

    /* ---- 等待发送完成 ---- */
    struct ibv_wc wc;                                 /* 完成事件 */
    ret = poll_cq_blocking(cq, &wc);                  /* 阻塞轮询 */
    CHECK_ERRNO(ret, "轮询发送完成失败");

    if (wc.status != IBV_WC_SUCCESS) {                /* 检查状态 */
        fprintf(stderr, "[错误] 发送失败: %s\n", ibv_wc_status_str(wc.status));
        goto cleanup;
    }
    printf("  发送完成 ✓ (wr_id=%lu)\n", wc.wr_id);
    print_wc_detail(&wc);                             /* 打印完成事件详情 */

    /* ---- 等待接收完成 ---- */
    ret = poll_cq_blocking(cq, &wc);                  /* 阻塞轮询 */
    CHECK_ERRNO(ret, "轮询接收完成失败");

    if (wc.status != IBV_WC_SUCCESS) {                /* 检查状态 */
        fprintf(stderr, "[错误] 接收失败: %s\n", ibv_wc_status_str(wc.status));
        goto cleanup;
    }
    printf("  接收完成 ✓ (wr_id=%lu, 字节数=%u)\n", wc.wr_id, wc.byte_len);
    print_wc_detail(&wc);                             /* 打印完成事件详情 */

    /* ================================================================
     * 步骤 10: 验证接收数据
     * ================================================================ */
    printf("\n[步骤 10] 验证接收数据...\n");
    printf("  接收到: \"%s\"\n", recv_buf);            /* 打印接收到的消息 */

    if (strcmp(send_buf, recv_buf) == 0) {             /* 对比收发数据 */
        printf("\n  ★ 成功! RDMA Send/Recv 验证通过 ★\n");
    } else {
        fprintf(stderr, "\n  ✗ 失败! 收发数据不一致\n");
    }

    printf("\n=== 程序结束 ===\n");

    /* ================================================================
     * 清理: 逆序释放所有资源
     * ================================================================ */
cleanup:
    printf("\n[清理] 释放资源...\n");

    if (sender_qp) ibv_destroy_qp(sender_qp);        /* 销毁发送端 QP */
    if (recver_qp) ibv_destroy_qp(recver_qp);        /* 销毁接收端 QP */
    if (send_mr)   ibv_dereg_mr(send_mr);             /* 注销发送 MR */
    if (recv_mr)   ibv_dereg_mr(recv_mr);             /* 注销接收 MR */
    if (cq)        ibv_destroy_cq(cq);                /* 销毁 CQ */
    if (pd)        ibv_dealloc_pd(pd);                /* 释放 PD */
    if (ctx)       ibv_close_device(ctx);             /* 关闭设备 */
    if (dev_list)  ibv_free_device_list(dev_list);    /* 释放设备列表 */
    if (send_buf)  free(send_buf);                    /* 释放发送缓冲区 */
    if (recv_buf)  free(recv_buf);                    /* 释放接收缓冲区 */

    printf("  资源释放完成\n");
    return ret;
}
