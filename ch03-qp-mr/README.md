# 第三章：QP与MR深入

## 学习目标
- 深入理解Queue Pair的工作原理
- 掌握Memory Region的高级特性
- 理解SGE scatter/gather的作用
- 能够进行RDMA读写操作

## 3.1 Queue Pair 详解

### QP类型

| 类型 | 说明 | 特点 |
|------|------|------|
| **RC (Reliable Connection)** | 可靠连接 | 保证顺序和交付，类似于TCP |
| **UC (Unreliable Connection)** | 不可靠连接 | 无重传，仅顺序交付 |
| **UD (Unreliable Datagram)** | 不可靠数据报 | 无连接，类似UDP |

**推荐使用 RC**，最常用且功能完整。

### QP编号
- 每个QP有一个唯一的数字标识 (0 ~ 2^24-1)
- 本地QP号在创建时分配
- 远端QP号需要通过通信建立过程交换

### Work Request 结构
```c
struct ibv_send_wr {
    uint64_t wr_id;           // 用户自定义标识
    struct ibv_sge *sg_list;  // Scatter/Gather列表
    int num_sge;              // SGE数量
    enum ibv_wr_opcode opcode; // 操作类型
    int send_flags;           // 发送标志
    // ... 其他字段
};
```

---

## 3.2 Scatter/Gather Entry (SGE)

### 什么是SGE？
SGE允许一次RDMA操作传输多个不连续的内存块：

```
传统: [██████]
SGE:  [██] + [████] + [█]
```

### SGE结构
```c
struct ibv_sge {
    uint64_t addr;      // 内存地址
    uint32_t length;   // 长度
    uint32_t lkey;     // 本地MR的lkey
};
```

### 使用示例
```c
struct ibv_sge sge = {
    .addr = (uint64_t)buffer,
    .length = BUFFER_SIZE,
    .lkey = mr->lkey,
};

struct ibv_send_wr wr = {
    .sg_list = &sge,
    .num_sge = 1,
    .opcode = IBV_WR_RDMA_WRITE,
    .wr.rdma = {
        .rkey = remote_mr->rkey,  // 远程内存的rkey
        .remote_addr = remote_addr, // 远程内存地址
    },
};
```

---

## 3.3 Memory Region 深入

### 虚拟地址 vs 物理地址
- RDMA网卡使用物理地址访问内存
- 操作系统使用虚拟地址
- **MR注册过程**：将虚拟地址转换为物理地址

### Key (密钥)
- **lkey** - 本地密钥，用于本地操作
- **rkey** - 远程密钥，用于远端操作

### 预注册内存池
频繁注册/注销内存有开销，建议：
```c
// 预注册大块内存池
#define POOL_SIZE (1024 * 1024 * 16)  // 16MB
char *memory_pool = malloc(POOL_SIZE);
ibv_reg_mr(pd, memory_pool, POOL_SIZE, permissions);

// 重复使用这块内存，避免重复注册
```

---

## 3.4 RDMA Write 操作

### 原理
直接写入远程主机的内存，无需远程CPU参与。

```
本地                              远程
┌─────────┐                      ┌─────────┐
│ Send Q  │───RDMA Write────────▶│ Memory  │
└─────────┘                      └─────────┘
```

### 代码示例
```c
// 发送端
struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_WRITE,
    .wr.rdma = {
        .remote_addr = remote_buffer_addr,
        .rkey = remote_buffer_rkey,
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};
ibv_post_send(qp, &wr, &bad_wr);
```

### 特点
- ✅ 不需要远程CPU参与
- ✅ 发送端控制何时写入
- ✅ 适合推送数据到远程

---

## 3.5 RDMA Read 操作

### 原理
直接读取远程主机的内存，数据拉取到本地。

```
本地                              远程
┌─────────┐                      ┌─────────┐
│ Send Q  │◀───RDMA Read────────│ Memory  │
└─────────┘                      └─────────┘
```

### 代码示例
```c
// 读取端
struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_READ,
    .wr.rdma = {
        .remote_addr = remote_buffer_addr,
        .rkey = remote_buffer_rkey,
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};
ibv_post_send(qp, &wr, &bad_wr);
```

### 特点
- ✅ 远程主机无感知
- ✅ 适合拉取远程数据
- ⚠️ 需要等待完成才能使用数据

---

## 3.6 Send/Recv 操作

### 原理
传统的消息传递模式，需要双方配合。

```
发送方 post_send ───▶│ Receive Q│───接收方处理
                      │         │
```

### Send端
```c
struct ibv_send_wr wr = {
    .opcode = IBV_WR_SEND,
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};
ibv_post_send(qp, &wr, &bad_wr);
```

### Recv端
```c
struct ibv_recv_wr wr = {
    .sg_list = &sge,
    .num_sge = 1,
};
ibv_post_recv(qp, &wr, &bad_wr);
```

### 对比
| 特性 | Send/Recv | RDMA Write |
|------|-----------|------------|
| 需要对方配合 | 是 | 否 |
| 发送方控制 | 否 | 是 |
| 即时数据 | 支持 | 不支持 |
| 典型场景 | 消息传递 | 数据传输 |

---

## 3.7 完成队列 (CQ)

### Polling模式
```c
while (1) {
    struct ibv_wc wc;
    int ne = ibv_poll_cq(cq, 1, &wc);
    
    if (ne < 0) {
        perror("poll CQ failed");
        break;
    }
    
    if (ne > 0) {
        if (wc.status != IBV_WC_SUCCESS) {
            printf("WC error: %s\n", ibv_wc_status_str(wc.status));
            continue;
        }
        
        switch (wc.opcode) {
            case IBV_WC_SEND:
                printf("Send completed\n");
                break;
            case IBV_WC_RDMA_WRITE:
                printf("RDMA Write completed\n");
                break;
            case IBV_WC_RDMA_READ:
                printf("RDMA Read completed\n");
                break;
        }
    }
}
```

### 事件模式
```c
// 创建带事件的CQ
cq = ibv_create_cq(context, cq_size, NULL, event_channel, 0);

// 等待事件
ibv_get_cq_event(event_channel, &cq_event);
ibv_ack_cq_event(cq, 1);
```

---

## 练习题

1. RC和UD QP的区别是什么？
2. SGE的作用是什么？
3. lkey和rkey的区别？
4. RDMA Write和Send的区别？
5. 如何判断RDMA操作是否成功？

---

## 下一步

进入下一章：[第四章：通信模式实践](../ch04-communication/README.md)
