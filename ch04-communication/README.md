# 第四章：RDMA 通信模式实践

## 学习目标

| 目标 | 说明 |
|------|------|
| 掌握四种通信模式 | Send/Recv、Write、Read、Atomic |
| 理解各模式适用场景 | 根据需求选择合适的模式 |
| 能够编写完整C/S程序 | 通过Socket交换连接信息 |
| 理解即时数据使用 | 掌握ImmData的使用 |

---

## 4.1 四种通信模式对比

| 模式 | 对方配合 | 典型用途 | 性能 | 复杂度 |
|------|----------|----------|------|--------|
| **Send/Recv** | 需要 | 消息传递 | 高 | 低 |
| **RDMA Write** | 不需要 | 数据推送 | 最高 | 中 |
| **RDMA Read** | 不需要 | 数据拉取 | 高 | 中 |
| **Atomic** | 不需要 | 计数器、锁 | 中 | 高 |

### 选择指南

```
┌─────────────────────────────────────────────────┐
│              通信模式选择                         │
├─────────────────────────────────────────────────┤
│                                                 │
│  需要对方知道数据来了？                          │
│     │                                          │
│     ├─ Yes → Send/Recv                         │
│     │                                          │
│     └─ No → 主动推送还是被动拉取？              │
│              │                                  │
│              ├─ 推送 → RDMA Write              │
│              └─ 拉取 → RDMA Read               │
│                                                 │
│  需要原子操作？                                  │
│     │                                          │
│     └─ Yes → Atomic (CAS/FAA)                  │
│                                                 │
└─────────────────────────────────────────────────┘
```

---

## 4.2 Send/Recv 模式

### 适用场景

- 消息驱动架构
- 不知道数据何时到达
- 需要即时数据(ImmData)
- 简单的请求/响应

### 通信流程

```
Server                          Client
  │                               │
  │◀────── Connect ──────────────│
  │─────── Accept ───────────────▶│
  │                               │
  │   post_recv()                  │
  │◀────── Send ─────────────────│
  │   处理数据                    │
  │   post_recv()                 │
  │                               │
  │─────── Send ─────────────────▶│
  │◀────── WC ───────────────────│
```

### 示例代码

参考：[02-send-recv/send_recv.c](./02-send-recv/send_recv.c)

### 编译运行

```bash
cd 02-send-recv
make

# 终端1 - Server
./send_recv server

# 终端2 - Client
./send_recv client 192.168.1.100
```

### 预期输出

**Server:**
```
Running as server
Server ready, waiting for messages...
Received: type=1, size=13, len=269
Got ACK
Done
```

**Client:**
```
Running as client
Client sending messages...
Got ACK
Done
```

---

## 4.3 RDMA Write 模式

### 适用场景

- 高性能数据传输
- 主动推送数据到远程
- 流式数据传输
- 无需对方感知的数据同步

### 特点

| 特性 | 说明 |
|------|------|
| 零CPU参与 | 远程主机无感知 |
| 发送方控制 | 何时写入由发送方决定 |
| 推送模式 | 主动将数据推给接收方 |
| 延迟最低 | 比Send/Recv更快 |

### 数据交换流程

```
1. Server启动，注册可写内存
2. 通过Socket交换: QP号 + LID + MR信息(addr/rkey)
3. Client执行RDMA Write
4. Server收到WC通知
```

### 编译运行

```bash
cd 01-rdma-write
make

# Server
./rdma_write server

# Client  
./rdma_write client <server_ip>
```

---

## 4.4 RDMA Read 模式

### 适用场景

- 远程数据处理
- 分布式计算中的数据拉取
- 请求/响应模式
- 读取远程配置或状态

### 特点

| 特性 | 说明 |
|------|------|
| 远程无感知 | 对方不知道你读取了数据 |
| 拉取模式 | 主动拉取远程数据 |
| 数据在本地 | 完成后数据在本地内存 |

### 通信流程

```
1. Server注册可读内存，交换MR信息
2. Client执行RDMA Read
3. 远程数据被拉到Client本地
4. Client处理数据
```

### 编译运行

```bash
cd 03-rdma-read
make

# Server
./rdma_read server

# Client
./rdma_read client <server_ip>
```

### 预期输出

**Server:**
```
Server: filling data...
Server: data ready at addr=140682304581632, rkey=...
Server: waiting...
```

**Client:**
```
Client: reading from server...
Remote: addr=140682304581632, rkey=...
Client: read data = "Hello from server! Time: 1234567890"
```

---

## 4.5 Atomics 原子操作

### 支持的操作

| 操作 | 说明 | 用途 |
|------|------|------|
| **Fetch & Add (FAA)** | 读取值，递增，写回 | 计数器、序列号生成 |
| **Compare & Swap (CAS)** | 比较相等则交换 | 分布式锁、乐观锁 |

### Fetch and Add

```c
// 原子地将远程计数器加1
struct ibv_send_wr wr = {
    .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD,
    .wr.atomic = {
        .remote_addr = counter_addr,
        .rkey = counter_rkey,
        .compare_add = 1,  // 加1
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};
// sge.addr 中包含旧值
```

### Compare and Swap

```c
// CAS: 如果值等于expected，则设为new
struct ibv_send_wr wr = {
    .opcode = IBV_WR_ATOMIC_CAS,
    .wr.atomic = {
        .remote_addr = lock_addr,
        .rkey = lock_rkey,
        .compare_add = expected_value,  // 期望值
        .swap = new_value,             // 新值
    },
};
// 成功时返回旧值
```

### 使用场景

**分布式计数器:**
```c
// 多个节点同时增加计数器
atomic_fetch_add(ctx, remote_addr, remote_rkey, 1);
printf("Current count: %lu\n", *(uint64_t*)result_buf);
```

**分布式锁:**
```c
// 尝试获取锁
while (atomic_cas(ctx, lock_addr, remote_rkey, 0, 1) != 0) {
    // 锁被占用，等待
    sleep(1);
}
// 获取到锁

// 释放锁
atomic_fetch_add(ctx, lock_addr, remote_rkey, -1);
```

### 编译运行

```bash
cd 04-atomic
make

# Server
./atomic_ops server

# Client
./atomic_ops client <server_ip>
```

### 预期输出

**Server:**
```
Server: counter at addr=..., rkey=...
Server: initial counter = 0
Server: final counter = 100
```

**Client:**
```
Client: performing atomic operations...
Remote: addr=..., rkey=...
Client: FAA +1, old value = 0
Client: FAA +10, old value = 1
Client: CAS expected=11, swapped=NO
```

---

## 4.6 即时数据 (ImmData)

### 什么是ImmData？

- 32位即时数据
- 随WR一起发送
- 无需额外内存操作

### 使用示例

```c
// 发送端
uint32_t imm = 12345;
struct ibv_send_wr wr = {
    .opcode = IBV_WR_SEND_WITH_IMM,
    .imm_data = htobe32(imm),  // 网络字节序
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};

// 接收端
if (wc.opcode == IBV_WC_SEND_WITH_IMM) {
    uint32_t imm = be32toh(wc.imm_data);
    printf("Received immediate data: %u\n", imm);
}
```

### 使用场景

- 消息类型标识
- 小数据传递（如操作码）
- 避免额外内存操作

---

## 4.7 连接建立方式

### 方式一：ibverbs (手动)

```c
// 1. 交换LID/QP号（通过Socket）
recv(sock, &remote_lid, sizeof(remote_lid), 0);
recv(sock, &remote_qp, sizeof(remote_qp), 0);

// 2. 交换MR信息
recv(sock, &remote_addr, sizeof(remote_addr), 0);
recv(sock, &remote_rkey, sizeof(remote_rkey), 0);

// 3. 配置QP属性
modify_qp_to_rtr(qp, remote_qp, remote_lid);
modify_qp_to_rts(qp);
```

### 方式二：RDMA CM (高级API)

```c
// 服务器
rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
rdma_bind_addr(listen_id, &addr);
rdma_listen(listen_id, 1);
rdma_get_cm_event(ec, &event);
rdma_accept(event->id, NULL);

// 客户端
rdma_create_id(ec, &id, NULL, RDMA_PS_TCP);
rdma_resolve_addr(id, NULL, &server_addr, 1000);
rdma_connect(id, &conn_param);
```

---

## 4.8 完整测试流程

### 环境要求

- 两台支持RDMA的机器
- IB网卡或RoCE配置
- Mellanox驱动

### 测试步骤

```bash
# 1. 检查RDMA设备
ibv_devices

# 2. 检查网络连接
ibv_devinfo -d mlx5_0

# 3. 运行示例
cd 01-rdma-write
./rdma_write server &   # 机器A

cd 01-rdma-write  
./rdma_write client <A_ip>  # 机器B

# 4. 检查性能
perftest -z -d mlx5_0 -c -n 100000 -s 4096
```

---

## 练习题

1. **选择题**: 场景：需要实现分布式锁，应该使用哪种操作？
2. **编程题**: 修改Send/Recv示例，添加ImmData传递消息类型
3. **分析题**: RDMA Write和Send/Recv的性能差异原因
4. **实践题**: 实现一个简单的分布式计数器

---

## 下一步

进入下一章：[第五章：高级主题](../ch05-advanced/README.md)
