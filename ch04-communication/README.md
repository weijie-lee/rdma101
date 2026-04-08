# 第四章：RDMA 通信模式实践

## 学习目标
- 掌握RDMA四种通信操作
- 理解四种模式的适用场景
- 能够编写完整的客户端/服务器程序
- 理解连接建立的流程

## 4.1 四种通信模式对比

| 模式 | 对方配合 | 典型用途 | 性能 |
|------|----------|----------|------|
| **Send/Recv** | 需要 | 消息传递 | 高 |
| **RDMA Write** | 不需要 | 数据推送 | 最高 |
| **RDMA Read** | 不需要 | 数据拉取 | 高 |
| **Atomic** | 不需要 | 计数器、锁 | 中 |

---

## 4.2 Send/Recv 模式

### 适用场景
- 消息驱动架构
- 不知道数据何时到达
- 需要即时数据(ImmData)

### 通信流程
```
Server                          Client
  │                               │
  │◀────── Connect Request ──────│
  │────── Connect Reply ─────────▶│
  │                               │
  │  post_recv()                  │
  │◀────── Send ─────────────────│
  │  处理数据                     │
  │                               │
  │  post_recv()                  │
  │                               │
```

### 示例代码
参考：[02-send-recv/send_recv.c](./02-send-recv/send_recv.c)

```bash
# 编译
cd 02-send-recv
make

# 运行 (需要两台机器)
# Server: ./send_recv server
# Client: ./send_recv client <server_ip>
```

---

## 4.3 RDMA Write 模式

### 适用场景
- 高性能数据传输
- 主动推送数据到远程
- 流式数据传输

### 通信流程
```
Server                          Client
  │                               │
  │◀────── Connect ──────────────│
  │                               │
  │  注册远程可写内存              │
  │  (交换 rkey + addr)          │
  │                               │
  │◀──RDMA Write(local→remote)───│
  │                               │
  │  通知应用数据已到达            │
  │                               │
```

### 示例代码
参考：[01-rdma-write/rdma_write.c](./01-rdma-write/rdma_write.c)

```bash
# 编译
cd 01-rdma-write
make
```

---

## 4.4 RDMA Read 模式

### 适用场景
- 远程数据处理
- 分布式计算中的数据拉取
- 请求-响应模式

### 通信流程
```
Server                          Client
  │                               │
  │◀────── Connect ──────────────│
  │                               │
  │  注册远程可读内存              │
  │  (交换 rkey + addr)          │
  │                               │
  │◀──RDMA Read(remote→local)────│
  │  (携带本地MR信息)             │
  │                               │
  │  读取本地内存                 │
  │  (数据已在这里)               │
  │                               │
```

### 示例代码
参考：[03-rdma-read/rdma_read.c](./03-rdma-read/rdma_read.c)

---

## 4.5 Atomics 原子操作

### 支持的操作
| 操作 | 说明 | 用途 |
|------|------|------|
| **Fetch & Add** | 读取值，递增，写回 | 计数器、序列号 |
| **Compare & Swap** | 比较相等则交换 | 分布式锁 |

### Fetch and Add
```c
struct ibv_send_wr wr = {
    .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
    .wr.atomic = {
        .remote_addr = counter_addr,
        .rkey = counter_rkey,
        .compare_add = 1,  // 加上1
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};
// sge.addr 中包含旧值
```

### Compare and Swap
```c
struct ibv_send_wr wr = {
    .opcode = IBV_WR_ATOMIC_CAS,
    .wr.atomic = {
        .remote_addr = lock_addr,
        .rkey = lock_rkey,
        .compare_add = expected_value,  // 期望值
        .swap = new_value,              // 新值
    },
};
```

### 示例代码
参考：[04-atomic/atomic_ops.c](./04-atomic/atomic_ops.c)

---

## 4.6 即时数据 (ImmData)

### 什么是ImmData？
- 32位即时数据，随WR一起发送
- 无需额外内存操作

### 使用示例
```c
// 发送带ImmData
uint32_t imm = 12345;
struct ibv_send_wr wr = {
    .opcode = IBV_WR_SEND_WITH_IMM,
    .imm_data = htobe32(imm),  // 网络字节序
    ...
};

// 接收端
if (wc.opcode == IBV_WC_SEND_WITH_IMM) {
    uint32_t imm = be32toh(wc.imm_data);
    printf("Received imm: %u\n", imm);
}
```

---

## 4.7 连接建立方式

### 方式一：基于ibverbs (手动)
1. 交换LID/QP号
2. 配置QP属性
3. 修改QP状态

### 方式二：基于RDMA CM
```c
// 服务器
struct rdma_cm_id *listen_id;
rdma_create_id(event_channel, &listen_id, NULL, RDMA_PS_TCP);
rdma_bind_addr(listen_id, &addr);
rdma_listen(listen_id, 1);

// 客户端
rdma_create_id(event_channel, &id, NULL, RDMA_PS_TCP);
rdma_resolve_addr(id, NULL, &server_addr, 1000);
rdma_resolve_route(id, 1000);
rdma_connect(id, &conn_param);
```

---

## 📂 示例代码汇总

| 目录 | 文件 | 说明 |
|------|------|------|
| 01-rdma-write | rdma_write.c | RDMA Write推送数据 |
| 02-send-recv | send_recv.c | Send/Recv消息传递 |
| 03-rdma-read | rdma_read.c | RDMA Read拉取数据 |
| 04-atomic | atomic_ops.c | 原子操作计数器 |

### 运行测试

```bash
# 需要两台支持RDMA的机器

# 1. RDMA Write 测试
# 机器A: ./rdma_write server
# 机器B: ./rdma_write client <A_ip>

# 2. Send/Recv 测试  
# 机器A: ./send_recv server
# 机器B: ./send_recv client <A_ip>

# 3. RDMA Read 测试
# 机器A: ./rdma_read server
# 机器B: ./rdma_read client <A_ip>

# 4. Atomic 测试
# 机器A: ./atomic_ops server
# 机器B: ./atomic_ops client <A_ip>
```

---

## 练习题

1. Send/Recv和RDMA Write的适用场景有何不同？
2. RDMA Read完成后，数据在哪里？
3. 如何用原子操作实现分布式锁？
4. ImmData有什么限制？
5. 哪种模式延迟最低？

---

## 下一步

进入下一章：[第五章：高级主题](../ch05-advanced/README.md)
