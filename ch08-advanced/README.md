# 第八章：高级专题

**GPUDirect RDMA、NCCL 与用户态驱动原理**

---

## 章节概述

本章介绍三个 RDMA 高级专题，帮助你理解 RDMA 在 AI/HPC 场景中的实际应用，
以及底层用户态驱动的工作原理。这些知识不是编写基础 RDMA 程序所必须的，
但对于理解现代 AI 集群通信和性能调优非常重要。

---

## 目录

| 节 | 内容 | 说明 |
|----|------|------|
| [01-gpudirect](./01-gpudirect/) | GPUDirect RDMA | GPU 显存直接走 RDMA，跳过 CPU 内存拷贝 |
| [02-nccl](./02-nccl/) | NCCL 与 RDMA | NVIDIA 集合通信库如何利用 RDMA |
| [03-userspace-driver](./03-userspace-driver/) | 用户态驱动原理 | ibv_post_send 为何不需要系统调用 |

---

## 01 - GPUDirect RDMA

### 什么是 GPUDirect RDMA？

传统的 GPU 网络通信路径：

```
GPU显存 → cudaMemcpy → CPU内存 → RDMA Send → 网络 → RDMA Recv → CPU内存 → cudaMemcpy → GPU显存
```

GPUDirect RDMA 的路径：

```
GPU显存 → RDMA Send → 网络 → RDMA Recv → GPU显存
```

**关键优势**：
- 跳过了两次 CPU 内存拷贝 (cudaMemcpy)
- 降低延迟，提高带宽利用率
- GPU 和 NIC 之间通过 PCIe 直接传输数据

### 工作原理

```
┌──────────────┐     PCIe     ┌──────────────┐
│   GPU (CUDA) │ ◄──────────► │  RDMA NIC    │
│   显存       │   直接DMA    │  (HCA)       │
└──────────────┘              └──────────────┘
       │                            │
       └────── 跳过 CPU 内存 ───────┘
```

1. **nvidia_peermem 模块**：让 RDMA NIC 能够直接访问 GPU 显存
2. **CUDA API**：`cuMemAlloc()` 分配 GPU 显存
3. **Verbs API**：`ibv_reg_mr()` 注册 GPU 显存为 MR
4. **数据传输**：NIC 通过 PCIe 直接从 GPU 显存读写数据

### 前置条件

| 条件 | 说明 |
|------|------|
| NVIDIA GPU | 支持 CUDA 的 GPU (Kepler 或更新) |
| nvidia_peermem | 内核模块，CUDA 11.4+ 自带 |
| Mellanox NIC | ConnectX-4 或更新的 RDMA 网卡 |
| PCIe 拓扑 | GPU 和 NIC 最好在同一 PCIe switch 下 |

### 本节文件

| 文件 | 说明 |
|------|------|
| `gpudirect_check.sh` | 一键检查 GPUDirect 环境是否就绪 |
| `gpudirect_framework.c` | GPUDirect RDMA 代码框架 (支持有/无 CUDA 编译) |

---

## 02 - NCCL (NVIDIA Collective Communications Library)

### 什么是 NCCL？

NCCL 是 NVIDIA 的集合通信库，专为多 GPU / 多节点 AI 训练设计。
它是 PyTorch `torch.distributed`、TensorFlow、DeepSpeed 等框架的底层通信后端。

### NCCL 与 RDMA 的关系

```
┌───────────────────────────────────────┐
│         PyTorch / TensorFlow          │  ← 用户代码
├───────────────────────────────────────┤
│         torch.distributed / Horovod   │  ← 分布式框架
├───────────────────────────────────────┤
│                NCCL                   │  ← 集合通信库
├──────────┬────────────┬───────────────┤
│  IB Verbs│  Socket    │   NVLink      │  ← 传输层
│  (RDMA)  │  (TCP/IP)  │  (节点内)     │
└──────────┴────────────┴───────────────┘
```

当 NCCL 检测到 RDMA 网卡时，自动使用 IB Verbs 进行节点间通信：
- 支持 GPUDirect RDMA（GPU 显存直接走网络）
- 支持 RDMA Write（零拷贝传输）
- 自动选择最优的 NIC-GPU 亲和性

### 关键环境变量

| 变量 | 作用 | 常用值 |
|------|------|--------|
| `NCCL_IB_DISABLE` | 禁用 IB | 0 (启用) / 1 (禁用) |
| `NCCL_IB_HCA` | 指定使用的 HCA | `mlx5_0` |
| `NCCL_IB_GID_INDEX` | RoCE GID 索引 | `3` (RoCE v2) |
| `NCCL_NET_GDR_LEVEL` | GPUDirect 级别 | `5` (跨节点) |
| `NCCL_DEBUG` | 调试级别 | `INFO` / `WARN` / `TRACE` |
| `NCCL_DEBUG_SUBSYS` | 调试子系统 | `NET` / `INIT` / `ALL` |
| `NCCL_SOCKET_IFNAME` | 控制面网口 | `eth0` |

### 本节文件

| 文件 | 说明 |
|------|------|
| `nccl_env_check.sh` | 检查 NCCL 安装和环境变量配置 |

---

## 03 - 用户态驱动原理

### 为什么 ibv_post_send 不需要系统调用？

这是 RDMA 高性能的核心秘密之一。传统网络编程中，每次 `send()` 都需要
一次系统调用（上下文切换 ~1μs）。而 RDMA 的 `ibv_post_send()` 是纯用户态操作。

### 工作原理

```
┌─────────────────────────────────────────────────┐
│                   用户态                          │
│                                                   │
│  应用程序                                         │
│    │                                              │
│    ▼                                              │
│  libibverbs (libmlx5.so / libhfi1.so)            │
│    │                                              │
│    ▼                                              │
│  写 WQE 到 Send Queue (mmap 的共享内存)          │
│    │                                              │
│    ▼                                              │
│  Doorbell Write (写 HCA 寄存器, 也是 mmap 的)    │
│    │                                              │
├────┼──────────────────────────────────────────────┤
│    │              内核态                           │
│    │  (ibv_post_send 不经过这里!)                 │
├────┼──────────────────────────────────────────────┤
│    ▼              硬件                             │
│  HCA 读取 WQE → DMA 读取数据 → 发送到网络        │
└─────────────────────────────────────────────────┘
```

### 关键步骤

1. **初始化阶段** (需要内核参与，通过 ioctl)：
   - `ibv_open_device()` → `open("/dev/infiniband/uverbs0")`
   - `ibv_create_qp()` → `ioctl()` 让内核分配 QP 资源
   - 内核通过 `mmap()` 将 QP 的 Send/Recv Queue 映射到用户态

2. **数据通路** (纯用户态，无系统调用)：
   - `ibv_post_send()` → 直接写 WQE 到 mmap 的 SQ 内存
   - 写 Doorbell 寄存器通知 HCA → 也是 mmap 的 MMIO 地址
   - HCA 通过 DMA 读取数据并发送

3. **完成通知**：
   - `ibv_poll_cq()` → 直接读 mmap 的 CQ 内存，也是纯用户态

### 哪些操作需要内核，哪些不需要？

| 操作 | 是否需要内核 | 原因 |
|------|-------------|------|
| `ibv_open_device` | 需要 | 打开 `/dev/infiniband/uverbs*` |
| `ibv_alloc_pd` | 需要 | ioctl 分配内核资源 |
| `ibv_reg_mr` | 需要 | ioctl 锁定物理页面 |
| `ibv_create_cq` | 需要 | ioctl 分配 + mmap 映射 |
| `ibv_create_qp` | 需要 | ioctl 分配 + mmap 映射 |
| `ibv_modify_qp` | 需要 | ioctl 修改 QP 状态 |
| `ibv_post_send` | **不需要** | 直接写 mmap 内存 + doorbell |
| `ibv_post_recv` | **不需要** | 直接写 mmap 内存 + doorbell |
| `ibv_poll_cq` | **不需要** | 直接读 mmap 内存 |

### 本节文件

| 文件 | 说明 |
|------|------|
| `userspace_driver_trace.sh` | 用 strace 追踪 RDMA 程序的系统调用 |

---

## 学习建议

1. **先理解原理**：本章重在概念理解，不需要你立即动手写 GPUDirect 代码
2. **运行检查脚本**：即使没有 GPU，也可以运行检查脚本了解环境要求
3. **strace 实验**：`userspace_driver_trace.sh` 是最值得动手尝试的——它能让你亲眼看到 RDMA 用户态驱动的工作方式
4. **结合前面的章节**：回顾 ch05 的 Send/Recv 程序，思考 `ibv_post_send()` 背后发生了什么

---

## 延伸阅读

- [NVIDIA GPUDirect RDMA 官方文档](https://docs.nvidia.com/cuda/gpudirect-rdma/)
- [NCCL 官方文档](https://docs.nvidia.com/deeplearning/nccl/)
- [Mellanox RDMA Aware Programming 用户手册](https://docs.nvidia.com/networking/display/rdmacore/)
- [libibverbs 源码 (rdma-core)](https://github.com/linux-rdma/rdma-core)

---

*下一章: [ch09-quickref](../ch09-quickref/) - API 速查手册与工具集*
