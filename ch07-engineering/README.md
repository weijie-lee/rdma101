# 第七章：RDMA 工程实践

**性能测试、调优、调试与错误处理**

---

## 概述

前面的章节我们学习了 RDMA 的核心概念和通信模式。本章聚焦于**工程实践**——
在真实项目中，你需要知道如何测量性能、优化吞吐/延迟、排查问题、以及优雅地处理错误。

本章包含四个模块：

| 模块 | 目录 | 内容 |
|------|------|------|
| 性能测试 | `01-perftest/` | 使用 perftest 工具套件测量 RDMA 性能 |
| 性能调优 | `02-tuning/` | Inline Data、Unsignaled Send、SRQ 三大优化 |
| 调试诊断 | `03-debug/` | 一键环境诊断、strace 分析 Verbs 调用 |
| 错误处理 | `04-error-handling/` | 故意触发错误 + 错误码诊断手册 |

---

## 01-perftest: 性能测试

### 为什么需要 perftest？

perftest 是 RDMA 社区的标准性能测试工具，可以测量：

- **延迟 (Latency)**：发送一个消息到收到响应的时间 (μs)
- **带宽 (Bandwidth)**：单位时间内传输的数据量 (Gb/s)
- **消息速率 (Message Rate)**：每秒处理的消息数 (Mpps)

### 文件说明

| 文件 | 说明 |
|------|------|
| `run_perftest.sh` | 自动化 perftest 基准测试脚本 |

### 快速使用

```bash
# 运行全部基准测试 (loopback 模式)
./run_perftest.sh

# 指定设备
./run_perftest.sh rxe0
```

### 关键指标解读

```
BW (Gb/s)   = 实际带宽 (千兆位/秒)
BW (MB/s)   = 实际带宽 (兆字节/秒), 1 Gb/s ≈ 125 MB/s
Latency(μs) = 半程延迟 (微秒)
MsgRate     = 每秒消息数 (百万条)
```

---

## 02-tuning: 性能调优

### 三大核心优化技术

#### 1. Inline Data (内联数据)

**原理**: 将小消息直接嵌入 WQE (Work Queue Element)，跳过 DMA 读取步骤。

```
普通路径: CPU → WQE(指针) → NIC DMA 读内存 → NIC 发送
Inline:   CPU → WQE(含数据) → NIC 直接发送 (省一次 DMA)
```

**适用场景**: 消息 < 256 字节 (具体取决于硬件 max_inline_data)

**注意**: inline 发送不需要 lkey，因为数据直接在 WQE 中。

#### 2. Unsignaled Completion (非信号完成)

**原理**: 并非每个 Send 操作都产生 CQE，减少 CQ 轮询开销。

```
Signaled:   每个 Send → 产生 CQE → 必须 poll_cq
Unsignaled: 每 N 个 Send → 只有第 N 个产生 CQE → poll 一次
```

**关键约束**:
- 必须在 SQ 满之前至少 signal 一次 (否则 SQ 会溢出)
- 必须在 CQ 满之前 poll (否则 CQ 会溢出)
- 推荐: 每 min(max_send_wr/2, 32) 个请求 signal 一次

#### 3. Shared Receive Queue (SRQ)

**原理**: 多个 QP 共享一个接收队列，减少 recv buffer 总量。

```
没有 SRQ: 每个 QP 需要独立的 recv buffer (N 个 QP × M 个 buffer)
使用 SRQ: 所有 QP 共享一个 recv pool (1 个 pool × M 个 buffer)
```

**适用场景**: 大量 QP 连接 (如数据库的数千个客户端连接)

### 文件说明

| 文件 | 说明 |
|------|------|
| `inline_data.c` | Inline Data 优化对比实验 |
| `unsignaled_send.c` | Unsignaled Completion 优化对比实验 |
| `srq_demo.c` | Shared Receive Queue 演示 |
| `Makefile` | 编译以上三个程序 |

### 编译运行

```bash
cd 02-tuning
make
./inline_data
./unsignaled_send
./srq_demo
```

---

## 03-debug: 调试诊断

### RDMA 调试的难点

1. **内核旁路**: 大部分操作不经过内核，strace/ltrace 看不到
2. **异步错误**: 错误可能在 WC 或异步事件中报告
3. **硬件依赖**: 问题可能出在驱动、固件、网线、交换机
4. **状态机复杂**: QP 状态错误不会立即报错，而是在后续操作中失败

### 诊断工具链

```
环境检查:  rdma_diag.sh      → 一键检查所有 RDMA 配置
系统调用:  strace_verbs.sh   → 追踪 Verbs 的系统调用
设备信息:  ibv_devinfo        → 查看设备详情
端口状态:  ibv_devinfo -v     → 查看端口状态
网络抓包:  tcpdump / Wireshark → 抓取 RoCE 数据包
内核日志:  dmesg              → 查看驱动层错误
```

### 文件说明

| 文件 | 说明 |
|------|------|
| `rdma_diag.sh` | 一键 RDMA 环境诊断 (模块/设备/端口/GID/配额) |
| `strace_verbs.sh` | 追踪 RDMA 程序的系统调用并注释说明 |

### 快速使用

```bash
# 一键诊断
./rdma_diag.sh

# 追踪 Verbs 调用
./strace_verbs.sh ./your_rdma_program
```

---

## 04-error-handling: 错误处理

### RDMA 错误类型

| 类型 | 来源 | 说明 |
|------|------|------|
| WC 错误 | `ibv_poll_cq` | 操作完成但失败 (最常见) |
| 异步事件 | `ibv_get_async_event` | 设备/端口/QP 级别异步错误 |
| Verbs 返回值 | API 调用返回 | 参数错误、资源不足 |

### 常见 WC 错误码

| 错误码 | 含义 | 常见原因 |
|--------|------|----------|
| `IBV_WC_LOC_LEN_ERR` | 本地长度错误 | recv buffer 太小 |
| `IBV_WC_LOC_PROT_ERR` | 本地保护错误 | lkey 错误 / PD 不匹配 |
| `IBV_WC_REM_ACCESS_ERR` | 远端访问错误 | rkey 错误 / 权限不足 |
| `IBV_WC_RNR_RETRY_EXC_ERR` | RNR 重试超限 | 对端没有 post recv |
| `IBV_WC_WR_FLUSH_ERR` | WR 刷出错误 | QP 已进入 ERROR 状态 |
| `IBV_WC_RETRY_EXC_ERR` | 重试超限 | 对端不可达 / QP 状态错 |

### QP 错误恢复

```
正常: RESET → INIT → RTR → RTS
出错: RTS → ERROR (自动转换)
恢复: ERROR → RESET → INIT → RTR → RTS (重走一遍)
```

### 文件说明

| 文件 | 说明 |
|------|------|
| `trigger_errors.c` | 故意触发 4 种典型 RDMA 错误并展示恢复 |
| `error_diagnosis.c` | RDMA 错误码诊断手册 (交互式查询) |
| `Makefile` | 编译以上两个程序 |

### 编译运行

```bash
cd 04-error-handling
make

# 触发并观察各种错误
./trigger_errors

# 查看所有错误码说明
./error_diagnosis

# 查看特定错误码
./error_diagnosis 5   # IBV_WC_REM_ACCESS_ERR
```

---

## 学习路线建议

```
1. 先跑 perftest 建立性能基准        → 01-perftest/
2. 用调优技术逐个提升性能            → 02-tuning/
3. 遇到问题时用诊断工具排查          → 03-debug/
4. 理解错误码，写出健壮的错误处理    → 04-error-handling/
```

---

## 编译依赖

```bash
# 基础编译工具
sudo apt-get install gcc make

# RDMA 开发库
sudo apt-get install libibverbs-dev librdmacm-dev

# perftest 工具
sudo apt-get install perftest

# strace (调试用)
sudo apt-get install strace
```

---

## 参考资料

- [perftest GitHub](https://github.com/linux-rdma/perftest)
- [RDMA Aware Programming User Manual](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- [rdma-core Kernel Documentation](https://www.kernel.org/doc/Documentation/infiniband/)
- [SoftRoCE (RXE) Wiki](https://github.com/linux-rdma/rdma-core/blob/master/Documentation/rxe.md)
