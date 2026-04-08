# RDMA 101 教程

**Remote Direct Memory Access (RDMA) 编程入门指南**

---

## 📚 教程概述

本教程为零基础学习者设计，通过系统化的章节安排，帮助你从概念理解到实际编程，掌握RDMA编程的核心技能。

**前置要求**：
- C语言基础
- Linux系统使用经验
- 计算机网络基础知识

---

## 📖 章节目录

### 预备阶段

| 章节 | 内容 | 描述 |
|------|------|------|
| [ch00-prerequisites](./ch00-prerequisites/README.md) | 预备知识 | Socket编程、TCP/UDP、网络字节序、RDMA连接交换 |

### 第一阶段：基础概念

| 章节 | 内容 | 描述 |
|------|------|------|
| [ch01-intro](./ch01-intro/README.md) | RDMA基础概念 | 理解RDMA是什么、为什么快、三种协议 |
| [ch02-verbs-api](./ch02-verbs-api/README.md) | Verbs API入门 | 掌握RDMA编程基本流程、设备初始化、资源创建 |
| [ch03-qp-mr](./ch03-qp-mr/README.md) | QP与MR深入 | 深入理解队列对和内存区域 |

### 第二阶段：通信实践

| 章节 | 内容 | 描述 |
|------|------|------|
| [ch04-communication](./ch04-communication/README.md) | 通信模式实践 | 四种RDMA操作详解、完整示例 |
| [ch05-advanced](./ch05-advanced/README.md) | 高级主题 | RDMA CM、多线程、性能优化、调试 |

---

## 🚀 快速开始

### 环境准备

```bash
# 1. 安装依赖
sudo apt-get install libibverbs-dev librdmacm-dev

# 2. 检查RDMA设备
ibv_devices

# 3. 编译示例
cd ch00-prerequisites
make
```

### 学习顺序

```
ch00-prerequisites → ch01-intro → ch02-verbs-api → ch03-qp-mr → ch04-communication → ch05-advanced
```

---

## 📂 示例代码

### 预备知识

| 示例 | 文件 | 说明 |
|------|------|------|
| TCP Server | `ch00-prerequisites/tcp_server.c` | 简单TCP服务器 |
| TCP Client | `ch00-prerequisites/tcp_client.c` | 简单TCP客户端 |

### 基础示例

| 示例 | 文件 | 说明 |
|------|------|------|
| 设备初始化 | `ch02-verbs-api/01-initialization/rdma_init.c` | 发现设备、创建QP/MR |
| QP状态转换 | `ch02-verbs-api/02-qp-state/qp_state.c` | RESET→INIT→RTR→RTS |

### 通信示例

| 示例 | 文件 | 说明 |
|------|------|------|
| RDMA Write | `ch04-communication/01-rdma-write/rdma_write.c` | 推送数据到远程 |
| Send/Recv | `ch04-communication/02-send-recv/send_recv.c` | 消息传递模式 |
| RDMA Read | `ch04-communication/03-rdma-read/rdma_read.c` | 拉取远程数据 |
| 原子操作 | `ch04-communication/04-atomic/atomic_ops.c` | FAA/CAS计数器 |

---

## 📝 每章包含

- **学习目标** - 明确要掌握的技能
- **概念讲解** - 详细的原理说明
- **代码示例** - 可运行的C代码
- **图解说明** - 直观理解操作流程
- **运行验证** - 测试步骤和预期输出
- **常见错误** - 问题排查指南
- **练习题** - 巩固知识点

---

## 🔧 编程模型

```
┌─────────────────────────────────────────────┐
│              Application                     │
└─────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────┐
│           libibverbs (Verbs API)            │
└─────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────┐
│         RDMA Hardware (NIC)                  │
└─────────────────────────────────────────────┘
```

---

## 📌 核心概念

| 概念 | 英文 | 说明 |
|------|------|------|
| 队列对 | QP | 通信基本单元 = SQ + RQ |
| 内存区域 | MR | 注册的内存，含lkey/rkey |
| 完成队列 | CQ | 操作完成通知 |
| 工作请求 | WR | 提交的操作 |
| 工作完成 | WC | 操作结果状态 |

---

## 🔗 相关资源

- [NVIDIA RDMA Programming Manual](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- [RDMAmojo](https://www.rdmamojo.com/)
- [Linux RDMA Documentation](https://www.kernel.org/doc/Documentation/infiniband/)

---

## 🤝 贡献

欢迎提交Issue和Pull Request！

---

## 📄 许可证

MIT License
