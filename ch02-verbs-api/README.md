# 第二章：Verbs API 入门

## 学习目标
- 掌握RDMA编程的基本流程
- 学会初始化硬件设备
- 理解资源创建与销毁
- 能看懂简单的RDMA程序

## 2.1 Verbs API 概述

libibverbs 是 RDMA 编程的底层 API，提供与硬件无关的编程接口。

### 核心头文件
```c
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>  // RDMA CM
```

---

## 2.2 设备发现与初始化

### 获取设备列表
```c
struct ibv_device **device_list;
int num_devices;

device_list = ibv_get_device_list(&num_devices);
if (!device_list) {
    perror("Failed to get device list");
    return -1;
}
```

### 打开设备
```c
struct ibv_context *context;
struct ibv_device *device = device_list[0];  // 选择第一个设备

context = ibv_open_device(device);
if (!context) {
    perror("Failed to open device");
    return -1;
}
```

---

## 2.3 资源创建流程

### 创建 Protection Domain (PD)
```c
struct ibv_pd *pd;
pd = ibv_alloc_pd(context);
if (!pd) {
    perror("Failed to allocate PD");
    return -1;
}
```

### 创建 Completion Queue (CQ)
```c
struct ibv_cq *cq;
int cq_size = 128;  // 完成队列大小

cq = ibv_create_cq(context, cq_size, NULL, NULL, 0);
if (!cq) {
    perror("Failed to create CQ");
    return -1;
}
```

### 创建 Queue Pair (QP)
```c
struct ibv_qp_init_attr qp_init_attr = {
    .send_cq = cq,
    .recv_cq = cq,
    .qp_type = IBV_QPT_RC,  // 可靠连接
    .cap = {
        .max_send_wr = 128,
        .max_recv_wr = 128,
        .max_send_sge = 1,
        .max_recv_sge = 1,
    },
};

struct ibv_qp *qp;
qp = ibv_create_qp(pd, &qp_init_attr);
if (!qp) {
    perror("Failed to create QP");
    return -1;
}
```

---

## 2.4 内存注册 (MR)

### 注册内存区域
```c
#define BUFFER_SIZE 4096

char *buffer = malloc(BUFFER_SIZE);
struct ibv_mr *mr;

mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, 
                 IBV_ACCESS_LOCAL_WRITE | 
                 IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE);
if (!mr) {
    perror("Failed to register MR");
    return -1;
}
```

### 内存权限
| 标志 | 说明 |
|------|------|
| IBV_ACCESS_LOCAL_WRITE | 本地可写 |
| IBV_ACCESS_REMOTE_READ | 远程可读 |
| IBV_ACCESS_REMOTE_WRITE | 远程可写 |
| IBV_ACCESS_ATOMIC | 远程原子操作 |

---

## 2.5 QP 状态机

QP 必须经过三次转换才能正常工作：

```
RESET → INIT → RTR (Ready To Receive) → RTS (Ready To Send)
```

### 转换到 INIT
```c
struct ibv_qp_attr init_attr = {
    .qp_state = IBV_QPS_INIT,
    .pkey_index = 0,
    .port_num = 1,
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | 
                      IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE,
};

ibv_modify_qp(qp, &init_attr, IBV_QP_STATE);
```

### 转换到 RTR
```c
struct ibv_qp_attr rtr_attr = {
    .qp_state = IBV_QPS_RTR,
    .path_mtu = IBV_MTU_256,
    .dest_qp_num = remote_qp_num,  // 对端QP号
    .rq_psn = 0,
    .max_dest_rd_atomic = 1,
    .min_rnr_timer = 12,
    .ah_attr = {
        .dlid = remote_lid,      // 对端LID
        .sl = 0,
        .port_num = 1,
    },
};

ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE);
```

### 转换到 RTS
```c
struct ibv_qp_attr rts_attr = {
    .qp_state = IBV_QPS_RTS,
    .sq_psn = 0,
    .timeout = 14,
    .retry_cnt = 7,
    .rnr_retry = 7,
    .max_rd_atomic = 1,
};

ibv_modify_qp(qp, &rts_attr, IBV_QP_STATE);
```

---

## 2.6 通信操作

### Post Send (发送)
```c
struct ibv_send_wr wr = {
    .wr_id = 0,
    .sg_list = &sge,
    .num_sge = 1,
    .opcode = IBV_WR_SEND,  // 可以是 RDMA_WRITE, RDMA_READ 等
    .send_flags = IBV_SEND_SIGNALED,
};

struct ibv_send_wr *bad_wr;
ibv_post_send(qp, &wr, &bad_wr);
```

### Post Recv (接收)
```c
struct ibv_recv_wr wr = {
    .wr_id = 0,
    .sg_list = &sge,
    .num_sge = 1,
};

struct ibv_recv_wr *bad_wr;
ibv_post_recv(qp, &wr, &bad_wr);
```

### Polling Completion
```c
struct ibv_wc wc;
int ne = ibv_poll_cq(cq, 1, &wc);
if (ne > 0) {
    if (wc.status == IBV_WC_SUCCESS) {
        // 操作成功
    } else {
        // 操作失败
    }
}
```

---

## 2.7 资源销毁

```c
ibv_dereg_mr(mr);
ibv_destroy_qp(qp);
ibv_destroy_cq(cq);
ibv_dealloc_pd(pd);
ibv_close_device(context);
ibv_free_device_list(device_list);
free(buffer);
```

---

## 📂 示例代码

| 文件 | 说明 |
|------|------|
| [01-initialization/rdma_init.c](./01-initialization/rdma_init.c) | 完整的设备初始化流程 |
| [02-qp-state/qp_state.c](./02-qp-state/qp_state.c) | QP状态转换示例 |

### 编译运行

```bash
# 编译
cd 01-initialization
make

# 运行 (需要RDMA硬件)
./rdma_init
```

---

## 练习题

1. 为什么要ibv_get_device_list而不是直接打开设备？
2. PD、QP、MR之间的关系是什么？
3. QP状态机有几个状态？分别是什么？
4. 内存注册的访问标志有哪些？
5. 如何获取通信完成事件？

---

## 下一步

进入下一章：[第三章：QP与MR深入](../ch03-qp-mr/README.md)
