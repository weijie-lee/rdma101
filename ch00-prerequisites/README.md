# 第零章：预备知识

## 学习目标

| 目标 | 说明 |
|------|------|
| 搭建开发环境 | 掌握Ubuntu下RDMA开发环境搭建 |
| 配置SoftRoCE | 使用软件模拟RDMA环境 |
| 掌握C语言基础 | 结构体、指针、轮询模式 |
| 理解Socket编程 | TCP/UDP编程基础 |

---

## 0.1 环境搭建

### 安装开发依赖

```bash
# 更新软件源
sudo apt update

# 安装RDMA开发库
sudo apt install -y libibverbs-dev librdmacm-dev

# 安装工具和测试软件
sudo apt install -y ibverbs-utils rdma-core perftest

# 安装编译工具
sudo apt install -y build-essential gcc gdb make
```

### 配置SoftRoCE（软件模拟）

```bash
# 加载内核模块
sudo modprobe rdma_rxe

# 查看网卡
ip link show

# 绑定网卡（假设eth0）
sudo rdma link add rxe0 type rxe netdev eth0

# 验证
ibv_devices
ibv_devinfo -d rxe0
```

详见：[00_environment.md](./00_environment.md)

---

## 0.2 C语言前置基础

### 结构体和指针

RDMA编程中大量使用结构体指针：

```c
struct ibv_context *ctx;
struct ibv_qp *qp;
struct ibv_mr *mr;
```

**示例代码**：[01_struct_pointer.c](./01_struct_pointer.c)

```bash
gcc -o 01_struct_pointer 01_struct_pointer.c
./01_struct_pointer
```

### 轮询模式

RDMA使用非阻塞轮询：

```c
while (1) {
    ne = ibv_poll_cq(cq, 1, &wc);
    if (ne > 0) {
        // 处理完成事件
    }
}
```

**示例代码**：[02_polling_pattern.c](./02_polling_pattern.c)

```bash
gcc -o 02_polling_pattern 02_polling_pattern.c
./02_polling_pattern
```

---

## 0.3 Socket编程基础

### TCP Server/Client模型

详见：[tcp_server.c](./tcp_server.c) 和 [tcp_client.c](./tcp_client.c)

---

## 0.4 编译运行

### 编译命令

```bash
# 不需要RDMA库的练习
gcc -o program program.c

# 需要RDMA库的程序
gcc -o program program.c -libverbs

# 使用Makefile
make
make clean
```

---

## 下一步

进入下一章：[第一章：RDMA基础概念](../ch01-intro/README.md)
