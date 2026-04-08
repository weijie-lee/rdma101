# 第五章：RDMA 高级主题

## 学习目标
- 掌握RDMA CM编程
- 理解多线程编程要点
- 学会性能优化技巧
- 了解硬件配置和调试

## 5.1 RDMA CM 编程

### 什么是RDMA CM？
- **RDMA Communication Manager**
- 基于连接的通信管理API
- 简化连接建立过程

### 优势
| Verbs | RDMA CM |
|--------|---------|
| 手动交换QP信息 | 自动处理连接建立 |
| 需要额外协议 | 内置连接管理 |
| 复杂 | 简单易用 |

### 服务器端流程
```c
struct rdma_cm_id *listen_id;
struct rdma_event_channel *ec;

ec = rdma_create_event_channel();
rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(5001),
    .sin_addr.s_addr = INADDR_ANY,
};
rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
rdma_listen(listen_id, 1);

// 等待连接
rdma_get_cm_event(ec, &event);
if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
    struct rdma_cm_id *conn_id = event->id;
    // 接受连接
    rdma_accept(conn_id, NULL);
}
```

### 客户端流程
```c
struct rdma_cm_id *conn_id;
struct rdma_event_channel *ec;

ec = rdma_create_event_channel();
rdma_create_id(ec, &conn_id, NULL, RDMA_PS_TCP);

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(5001),
    .sin_addr.s_addr = inet_addr("192.168.1.100"),
};
rdma_resolve_addr(conn_id, NULL, (struct sockaddr *)&addr, 1000);
rdma_resolve_route(conn_id, 1000);

struct rdma_conn_param conn_param = {
    .initiator_depth = 1,
    .responder_resources = 1,
};
rdma_connect(conn_id, &conn_param);
```

---

## 5.2 多线程 RDMA 编程

### 线程模型

#### 模型1：单QP多线程
- 多个线程共享一个QP
- 需要加锁保护
- 简单但有竞争

```c
pthread_mutex_t qp_lock;

void *thread_func(void *arg) {
    pthread_mutex_lock(&qp_lock);
    ibv_post_send(qp, &wr, &bad_wr);
    pthread_mutex_unlock(&qp_lock);
}
```

#### 模型2：每线程独立QP
- 每个线程拥有独立的QP
- 无锁，性能好
- 推荐使用

```c
// 主线程创建资源
for (int i = 0; i < num_threads; i++) {
    create_qp_per_thread(i);  // 每个线程独立的QP
}
```

### 注意事项

| 问题 | 解决方案 |
|------|----------|
| QP竞争 | 每线程独立QP |
| MR共享 | 预注册，按需分片 |
| CQ事件 | 每线程独立CQ或轮询 |
| 内存同步 | 使用volatile或内存屏障 |

---

## 5.3 性能优化技巧

### 1. 内存优化

#### 预注册内存池
```c
// 避免频繁注册/注销
#define PAGE_SIZE 4096
#define NUM_PAGES 1024

void *pool = aligned_alloc(PAGE_SIZE, PAGE_SIZE * NUM_PAGES);
struct ibv_mr *mr = ibv_reg_mr(pd, pool, PAGE_SIZE * NUM_PAGES, 
                                IBV_ACCESS_LOCAL_WRITE | 
                                IBV_ACCESS_REMOTE_WRITE);

// 分配子块
void *alloc_buf(size_t size) {
    // 从池中分配
}
```

#### 对齐
```c
// 确保内存对齐到缓存行或页面
posix_memalign(&buffer, 4096, size);
```

### 2. 批处理

#### 链接多个WR
```c
struct ibv_send_wr wr1 = { ... };
struct ibv_send_wr wr2 = { ... };
wr1.next = &wr2;  // 链接

ibv_post_send(qp, &wr1, &bad_wr);  // 一次提交多个
```

### 3. 异步 vs 同步

#### 同步模式
```c
ibv_post_send(qp, &wr, &bad_wr);
while (poll_cq(cq, &wc) == 0);  // 等待完成
```

#### 异步模式（推荐）
```c
// 批量提交
for (int i = 0; i < batch_size; i++) {
    ibv_post_send(qp, &wr_array[i], &bad_wr);
}

// 稍后批量轮询
while (completed < batch_size) {
    int n = poll_cq(cq, batch_size - completed, &wc_array[completed]);
    completed += n;
}
```

---

## 5.4 硬件配置

### 检查RDMA设备
```bash
# 列出RDMA设备
ibv_devices

# 查看设备信息
ibv_devinfo -d mlx5_0

# 查看端口状态
ibportstate 1 1  # 查看端口1状态
```

### 配置 Mellanox 网卡
```bash
# 查看当前配置
mlxconfig -d /dev/mst/mt4123_pciconf0 query

# 开启RoCE
mlxconfig -d /dev/mst/mt4123_pciconf0 set ROCE_V2_ENABLED=1
```

### Subnet Manager (对于IB)
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
# 查看设备信息
ibv_devinfo

# 测试RDMA连接
rdma_xsrq -d mlx5_0 -n 1000

# 带宽测试
perftest -z -d mlx5_0 -c -n 100000 -s 4096
```

### perf 工具
```bash
# 查看RDMA事件
perf record -e ib_rdma_* -a
perf report
```

### 内核日志
```bash
dmesg | grep -i rdma
dmesg | grep -i mlx5
```

---

## 5.6 常见错误处理

### WC 状态码
| 状态码 | 说明 |
|--------|------|
| IBV_WC_SUCCESS | 成功 |
| IBV_WC_LOC_LEN_ERR | 长度错误 |
| IBV_WC_LOC_QP_OP_ERR | QP操作错误 |
| IBV_WC_WR_FLUSH_ERR | 队列已刷新 |
| IBV_WC_REM_INV_REQ_ERR | 远程无效请求 |

### 调试步骤
```c
// 1. 检查返回码
if (ibv_post_send(qp, &wr, &bad_wr)) {
    perror("post_send failed");
}

// 2. 检查WC状态
if (wc.status != IBV_WC_SUCCESS) {
    printf("Error: %s\n", ibv_wc_status_str(wc.status));
}

// 3. 检查QP状态
enum ibv_qp_state state;
ibv_query_qp(qp, &attr, IBV_QP_STATE, &state);
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

## 练习题

1. RDMA CM相比ibverbs的优势是什么？
2. 多线程使用QP需要注意什么？
3. 如何避免内存频繁注册的开销？
4. WC状态码有哪些常见错误？
5. 如何进行RDMA性能测试？

---

## 下一步

继续深入学习或开始实践项目：
- 阅读NVIDIA官方文档
- 尝试修改示例代码
- 搭建实际RDMA测试环境
