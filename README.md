# RDMA 101 教程

**Remote Direct Memory Access (RDMA) 编程入门指南**

---

## 📚 教程概述

本教程为零基础学习者设计，通过系统化的章节安排，帮助你从概念理解到实际编程，掌握RDMA编程的核心技能。

**前置要求**：
- C语言基础
- Linux系统使用经验

**学习路径**：

```
前置基础 → 环境搭建 → 资源初始化 → Send/Recv → Write/Read → 高级主题
```

---

## 📖 目录结构

### 快速开始

| 目录 | 说明 |
|------|------|
| [docs/](./docs/) | 文档目录 |
| [src/](./src/) | 示例代码目录 |

### 学习路径

```
docs/
└── 00_environment.md          # 环境搭建指南

src/
├── 01_basic_concepts/         # C语言前置基础
│   ├── 01_struct_pointer.c   # 结构体和指针练习
│   └── 02_polling_pattern.c  # 轮询模式练习
│
├── 02_resources/              # RDMA资源初始化
│   └── 01_init_resources.c   # 六步初始化
│
└── 03_send_recv/              # 第一个RDMA程序
    └── 01_loopback_send_recv.c # Send/Recv Loopback
```

---

## 🚀 快速开始

### 1. 环境准备

```bash
# 安装依赖
sudo apt update
sudo apt install -y libibverbs-dev librdmacm-dev build-essential
```

详见：[环境搭建文档](./docs/00_environment.md)

### 2. 编译运行示例

```bash
# 前置练习（不需要RDMA硬件）
cd src/01_basic_concepts
make
./01_struct_pointer
./02_polling_pattern

# 资源初始化（需要RDMA环境）
cd ../02_resources
make
./01_init_resources

# Send/Recv程序
cd ../03_send_recv
make
./01_loopback_send_recv
```

---

## 📚 学习路径

### 第一阶段：前置基础（1-2周）

- [x] C语言：指针与内存管理
- [x] 结构体填参 + 指针传递
- [x] 网络概念：Client/Server模型

### 第二阶段：环境搭建

- [x] Ubuntu开发环境
- [x] SoftRoCE配置（软件模拟）
- [x] 验证ibv_devinfo

### 第三阶段：RDMA概念与架构

- [x] 为什么比TCP/IP快
- [x] 三种技术：InfiniBand/RoCE/iWARP
- [x] 核心对象：QP、CQ、MR、PD

### 第四阶段：IBV Verbs资源初始化

- [x] 六步初始化：device → pd → mr → cq → qp
- [x] 内存注册：lkey/rkey
- [x] QP状态机：RESET → INIT → RTR → RTS

### 第五阶段：第一个RDMA程序

- [x] RC模式Send/Recv Loopback
- [x] ibv_post_send/recv/poll_cq

### 第六阶段：RDMA Write/Read与CM

- [ ] RDMA Write：单边操作
- [ ] RDMA Read：从远端拉取
- [ ] RDMA_CM API

---

## 🔧 常用API速查

```c
// 初始化
ibv_get_device_list(&n)    // 获取设备列表
ibv_open_device(dev)        // 打开设备
ibv_alloc_pd(ctx)           // 创建保护域
ibv_reg_mr(pd, buf, len, flags)  // 注册内存
ibv_create_cq(ctx, depth) // 创建完成队列
ibv_create_qp(pd, &attr)  // 创建队列对
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
 ├── 内存区 MR
 │ ├── lkey：本地访问
 │ └── rkey：远端访问
 ├── 完成队列 CQ
 └── 队列对 QP
     ├── 发送队列 SQ
     └── 接收队列 RQ
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
