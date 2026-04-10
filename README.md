# RDMA 101 教程

**Remote Direct Memory Access (RDMA) 编程完整知识图谱**

---

## 教程概述

本教程基于 [NVIDIA RDMA Aware Programming v1.7](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17) 编写，覆盖 RDMA 编程从入门到工程实践的完整知识图谱。

**环境要求**：
- Linux Ubuntu 22.04
- 已安装 rdma-core / libibverbs（`sudo apt-get install libibverbs-dev librdmacm-dev`）
- InfiniBand 或 RoCE 真实网卡（或 SoftRoCE 软件模拟）
- `ibv_devinfo` 有正常输出

**特色**：
- 所有代码同时兼容 InfiniBand 和 RoCE 环境（自动检测传输层）
- 每行关键代码都有中文注释
- 包含完整的编译命令和预期输出说明
- 提供 Shell 脚本进行环境检查和诊断

---

## 章节目录

### 预备阶段

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch00-prerequisites](./ch00-prerequisites/README.md) | 预备知识 | 环境搭建、C语言基础、Socket编程 |

### 第一阶段：基础概念

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch01-intro](./ch01-intro/README.md) | RDMA 基础概念 | 理解RDMA是什么、为什么快 |
| [ch02-network-layer](./ch02-network-layer/README.md) | 网络技术层 | InfiniBand、RoCE、iWARP、Verbs 抽象 |

### 第二阶段：核心编程对象

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch03-verbs-api](./ch03-verbs-api/README.md) | Verbs API 入门 | 设备初始化、PD、MR、CQ、QP 完整编程 |
| [ch04-qp-mr](./ch04-qp-mr/README.md) | QP 与 MR 深入 | 深入理解队列对和内存区域 |

### 第三阶段：通信实践

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch05-communication](./ch05-communication/README.md) | 通信模式实践 | Send/Recv、RDMA Write/Read、原子操作 |
| [ch06-connection](./ch06-connection/README.md) | 连接管理 | 手动建连、RDMA CM、UD 模式 |

### 第四阶段：工程实践

| 章节 | 内容 | 说明 |
|------|------|------|
| [ch07-engineering](./ch07-engineering/README.md) | 工程实践 | perftest、调优技巧、调试工具、错误处理 |
| [ch08-advanced](./ch08-advanced/README.md) | 进阶专题 | GPUDirect RDMA、NCCL、用户态驱动原理 |
| [ch09-quickref](./ch09-quickref/README.md) | 速查手册 | API 速查、错误码速查、Hello World 模板 |

---

## 快速开始

### 无 RDMA 硬件？没问题！（SoftRoCE 一键配置）

本教程完全支持在**没有 RDMA 物理网卡**的普通 Linux 机器上学习，
通过 SoftRoCE 软件模拟实现与真实硬件一致的 API 行为。

```bash
# 三步完成环境配置和验证:

# Step 1: 一键配置 SoftRoCE
sudo ./scripts/setup_softrce.sh

# Step 2: 编译所有示例
make all

# Step 3: 运行全量测试，验证环境
./scripts/run_all_tests.sh
```

> 💡 详细的 SoftRoCE 配置说明和故障排查请参考:
> [ch00-prerequisites/00_environment.md](./ch00-prerequisites/00_environment.md)

### 有 RDMA 硬件

```bash
# 1. 安装依赖
sudo apt-get install libibverbs-dev librdmacm-dev rdma-core perftest

# 2. 检查设备
ibv_devices
ibv_devinfo

# 3. 编译和测试
make all
./scripts/run_all_tests.sh
```

### 学习顺序

```
ch00 → ch01 → ch02 → ch03 → ch04 → ch05 → ch06 → ch07 → ch08 → ch09
```

建议按顺序学习，每个阶段打好基础后再进入下一阶段。

### Makefile 命令速查

```bash
make all       # 编译所有示例程序
make setup     # 一键配置 SoftRoCE 环境 (需要 sudo)
make test      # 运行全量测试
make check-env # 检查 RDMA 环境状态
make clean     # 清理编译产物
```

---

## 示例代码一览

### ch00-prerequisites — 预备知识

| 文件 | 说明 |
|------|------|
| `01_struct_pointer.c` | C语言：结构体指针练习 |
| `02_polling_pattern.c` | C语言：轮询模式练习 |
| `tcp_server.c` / `tcp_client.c` | Socket：TCP 客户端/服务器 |

### ch02-network-layer — 网络技术层

| 文件 | 说明 |
|------|------|
| `01-infiniband/ib_env_check.sh` | IB 环境检查脚本 |
| `01-infiniband/ib_port_detail.c` | ibv_query_port() 全字段打印 |
| `02-roce/roce_env_check.sh` | RoCE GID/PFC/ECN 检查 |
| `02-roce/roce_gid_query.c` | ibv_query_gid() 遍历 + IB/RoCE 检测 |
| `03-iwarp/iwarp_query.c` | iWARP 传输类型查询 |
| `04-verbs-abstraction/verbs_any_transport.c` | Verbs 统一抽象层演示 |

### ch03-verbs-api — Verbs API 入门

| 文件 | 说明 |
|------|------|
| `01-initialization/01_init_resources.c` | 六步初始化 + 设备能力查询 |
| `01-initialization/02_multi_device.c` | 多设备多端口枚举 |
| `02-qp-state/qp_state.c` | QP 状态转换（IB/RoCE 双模） |
| `02-qp-state/01_qp_error_recovery.c` | QP 错误恢复演示 |
| `03-pd/pd_isolation.c` | PD 保护域隔离演示 |
| `04-mr/mr_access_flags.c` | MR 访问权限测试 |
| `04-mr/mr_multi_reg.c` | 同一内存多次注册 |
| `05-cq/cq_event_driven.c` | 事件驱动 CQ 演示 |
| `05-cq/cq_overflow.c` | CQ 溢出演示 |

### ch05-communication — 通信模式实践

| 文件 | 说明 |
|------|------|
| `01-rdma-write/rdma_write.c` | RDMA Write (C/S 模式) |
| `01-rdma-write/01_write_imm.c` | RDMA Write with Immediate |
| `02-send-recv/01_loopback_send_recv.c` | Send/Recv Loopback（多SGE） |
| `02-send-recv/02_sge_demo.c` | Scatter-Gather 演示 |
| `02-send-recv/03_rnr_error_demo.c` | RNR 错误演示 |
| `03-rdma-read/rdma_read.c` | RDMA Read (C/S 模式) |
| `03-rdma-read/01_batch_read.c` | 批量 RDMA Read |
| `04-atomic/atomic_ops.c` | FAA + CAS 原子操作 |
| `04-atomic/01_alignment_error.c` | 原子操作对齐错误 |
| `04-atomic/02_spinlock.c` | CAS 分布式自旋锁 |

### ch06-connection — 连接管理

| 文件 | 说明 |
|------|------|
| `01-manual-connect/manual_connect.c` | 手动建连（双进程 -s/-c） |
| `02-rdma-cm/rdma_cm_example.c` | RDMA CM API 完整示例 |
| `03-ud-mode/ud_loopback.c` | UD 模式 Loopback + Address Handle |

### ch07-engineering — 工程实践

| 文件 | 说明 |
|------|------|
| `01-perftest/run_perftest.sh` | 自动化性能基准测试 |
| `02-tuning/inline_data.c` | Inline Data 优化 |
| `02-tuning/unsignaled_send.c` | Unsignaled Send 优化 |
| `02-tuning/srq_demo.c` | Shared Receive Queue 演示 |
| `03-debug/rdma_diag.sh` | RDMA 环境诊断脚本 |
| `04-error-handling/trigger_errors.c` | 故意触发各种 WC 错误 |
| `04-error-handling/error_diagnosis.c` | 错误码诊断工具 |

### ch08-advanced — 进阶专题

| 文件 | 说明 |
|------|------|
| `01-gpudirect/gpudirect_check.sh` | GPUDirect 环境检查 |
| `01-gpudirect/gpudirect_framework.c` | GPUDirect RDMA 代码框架 |
| `02-nccl/nccl_env_check.sh` | NCCL 环境检查 |
| `03-userspace-driver/userspace_driver_trace.sh` | 用户态驱动 strace 追踪 |

### ch09-quickref — 速查手册

| 文件 | 说明 |
|------|------|
| `hello_rdma.c` | RDMA Hello World（最小可运行模板） |
| `env_check.sh` | 一键环境检查脚本 |
| `error_cheatsheet.c` | 常见错误速查工具 |

---

## 公共工具库

`common/rdma_utils.h` 提供所有程序共用的 IB/RoCE 双模支持：

```c
#include "rdma_utils.h"

// 自动检测传输层类型
enum rdma_transport t = detect_transport(ctx, port);

// 一键 QP 状态转换（自动处理 IB/RoCE 差异）
qp_full_connect(qp, &remote_ep, port, is_roce, access_flags);

// TCP 带外信息交换
exchange_endpoint_tcp(server_ip, tcp_port, &local_ep, &remote_ep);

// 打印完整 WC 详情
print_wc_detail(&wc);
```

---

## 核心 API 速查

```c
// 初始化
ibv_get_device_list(&n)               // 获取设备列表
ibv_open_device(dev)                   // 打开设备
ibv_query_device(ctx, &attr)           // 查询设备能力
ibv_alloc_pd(ctx)                      // 创建保护域
ibv_reg_mr(pd, buf, len, flags)        // 注册内存
ibv_create_cq(ctx, depth, ...)         // 创建完成队列
ibv_create_qp(pd, &attr)              // 创建队列对
ibv_modify_qp(qp, &attr, flags)       // 迁移QP状态

// 数据传输
ibv_post_recv(qp, &wr, &bad_wr)       // 提交接收
ibv_post_send(qp, &wr, &bad_wr)       // 提交发送
ibv_poll_cq(cq, n, &wc)               // 轮询完成

// 释放（逆序）
ibv_destroy_qp(qp)
ibv_destroy_cq(cq)
ibv_dereg_mr(mr)
ibv_dealloc_pd(pd)
ibv_close_device(ctx)
```

---

## QP 状态机

```
RESET → INIT → RTR → RTS
  ↑                    ↓
  └────── ERROR ◄──────┘
```

---

## 学习路径

| 阶段 | 周数 | 章节 | 目标 |
|------|------|------|------|
| 预备 | 1-2 | ch00 | C 基础、Socket 编程、环境搭建 |
| 概念 | 1 | ch01-ch02 | 理解 RDMA 原理和网络技术 |
| 编程 | 2 | ch03-ch04 | 掌握 Verbs API 和核心对象 |
| 通信 | 2-3 | ch05-ch06 | 实现四种通信模式和连接管理 |
| 工程 | 2 | ch07-ch09 | 性能调优、调试、错误处理 |

---

## 参考资源

- [NVIDIA RDMA Programming Manual v1.7](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- [rdma-core GitHub](https://github.com/linux-rdma/rdma-core)
- [Linux Kernel RDMA Documentation](https://www.kernel.org/doc/html/latest/infiniband/)

---

## 许可证

MIT License
