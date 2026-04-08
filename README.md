# RDMA 101 教程

**Remote Direct Memory Access (RDMA) 编程入门指南**

---

## 📚 教程概述

本教程为零基础学习者设计，通过系统化的章节安排，帮助你从概念理解到实际编程，掌握RDMA编程的核心技能。

**前置要求**：
- C语言基础
- Linux系统使用经验
- 目标：阿里云ECS + SoftRoCE环境

---

## 📖 章节目录

### 预备阶段

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch00-prerequisites](./ch00-prerequisites/README.md) | 预备知识 | 环境搭建、C语言基础、Socket编程 |

### 第一阶段：基础概念

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch01-intro](./ch01-intro/README.md) | RDMA基础概念 | 理解RDMA是什么、为什么快 |
| [ch02-verbs-api](./ch02-verbs-api/README.md) | Verbs API入门 | 掌握RDMA编程基本流程 |
| [ch03-qp-mr](./ch03-qp-mr/README.md) | QP与MR深入 | 深入理解队列对和内存区域 |

### 第二阶段：通信实践

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch04-communication](./ch04-communication/README.md) | 通信模式实践 | 四种RDMA操作详解 |
| [ch05-advanced](./ch05-advanced/README.md) | 高级主题 | RDMA CM、多线程、性能优化 |

---

## 🚀 快速开始

### 环境准备

```bash
# 1. 安装依赖
sudo apt-get install libibverbs-dev librdmacm-dev

# 2. 检查RDMA设备
ibv_devices
```

### 学习顺序

```
ch00 → ch01 → ch02 → ch03 → ch04 → ch05
```

---

## 📂 示例代码

### ch00-prerequisites

| 文件 | 说明 |
|------|------|
| `00_environment.md` | 环境搭建指南 |
| `01_struct_pointer.c` | C语言：结构体指针练习 |
| `02_polling_pattern.c` | C语言：轮询模式练习 |
| `tcp_server.c` | Socket：TCP服务器 |
| `tcp_client.c` | Socket：TCP客户端 |

### ch02-verbs-api

| 文件 | 说明 |
|------|------|
| `01-initialization/rdma_init.c` | 设备初始化 |
| `01-initialization/01_init_resources.c` | 六步初始化 |
| `02-qp-state/qp_state.c` | QP状态转换 |

### ch04-communication

| 文件 | 说明 |
|------|------|
| `01-rdma-write/rdma_write.c` | RDMA Write |
| `02-send-recv/send_recv.c` | Send/Recv (跨机器) |
| `02-send-recv/01_loopback_send_recv.c` | Send/Recv (Loopback) |
| `03-rdma-read/rdma_read.c` | RDMA Read |
| `04-atomic/atomic_ops.c` | 原子操作 |

---

## 📚 学习路径

### 前置基础（1-2周）

- [x] C语言：指针与内存管理
- [x] C语言：结构体填参 + 指针传递
- [x] 网络概念：Client/Server模型

### 阶段一：概念与架构（1周）

- [x] 为什么比TCP/IP快
- [x] 三种技术：InfiniBand/RoCE/iWARP
- [x] 核心对象：QP、CQ、MR、PD

### 阶段二：资源初始化（2周）

- [x] 六步初始化：device → pd → mr → cq → qp
- [x] 内存注册：lkey/rkey
- [x] QP状态机：RESET → INIT → RTR → RTS

### 阶段三：第一个RDMA程序（2-3周）

- [x] RC模式Send/Recv
- [x] ibv_post_send/recv/poll_cq

### 阶段四：RDMA Write/Read与CM（2周）

- [x] RDMA Write：单边操作
- [x] RDMA Read：从远端拉取
- [x] RDMA_CM API

---

## 🔧 常用API速查

```c
// 初始化
ibv_get_device_list(&n)    // 获取设备列表
ibv_open_device(dev)        // 打开设备
ibv_alloc_pd(ctx)           // 创建保护域
ibv_reg_mr(pd, buf, len, flags)  // 注册内存
ibv_create_cq(ctx, depth)  // 创建完成队列
ibv_create_qp(pd, &attr)   // 创建队列对
ibv_modify_qp(qp, &attr, flags)  // 迁移QP状态

// 数据传输
ibv_post_recv(qp, &wr, &bad_wr)  // 提交接收
ibv_post_send(qp, &wr, &bad_wr)  // 提交发送
ibv_poll_cq(cq, n, &wc)          // 轮询完成

// 释放（逆序）
ibv_destroy_qp(qp)
ibv_destroy_cq(cq)
ibv_dereg_mr(mr)
ibv_dealloc_pd(pd)
ibv_close_device(ctx)
```

---

## 📌 核心概念速查

### 对象关系

```
设备 (device)
 └── 保护域 PD
     ├── 内存区 MR (lkey/rkey)
     ├── 完成队列 CQ
     └── 队列对 QP (SQ + RQ)
```

### QP状态机

```
RESET → INIT → RTR → RTS
```

### 四种通信操作

| 操作 | 对端CPU | 说明 |
|------|---------|------|
| Send | 是 | 双边操作 |
| Recv | 是 | 双边操作 |
| RDMA Write | **否** | 单边操作 |
| RDMA Read | **否** | 单边操作 |

---

## 🔗 参考资源

- [NVIDIA RDMA Programming Manual](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- [rdma-core GitHub](https://github.com/linux-rdma/rdma-core)

---

## 📄 许可证

MIT License
