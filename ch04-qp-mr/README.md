# 第四章：QP与MR深入

## 学习目标

| 目标 | 说明 |
|------|------|
| 深入理解Queue Pair | 掌握QP类型、SGE、WR结构 |
| 掌握Memory Region | 理解虚拟地址到物理地址的转换 |
| 理解RDMA操作 | 掌握RDMA Write/Read的原理 |
| 理解CQ工作方式 | 掌握轮询和事件两种模式 |

---

## 3.1 Queue Pair 详解

### QP类型

| 类型 | 名称 | 特点 | 适用场景 |
|------|------|------|----------|
| **RC** | Reliable Connection | 保证顺序和交付 | 推荐使用 |
| **UC** | Unreliable Connection | 仅顺序交付，无重传 | 特定场景 |
| **UD** | Unreliable Datagram | 无连接，类似UDP | 多播、广播 |

**推荐使用 RC (Reliable Connection)**，最常用且功能完整。

### QP编号

- 每个QP有一个唯一的数字标识
- 范围：0 ~ 2^24-1
- 本地QP号在创建时分配
- 远端QP号需要通过连接过程交换

### QP与CQ的关系

```
┌─────────────────────────────────────────┐
│              Queue Pair                  │
│  ┌───────────────┐  ┌───────────────┐  │
│  │  Send Queue  │  │ Receive Queue │  │
│  │    (SQ)      │  │     (RQ)     │  │
│  └───────┬───────┘  └───────┬───────┘  │
│          │                  │           │
│          └────────┬─────────┘           │
│                   ▼                    │
│          ┌───────────────┐             │
│          │  Completion   │             │
│          │    Queue      │             │
│          │     (CQ)      │             │
│          └───────────────┘             │
└─────────────────────────────────────────┘
```

---

## 3.2 Scatter/Gather Entry (SGE)

### 什么是SGE？

SGE允许**一次RDMA操作传输多个不连续的内存块**：

```
传统模式:
[████████████]

SGE模式:
[██] + [██████] + [██]
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
// 单元素SGE
struct ibv_sge sge = {
    .addr = (uint64_t)buffer,
    .length = BUFFER_SIZE,
    .lkey = mr->lkey,
};

// 多元素SGE (用于非连续内存)
struct ibv_sge sge[2] = {
    {.addr = (uint64_t)buf1, .length = 100, .lkey = mr->lkey},
    {.addr = (uint64_t)buf2, .length = 200, .lkey = mr->lkey},
};

struct ibv_send_wr wr = {
    .sg_list = sge,
    .num_sge = 2,  // 使用2个SGE
    ...
};
```

---

## 3.3 Memory Region 深入

### 虚拟地址 vs 物理地址

```
用户程序          内核              RDMA网卡
   │               │                   │
   │ 虚拟地址      │                   │
   ├──────────────▶│                   │
   │               │ 物理地址          │
   │               ├─────────────────▶│
   │               │                   │ DMA
   │               │                   ├──────────▶ 网络
```

- 操作系统使用**虚拟地址**
- RDMA网卡使用**物理地址**进行DMA
- **MR注册过程**将虚拟地址转换为物理地址

### Key (密钥)

| Key | 说明 | 用途 |
|-----|------|------|
| **lkey** | 本地密钥 | 本地操作（发送、接收） |
| **rkey** | 远程密钥 | 远端操作（RDMA Write/Read） |

```c
// 本地操作使用lkey
sge.lkey = mr->lkey;

// 远端操作需要知道对方的rkey
wr.rdma.rkey = remote_mr->rkey;
```

### 预注册内存池

频繁注册/注销内存有较大开销，建议预注册：

```c
#define POOL_SIZE (16 * 1024 * 1024)  // 16MB
char *memory_pool = malloc(POOL_SIZE);

// 一次性注册
struct ibv_mr *pool_mr = ibv_reg_mr(pd, memory_pool, POOL_SIZE,
                                        IBV_ACCESS_REMOTE_WRITE);

// 重复使用这块内存的不同区域
void *alloc_buf(size_t size) {
    static size_t offset = 0;
    void *ptr = memory_pool + offset;
    offset += size;
    return ptr;
}
```

---

## 3.4 RDMA Write 操作

### 原理

直接写入远程主机的内存，**无需远程CPU参与**。

```
本地                              远程
┌─────────┐                      ┌─────────┐
│ Send Q  │───RDMA Write────────▶│ Memory  │
└─────────┘                      └─────────┘
    │                                ▲
    │    WC (完成通知)               │
    └────────────────────────────────┘
```

### 代码示例

```c
// 发送端
struct ibv_sge sge = {
    .addr = (uint64_t)local_buffer,
    .length = data_size,
    .lkey = local_mr->lkey,
};

struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_WRITE,
    .wr.rdma = {
        .remote_addr = remote_buffer_addr,  // 远程地址
        .rkey = remote_buffer_rkey,         // 远程密钥
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};

ibv_post_send(qp, &wr, &bad_wr);

// 等待完成
poll_cq(cq, &wc);
```

### 特点

| 特性 | 说明 |
|------|------|
| 对方配合 | 不需要对方CPU参与 |
| 发送方控制 | 发送方决定何时写入 |
| 推送模式 | 适合主动推送数据到远程 |
| 性能 | 延迟最低 |

---

## 3.5 RDMA Read 操作

### 原理

直接读取远程主机的内存，**数据拉取到本地**：

```
本地                              远程
┌─────────┐                      ┌─────────┐
│ Send Q  │◀───RDMA Read────────│ Memory  │
└─────────┘                      └─────────┘
    │                                │
    │    WC + 数据到本地             │
    └────────────────────────────────┘
```

### 代码示例

```c
// 读取端
struct ibv_sge sge = {
    .addr = (uint64_t)local_buffer,   // 数据将放在这里
    .length = data_size,
    .lkey = local_mr->lkey,
};

struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_READ,
    .wr.rdma = {
        .remote_addr = remote_buffer_addr,  // 远程地址
        .rkey = remote_buffer_rkey,         // 远程密钥
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};

ibv_post_send(qp, &wr, &bad_wr);

// 等待完成，然后local_buffer包含远程数据
poll_cq(cq, &wc);
```

### 特点

| 特性 | 说明 |
|------|------|
| 对方配合 | 远程主机无感知 |
| 拉取模式 | 适合主动拉取远程数据 |
| 数据位置 | 完成后数据在本地内存 |

---

## 3.6 Send/Recv 操作

### 原理

传统的消息传递模式，**需要双方配合**：

```
发送方                           接收方
  │                                │
  │───post_send()────────────────▶│
  │                                │
  │        post_recv()             │
  │◀───────────────────────────────│
  │                                │
  │────post_send()───────────────▶│───WC
  │◀──────WC────────────────────────│
```

### 对比表

| 特性 | Send/Recv | RDMA Write |
|------|-----------|------------|
| 对方配合 | 需要 | 不需要 |
| 控制权 | 接收方 | 发送方 |
| 即时数据 | 支持(ImmData) | 不支持 |
| 典型用途 | 消息通知 | 数据传输 |

---

## 3.7 完成队列 (CQ) 深入

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
            printf("Error: %s\n", ibv_wc_status_str(wc.status));
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
                printf("RDMA Read completed, got %d bytes\n", wc.byte_len);
                break;
        }
    }
}
```

### 事件模式

```c
// 创建带事件的CQ
struct ibv_comp_channel *channel;
channel = ibv_create_comp_channel(context);
cq = ibv_create_cq(context, cq_size, NULL, channel, 0);

// 等待事件
struct ibv_cq *ev_cq;
void *ev_ctx;
ibv_get_cq_event(channel, &ev_cq, &ev_ctx);

// 处理完成
ibv_ack_cq_event(ev_cq, 1);
```

---

## 3.8 运行验证

### 编译
```bash
cd ch04-communication/01-rdma-write
make

cd ../03-rdma-read
make
```

### 测试RDMA Read

```bash
# 机器A (Server)
./rdma_read server

# 机器B (Client)
./rdma_read client <A_ip>
```

### 预期输出

**Server端:**
```
Server: filling data...
Server: data ready at addr=..., rkey=...
Server: waiting...
```

**Client端:**
```
Client: reading from server...
Remote: addr=..., rkey=...
Client: read data = "Hello from server! Time: ..."
```

---

## 练习题

1. **概念题**: RC和UD QP的区别是什么？
2. **编程题**: 修改RDMA Read示例，实现双向数据交换
3. **分析题**: 为什么RDMA Write延迟比Send/Recv低？
4. **实践题**: 使用多SGE实现非连续内存传输

---

## 下一步

进入下一章：[第五章：通信模式实践](../ch05-communication/README.md)
