# 第五章：RDMA 高级主题

## 学习目标

| 目标 | 说明 |
|------|------|
| 掌握RDMA CM编程 | 理解高级连接管理API |
| 理解多线程RDMA | 掌握线程安全编程要点 |
| 掌握性能优化技巧 | 学会提升RDMA性能 |
| 掌握调试方法 | 能够排查RDMA问题 |

---

## 5.1 RDMA CM 编程

### 什么是RDMA CM？

**RDMA Communication Manager** - 建立在libibverbs之上的高级API，简化连接建立过程。

### 对比

| 特性 | Verbs API | RDMA CM |
|------|------------|---------|
| 复杂度 | 高 | 低 |
| 控制力 | 完整 | 有限 |
| 连接管理 | 手动 | 自动 |
| 代码量 | 多 | 少 |

### 服务器端流程

```c
#include <rdma/rdma_cma.h>

// 1. 创建事件通道
struct rdma_event_channel *cm_channel;
cm_channel = rdma_create_event_channel();

// 2. 创建监听ID
struct rdma_cm_id *listen_id;
rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_TCP);

// 3. 绑定地址
struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(5001),
    .sin_addr.s_addr = INADDR_ANY,
};
rdma_bind_addr(listen_id, (struct sockaddr*)&addr);

// 4. 监听
rdma_listen(listen_id, 1);

// 5. 等待连接
struct rdma_cm_event *event;
rdma_get_cm_event(cm_channel, &event);

if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
    struct rdma_cm_id *conn_id = event->id;
    
    // 获取连接参数
    struct ibv_qp_init_attr qp_attr;
    rdma_conn_param conn_param = {0};
    
    // 接受连接
    rdma_accept(conn_id, &conn_param);
}

// 等待连接建立
rdma_get_cm_event(cm_channel, &event);
```

### 客户端流程

```c
// 1. 创建ID
rdma_create_id(cm_channel, &conn_id, NULL, RDMA_PS_TCP);

// 2. 解析地址
struct sockaddr_in server_addr = {
    .sin_family = AF_INET,
    .sin_port = htons(5001),
    .sin_addr.s_addr = inet_addr("192.168.1.100"),
};
rdma_resolve_addr(conn_id, NULL, (struct sockaddr*)&server_addr, 1000);

// 3. 解析路由
rdma_resolve_route(conn_id, 1000);

// 4. 连接
struct rdma_conn_param conn_param = {
    .initiator_depth = 1,
    .responder_resources = 1,
};
rdma_connect(conn_id, &conn_param);

// 5. 等待连接建立
rdma_get_cm_event(cm_channel, &event);
```

---

## 5.2 多线程 RDMA 编程

### 线程模型

#### 模型1：单QP多线程

```c
// 多个线程共享一个QP - 需要加锁
pthread_mutex_t qp_lock;

void *thread_func(void *arg) {
    pthread_mutex_lock(&qp_lock);
    ibv_post_send(qp, &wr, &bad_wr);
    pthread_mutex_unlock(&qp_lock);
}
```

**问题**：竞争激烈，性能差

#### 模型2：每线程独立QP（推荐）

```c
// 每个线程拥有独立的QP
struct thread_ctx {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    // ...
};

void *thread_func(void *arg) {
    struct thread_ctx *ctx = arg;
    // 使用自己的QP，无锁
    ibv_post_send(ctx->qp, &wr, &bad_wr);
}
```

### 资源管理策略

| 资源 | 策略 | 说明 |
|------|------|------|
| QP | 每线程独立 | 避免竞争 |
| CQ | 每线程独立 | 避免竞争 |
| PD | 共享 | 可以共享 |
| MR | 预注册，按需分片 | 避免重复注册 |

### 线程安全要点

```c
// 1. 内存同步
volatile int ready = 1;
// 或
__sync_synchronize();

// 2. 避免false sharing
struct foo {
    int value1;  // 线程1
    char pad[64]; // 填充到缓存行
    int value2;  // 线程2
};
```

---

## 5.3 性能优化技巧

### 1. 内存优化

#### 预注册内存池

```c
#define PAGE_SIZE 4096
#define POOL_SIZE (16 * 1024 * 1024)  // 16MB

void *pool = aligned_alloc(PAGE_SIZE, POOL_SIZE);
struct ibv_mr *mr = ibv_reg_mr(pd, pool, POOL_SIZE,
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

// 分配子块
void *alloc_buf(size_t size) {
    static size_t offset = 0;
    void *ptr = pool + offset;
    offset = (offset + size + 255) & ~255;  // 256字节对齐
    return ptr;
}
```

#### 内存对齐

```c
// 对齐到页面边界
posix_memalign(&buffer, 4096, size);

// 或
void *buffer;
buffer = memalign(4096, size);
```

### 2. 批处理

#### 链接多个WR

```c
struct ibv_send_wr wr1 = { ... };
struct ibv_send_wr wr2 = { ... };
wr1.next = &wr2;  // 链接

// 一次提交多个
ibv_post_send(qp, &wr1, &bad_wr);
```

#### 批量轮询CQ

```c
struct ibv_wc wc_array[32];
int num = ibv_poll_cq(cq, 32, wc_array);

for (int i = 0; i < num; i++) {
    // 处理每个完成
}
```

### 3. 异步操作

```c
// 批量提交
for (int i = 0; i < batch_size; i++) {
    ibv_post_send(qp, &wr_array[i], &bad_wr);
}

// 稍后批量轮询
int completed = 0;
while (completed < batch_size) {
    int n = ibv_poll_cq(cq, batch_size - completed, &wc_array[completed]);
    completed += n;
}
```

### 4. 性能对比

| 优化项 | 优化前 | 优化后 |
|--------|--------|--------|
| 内存注册 | 每次注册 | 预注册池 |
| WR提交 | 单个提交 | 批量提交 |
| CQ轮询 | 单个轮询 | 批量轮询 |
| 内存对齐 | 任意对齐 | 页面对齐 |

---

## 5.4 硬件配置

### 检查RDMA设备

```bash
# 列出设备
ibv_devices

# 查看设备详情
ibv_devinfo -d mlx5_0

# 查看端口状态
ibv_port_state mlx5_0 1
```

### 配置Mellanox网卡

```bash
# 查看当前配置
mlxconfig -d /dev/mst/mt4123_pciconf0 query

# 开启RoCE v2
mlxconfig -d /dev/mst/mt4123_pciconf0 set ROCE_V2_ENABLED=1

# 设置MTU
mlxconfig -d /dev/mst/mt4123_pciconf0 set LINK_TYPE_ETH=2
```

### Subnet Manager (IB网络)

```bash
# 检查SM状态
opensm status

# 启动SM
opensm -B
```

---

## 5.5 调试工具

### ibv_* 工具

```bash
# 设备信息
ibv_devinfo

# 带宽测试
perftest -z -d mlx5_0 -c -n 100000 -s 4096

# 延迟测试
perftest -z -d mlx5_0 -t 1 -n 1000
```

### perf 工具

```bash
# 记录RDMA事件
perf record -e ib_rdma_* -a

# 查看报告
perf report
```

### 内核日志

```bash
# 查看RDMA相关日志
dmesg | grep -i rdma
dmesg | grep -i mlx5

# 实时监控
dmesg -w | grep rdma
```

---

## 5.6 常见错误处理

### WC状态码

| 状态码 | 说明 | 常见原因 |
|--------|------|----------|
| `IBV_WC_SUCCESS` | 成功 | - |
| `IBV_WC_LOC_LEN_ERR` | 长度错误 | SGE长度超限 |
| `IBV_WC_LOC_QP_OP_ERR` | QP操作错误 | QP状态不对 |
| `IBV_WC_WR_FLUSH_ERR` | 队列已刷新 | 对端断开 |
| `IBV_WC_REM_INV_REQ_ERR` | 远程无效请求 | rkey无效 |
| `IBV_WC_REM_ACCESS_ERR` | 远程访问错误 | 权限不足 |

### 调试步骤

```c
// 1. 检查返回码
if (ibv_post_send(qp, &wr, &bad_wr)) {
    perror("post_send failed");
}

// 2. 检查WC状态
if (wc.status != IBV_WC_SUCCESS) {
    printf("WC error: %s\n", ibv_wc_status_str(wc.status));
}

// 3. 检查QP状态
enum ibv_qp_state state;
ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr);
printf("QP state: %d\n", state);
```

---

## 5.7 最佳实践

### 开发建议

1. **使用RC QP** - 最稳定可靠
2. **预注册内存** - 避免运行时开销
3. **错误检查** - 始终检查返回值
4. **资源清理** - 正确释放所有资源
5. **日志记录** - 便于调试

### 性能建议

1. **批量操作** - 减少系统调用
2. **零拷贝** - 避免不必要的数据移动
3. **对齐** - 内存对齐到页面边界
4. **无锁设计** - 避免锁竞争
5. **预取** - 提前准备下一批操作

---

## 5.8 运行验证

### 基本测试

```bash
# 1. 检查设备
ibv_devices

# 2. 测试连接
rdma_xsrq -d mlx5_0 -n 1000

# 3. 带宽测试
# 机器A
perftest -z -d mlx5_0 -s 4096 -n 1000000 --run_infinitely

# 机器B
perftest -z -d mlx5_0 -c -s 4096 -n 1000000 --run_infinitely
```

### 预期输出

```
RDMA_Write BW test
-----------------------
Bytes             : 4096
Iterations       : 1000000
Inline size     : 0
Port            : 1
CQ Moderation   : 100
-------------------
Mellanox Technologies ...
Link : Up
-----------------------
Total loops     : 1000000
Total data(MB) : 4096.000000
Avg latency(us): 0.721
Peak bandwidth  : 5.637 GB/s
```

---

## 练习题

1. **概念题**: RDMA CM相比ibverbs的优势是什么？
2. **编程题**: 实现多线程RDMA数据传输
3. **分析题**: 如何优化RDMA延迟？
4. **实践题**: 使用perftest测试网络带宽

---

## 下一步

恭喜完成RDMA入门教程！接下来可以：

- 阅读[NVIDIA官方文档](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- 研究RDMA在分布式机器学习中的应用
- 搭建实际的RDMA测试环境
