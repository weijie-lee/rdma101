# 第四章：RDMA 通信模式实践

## 学习目标

| 目标 | 说明 |
|------|------|
| 掌握四种通信模式 | Send/Recv、Write、Read、Atomic |
| 理解各模式适用场景 | 根据需求选择合适的模式 |
| 能够编写完整C/S程序 | 通过Socket交换连接信息 |
| 理解即时数据使用 | 掌握ImmData的使用 |
| 掌握RDMA CM编程 | 理解高级连接管理API |
| 理解多线程RDMA | 掌握线程安全编程要点 |
| 掌握性能优化技巧 | 学会提升RDMA性能 |

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
需要对方知道数据来了？
     │
     ├─ Yes → Send/Recv
     │
     └─ No → 主动推送还是被动拉取？
              │
              ├─ 推送 → RDMA Write
              └─ 拉取 → RDMA Read

需要原子操作？
     │
     └─ Yes → Atomic (CAS/FAA)
```

---

## 4.2 Send/Recv 模式

### 适用场景

- 消息驱动架构
- 不知道数据何时到达
- 需要即时数据(ImmData)
- 简单的请求/响应

### 示例代码

- [02-send-recv/send_recv.c](./02-send-recv/send_recv.c) - 跨机器通信
- [02-send-recv/01_loopback_send_recv.c](./02-send-recv/01_loopback_send_recv.c) - 本地Loopback

### 编译运行

```bash
cd 02-send-recv
make

# 终端1 - Server
./send_recv server

# 终端2 - Client
./send_recv client 192.168.1.100
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

### 示例代码

[01-rdma-write/rdma_write.c](./01-rdma-write/rdma_write.c)

---

## 4.4 RDMA Read 模式

### 适用场景

- 远程数据处理
- 分布式计算中的数据拉取
- 请求/响应模式
- 读取远程配置或状态

### 示例代码

[03-rdma-read/rdma_read.c](./03-rdma-read/rdma_read.c)

---

## 4.5 Atomics 原子操作

### 支持的操作

| 操作 | 说明 | 用途 |
|------|------|------|
| **Fetch & Add (FAA)** | 读取值，递增，写回 | 计数器、序列号生成 |
| **Compare & Swap (CAS)** | 比较相等则交换 | 分布式锁、乐观锁 |

### 示例代码

[04-atomic/atomic_ops.c](./04-atomic/atomic_ops.c)

---

## 4.6 即时数据 (ImmData)

- 32位即时数据，随WR一起发送
- 无需额外内存操作

---

## 4.7 连接建立方式

### 方式一：ibverbs (手动)

通过Socket交换QP信息后配置QP属性。

### 方式二：RDMA CM (高级API)

```c
// 服务器
rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
rdma_bind_addr(listen_id, &addr);
rdma_listen(listen_id, 1);
rdma_accept(event->id, NULL);

// 客户端
rdma_create_id(ec, &id, NULL, RDMA_PS_TCP);
rdma_resolve_addr(id, NULL, &server_addr, 1000);
rdma_connect(id, &conn_param);
```

---

## 4.8 多线程RDMA编程

### 线程模型

| 模型 | 特点 |
|------|------|
| 单QP多线程 | 需要加锁，竞争激烈 |
| 每线程独立QP | 无锁，性能好（推荐） |

### 资源管理策略

| 资源 | 策略 |
|------|------|
| QP | 每线程独立 |
| CQ | 每线程独立 |
| PD | 共享 |
| MR | 预注册，按需分片 |

---

## 4.9 性能优化技巧

### 1. 内存优化

```c
// 预注册内存池
#define POOL_SIZE (16 * 1024 * 1024)
void *pool = aligned_alloc(4096, POOL_SIZE);
struct ibv_mr *mr = ibv_reg_mr(pd, pool, POOL_SIZE, 
    IBV_ACCESS_REMOTE_WRITE);
```

### 2. 批处理

```c
// 链接多个WR
wr1.next = &wr2;
ibv_post_send(qp, &wr1, &bad_wr);
```

### 3. 异步操作

```c
// 批量提交
for (int i = 0; i < batch_size; i++) {
    ibv_post_send(qp, &wr_array[i], &bad_wr);
}
// 批量轮询
int completed = 0;
while (completed < batch_size) {
    completed += ibv_poll_cq(cq, batch_size - completed, &wc_array[completed]);
}
```

---

## 4.10 调试方法

### WC状态码

| 状态码 | 说明 |
|--------|------|
| `IBV_WC_SUCCESS` | 成功 |
| `IBV_WC_LOC_LEN_ERR` | 长度错误 |
| `IBV_WC_WR_FLUSH_ERR` | 队列已刷新 |
| `IBV_WC_REM_INV_REQ_ERR` | 远程无效请求 |

### 调试命令

```bash
# 查看设备
ibv_devices
ibv_devinfo -d mlx5_0

# 带宽测试
perftest -z -d mlx5_0 -c -n 100000 -s 4096

# 内核日志
dmesg | grep -i rdma
```

---

## 练习题

1. **选择题**: 场景：需要实现分布式锁，应该使用哪种操作？
2. **编程题**: 修改Send/Recv示例，添加ImmData传递消息类型
3. **分析题**: RDMA Write和Send/Recv的性能差异原因
4. **实践题**: 实现一个简单的分布式计数器

---

## 下一步

恭喜完成RDMA入门！接下来可以：

- 阅读[NVIDIA官方文档](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- 研究RDMA在分布式机器学习中的应用
- 搭建实际的RDMA测试环境
