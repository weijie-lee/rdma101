# 第一章：C语言前置基础

## 学习目标

| 目标 | 说明 |
|------|------|
| 掌握结构体和指针 | RDMA编程的核心技能 |
| 理解内存分配释放 | malloc/free的正确使用 |
| 理解轮询模式 | RDMA中的非阻塞轮询机制 |

---

## 1.1 结构体和指针

### 为什么要学这个？

RDMA编程中，**几乎所有API都使用结构体指针**，例如：

```c
struct ibv_context *ctx;
struct ibv_qp *qp;
struct ibv_mr *mr;
```

### 示例代码

参考：[01_struct_pointer.c](./01_struct_pointer.c)

```bash
# 编译
gcc -o 01_struct_pointer 01_struct_pointer.c

# 运行
./01_struct_pointer
```

### 预期输出

```
=== 结构体指针练习 ===

Device: mlx5_0
Port: 1
FD: 123

Using arrow operator:
Device: mlx5_0
Modified port: 2

=== 练习完成 ===
```

---

## 1.2 轮询模式

### 为什么要学这个？

RDMA使用**非阻塞轮询**而非阻塞等待：

```c
// 典型RDMA轮询
while (1) {
    ne = ibv_poll_cq(cq, 1, &wc);
    if (ne > 0) {
        // 处理完成事件
    }
}
```

### 示例代码

参考：[02_polling_pattern.c](./02_polling_pattern.c)

```bash
# 编译
gcc -o 02_polling_pattern 02_polling_pattern.c

# 运行
./02_polling_pattern
```

### 预期输出

```
=== 轮询模式练习 ===

模拟添加3个完成事件...

--- 非阻塞轮询 ---
完成事件: opcode=1, bytes=1024, status=SUCCESS
完成事件: opcode=2, bytes=2048, status=SUCCESS
完成事件: opcode=3, bytes=512, status=SUCCESS

--- 阻塞轮询 ---
收到完成事件: opcode=1, bytes=4096

=== 练习完成 ===

注意：RDMA中的ibv_poll_cq()类似这个poll_cq_nonblocking()
     它是非阻塞的，需要在循环中不断调用
```

---

## 1.3 常见错误

| 错误 | 原因 | 解决方法 |
|------|------|----------|
| 段错误 | 指针未初始化 | 使用malloc后 memset为0 |
| 内存泄漏 | 忘记free | 确保配对使用malloc/free |
| 数据随机 | 结构体未清零 | 使用memset |

---

## 下一步

进入下一章：[环境搭建](../docs/00_environment.md) 或 [RDMA资源初始化](../02_resources/01_init_resources.c)
