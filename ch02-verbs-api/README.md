# 第二章：Verbs API 入门

## 学习目标

| 目标 | 说明 |
|------|------|
| 掌握RDMA编程基本流程 | 理解设备发现→资源创建→通信→销毁的完整流程 |
| 学会初始化硬件设备 | 掌握ibv_get_device_list、ibv_open_device |
| 理解资源创建与销毁 | 掌握PD、CQ、QP、MR的创建和销毁 |
| 能看懂简单的RDMA程序 | 理解示例代码的每个部分 |

---

## 2.1 Verbs API概述

### 什么是libibverbs？

**libibverbs** 是Linux下RDMA编程的底层API，提供与硬件无关的编程接口。

```
┌─────────────────────────────────────────────┐
│           Application (你的程序)              │
└─────────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────┐
│           libibverbs (Verbs API)            │
│   - 设备管理 (ibv_*)                        │
│   - 内存管理 (ibv_*)                        │
│   - 通信操作 (ibv_post_*)                   │
└─────────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────┐
│         RDMA Hardware Driver                │
│   - Mellanox driver                        │
│   - Intel OPA driver                        │
└─────────────────────────────────────────────┘
```

### 核心头文件
```c
#include <infiniband/verbs.h>    // Verbs API
#include <rdma/rdma_cma.h>      // RDMA CM (高级API)
```

---

## 2.2 设备发现与初始化

### 完整流程

```c
// 1. 获取设备列表
struct ibv_device **device_list;
int num_devices;
device_list = ibv_get_device_list(&num_devices);

// 2. 选择设备
struct ibv_device *device = device_list[0];

// 3. 打开设备
struct ibv_context *context;
context = ibv_open_device(device);

// 4. 查询设备信息
struct ibv_device_attr attr;
ibv_query_device(context, &attr);
```

### 代码详解

```c
// 获取所有RDMA设备
device_list = ibv_get_device_list(&num_devices);
if (!device_list) {
    perror("Failed to get device list");
    return -1;
}
printf("Found %d RDMA devices\n", num_devices);

// 打印设备名称
for (int i = 0; i < num_devices; i++) {
    printf("  %d: %s\n", i, ibv_get_device_name(device_list[i]));
}
```

---

## 2.3 资源创建流程

### 创建顺序

```
1. Protection Domain (PD)   - 保护域，隔离不同应用的资源
         │
         ▼
2. Completion Queue (CQ)    - 完成队列，存储操作完成通知
         │
         ▼
3. Queue Pair (QP)          - 队列对，通信基本单元
         │
         ▼
4. Memory Region (MR)       - 内存区域，注册可访问的内存
```

### Protection Domain (PD)

```c
// 分配PD - 所有RDMA资源的容器
struct ibv_pd *pd;
pd = ibv_alloc_pd(context);
if (!pd) {
    perror("Failed to allocate PD");
    return -1;
}
printf("PD allocated, handle=%d\n", pd->handle);
```

### Completion Queue (CQ)

```c
// 创建CQ - 存储操作完成事件
struct ibv_cq *cq;
int cq_size = 128;  // 完成队列大小

cq = ibv_create_cq(context, cq_size, NULL, NULL, 0);
if (!cq) {
    perror("Failed to create CQ");
    return -1;
}
printf("CQ created with %d entries\n", cq_size);
```

### Queue Pair (QP)

```c
// 创建QP - 通信基本单元
struct ibv_qp_init_attr qp_init_attr = {
    .send_cq = cq,           // 发送完成队列
    .recv_cq = cq,           // 接收完成队列
    .qp_type = IBV_QPT_RC,   // 可靠连接
    .cap = {
        .max_send_wr = 128,  // 最大发送WR数
        .max_recv_wr = 128,  // 最大接收WR数
        .max_send_sge = 1,   // 最大发送SGE数
        .max_recv_sge = 1,   // 最大接收SGE数
    },
};

struct ibv_qp *qp;
qp = ibv_create_qp(pd, &qp_init_attr);
if (!qp) {
    perror("Failed to create QP");
    return -1;
}
printf("QP created, number=%u\n", qp->qp_num);
```

### Memory Region (MR)

```c
// 注册内存 - 让RDMA设备可以访问
#define BUFFER_SIZE 4096
char *buffer = malloc(BUFFER_SIZE);

struct ibv_mr *mr;
mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, 
                 IBV_ACCESS_LOCAL_WRITE |    // 本地可写
                 IBV_ACCESS_REMOTE_READ |    // 远程可读
                 IBV_ACCESS_REMOTE_WRITE);   // 远程可写
if (!mr) {
    perror("Failed to register MR");
    return -1;
}
printf("MR registered: lkey=%u, rkey=%u\n", mr->lkey, mr->rkey);
```

### 内存权限标志

| 标志 | 说明 |
|------|------|
| `IBV_ACCESS_LOCAL_WRITE` | 本地CPU可写 |
| `IBV_ACCESS_REMOTE_READ` | 远程可读 |
| `IBV_ACCESS_REMOTE_WRITE` | 远程可写 |
| `IBV_ACCESS_ATOMIC` | 远程原子操作 |

---

## 2.4 QP状态机

### 状态转换图

```
         ┌─────────┐
         │ RESET   │ ←── 初始状态
         └────┬────┘
              │ ibv_modify_qp(IBV_QP_STATE)
              ▼
         ┌─────────┐
         │  INIT   │ ←── 已初始化，不能通信
         └────┬────┘
              │ ibv_modify_qp(IBV_QP_STATE)
              ▼
         ┌─────────┐
         │   RTR   │ ←── Ready to Receive，可以接收
         └────┬────┘
              │ ibv_modify_qp(IBV_QP_STATE)
              ▼
         ┌─────────┐
         │   RTS   │ ←── Ready to Send，可以发送
         └─────────┘
```

### 转换到INIT

```c
struct ibv_qp_attr attr = {
    .qp_state = IBV_QPS_INIT,
    .pkey_index = 0,
    .port_num = 1,
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                      IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE,
};

ibv_modify_qp(qp, &attr, IBV_QP_STATE);
```

### 转换到RTR (Ready to Receive)

```c
// 需要知道对端的QP号和LID
attr.qp_state = IBV_QPS_RTR;
attr.path_mtu = IBV_MTU_256;
attr.dest_qp_num = remote_qp_num;    // 对端QP号
attr.rq_psn = 0;
attr.max_dest_rd_atomic = 1;
attr.min_rnr_timer = 12;
attr.ah_attr.dlid = remote_lid;      // 对端LID
attr.ah_attr.sl = 0;
attr.ah_attr.port_num = 1;

ibv_modify_qp(qp, &attr, IBV_QP_STATE);
```

### 转换到RTS (Ready to Send)

```c
attr.qp_state = IBV_QPS_RTS;
attr.sq_psn = 0;
attr.timeout = 14;
attr.retry_cnt = 7;
attr.rnr_retry = 7;
attr.max_rd_atomic = 1;

ibv_modify_qp(qp, &attr, IBV_QP_STATE);
```

---

## 2.5 通信操作

### Post Send (发送)

```c
// 准备SGE (Scatter/Gather Element)
struct ibv_sge sge = {
    .addr = (uint64_t)buffer,
    .length = BUFFER_SIZE,
    .lkey = mr->lkey,
};

// 准备Work Request
struct ibv_send_wr wr = {
    .wr_id = 0,
    .sg_list = &sge,
    .num_sge = 1,
    .opcode = IBV_WR_SEND,           // 操作类型
    .send_flags = IBV_SEND_SIGNALED, // 需要完成通知
};

// 提交发送请求
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
        printf("Operation completed successfully\n");
        printf("  opcode: %d\n", wc.opcode);
        printf("  byte_len: %d\n", wc.byte_len);
    } else {
        printf("Operation failed: %s\n", ibv_wc_status_str(wc.status));
    }
}
```

### WC状态码

| 状态码 | 说明 |
|--------|------|
| `IBV_WC_SUCCESS` | 成功 |
| `IBV_WC_LOC_LEN_ERR` | 长度错误 |
| `IBV_WC_LOC_QP_OP_ERR` | QP操作错误 |
| `IBV_WC_WR_FLUSH_ERR` | 队列已刷新 |
| `IBV_WC_REM_INV_REQ_ERR` | 远程无效请求 |

---

## 2.6 资源销毁

### 销毁顺序（与创建顺序相反）

```c
// 1. 注销内存
ibv_dereg_mr(mr);
free(buffer);

// 2. 销毁QP
ibv_destroy_qp(qp);

// 3. 销毁CQ
ibv_destroy_cq(cq);

// 4. 释放PD
ibv_dealloc_pd(pd);

// 5. 关闭设备
ibv_close_device(context);

// 6. 释放设备列表
ibv_free_device_list(device_list);
```

---

## 2.7 完整示例运行验证

### 编译
```bash
cd ch02-verbs-api/01-initialization
make
```

### 运行（需要RDMA硬件）
```bash
./rdma_init
```

### 预期输出（有RDMA设备）
```
QP State Transition Example
============================

Found 1 RDMA device(s)
  0: mlx5_0
Opened device: mlx5_0
=== Port 1 Info ===
Link Layer: IB
State: ACTIVE
LID: 1
SM LID: 0
===================
Allocated PD
Created CQ with 128 entries
Created QP with num=1

=== Local QP Info ===
QP Number: 1
State: 0 (RESET)
Port: 1
PKey Index: 0
=====================

Step 1: RESET -> INIT

=== Local QP Info ===
QP Number: 1
State: 1 (INIT)
Port: 1
PKey Index: 0
=====================

Note: INIT->RTR requires remote QP info.
Complete state transition sequence:
  1. RESET -> INIT (done)
  2. INIT -> RTR (need remote QP num + LID)
  3. RTR -> RTS (done)

All resources cleaned up
```

### 如果没有RDMA设备
```
Found 0 RDMA device(s)
No RDMA devices found
```

---

## 2.8 常见错误排查

| 错误 | 原因 | 解决方法 |
|------|------|----------|
| `No RDMA devices found` | 没有RDMA硬件或驱动 | 检查 `ibv_devices` |
| `Failed to open device` | 权限不足 | 使用sudo运行 |
| `Failed to allocate PD` | 资源不足 | 检查内核限制 |
| `Failed to register MR` | 内存无效或权限不足 | 检查内存对齐和权限 |

---

## 练习题

1. **简答题**: 为什么要ibv_get_device_list而不是直接打开设备？
2. **概念题**: PD、QP、MR之间的关系是什么？
3. **画图题**: 画出QP状态转换图
4. **编程题**: 修改示例代码，打印设备属性信息

---

## 下一步

进入下一章：[第三章：QP与MR深入](../ch03-qp-mr/README.md)
