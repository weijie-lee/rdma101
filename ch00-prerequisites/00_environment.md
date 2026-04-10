# 环境搭建指南 — SoftRoCE 完全手册

## 学习目标

| 目标 | 说明 |
|------|------|
| 理解 SoftRoCE | 什么是 SoftRoCE，为什么它能代替真实 RDMA 硬件进行学习 |
| 一键配置环境 | 使用脚本快速搭建 SoftRoCE 开发环境 |
| 理解关键差异 | SoftRoCE vs 真实硬件的区别（LID=0、GID、MTU） |
| 掌握故障排查 | 解决 SoftRoCE 环境下的常见问题 |

---

## 1. SoftRoCE 概述

### 什么是 SoftRoCE？

**SoftRoCE (Soft RDMA over Converged Ethernet)** 是 Linux 内核中的软件 RDMA 实现，
通过 `rdma_rxe` 内核模块，在**普通以太网网卡**上模拟完整的 RDMA 功能。

```
┌─────────────────────────────────────────────────────┐
│              真实 RDMA 硬件 (Mellanox ConnectX)       │
│  ┌──────────┐    ┌───────────┐    ┌──────────────┐  │
│  │ Verbs API │ → │ 驱动(mlx5) │ → │ 硬件网卡 (HCA)│  │
│  └──────────┘    └───────────┘    └──────────────┘  │
│                                                     │
│  延迟: ~1-2 μs    带宽: 100-400 Gbps               │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│              SoftRoCE (软件模拟)                      │
│  ┌──────────┐    ┌───────────┐    ┌──────────────┐  │
│  │ Verbs API │ → │ rdma_rxe  │ → │ 普通以太网网卡 │  │
│  └──────────┘    └───────────┘    └──────────────┘  │
│                                                     │
│  延迟: ~50-100 μs    带宽: 取决于网卡 (1-25 Gbps)   │
└─────────────────────────────────────────────────────┘
```

### 为什么用 SoftRoCE 学习？

| 优势 | 说明 |
|------|------|
| **零成本** | 无需购买 RDMA 网卡和交换机 |
| **API 完全一致** | 同一套 Verbs API 代码，行为完全一致 |
| **单机学习** | 一台笔记本/虚拟机即可完成所有练习 |
| **真实验证** | 所有代码在真实硬件上无需修改即可运行 |

### SoftRoCE vs 真实硬件的区别

| 项目 | SoftRoCE | 真实 RDMA 硬件 |
|------|----------|---------------|
| 延迟 | ~50-100 μs (走内核协议栈) | ~1-2 μs (硬件直通) |
| 带宽 | 受限于普通网卡 | 100-400+ Gbps |
| CPU 占用 | 高 (软件处理) | 低 (硬件卸载) |
| **LID** | **始终为 0** | IB 模式由 SM 分配 |
| **GID** | 从网卡 IP 地址生成 | 从 HCA GUID 生成 |
| 链路层 | Ethernet | IB 或 Ethernet |
| Kernel Bypass | 否 (仍经过内核) | 是 (真正旁路内核) |
| **API 行为** | **完全一致** | **完全一致** |

> **关键**: 虽然性能不同，但 **API 编程接口和行为完全一致**。
> 在 SoftRoCE 上写的代码，直接放到真实硬件上就能运行。

---

## 2. 一键配置

### 自动配置（推荐）

```bash
# 一键配置 SoftRoCE 环境
sudo ./scripts/setup_softrce.sh
```

脚本会自动完成：安装依赖 → 加载模块 → 创建设备 → 设置 ulimit → 验证环境 → 编译项目

### 手动配置

如果自动脚本无法运行，按以下步骤手动操作：

#### 步骤 1: 安装依赖

```bash
sudo apt update
sudo apt install -y libibverbs-dev librdmacm-dev rdma-core \
                    ibverbs-utils perftest build-essential
```

#### 步骤 2: 加载内核模块

```bash
# 加载 SoftRoCE 模块
sudo modprobe rdma_rxe

# 验证模块已加载
lsmod | grep rdma_rxe
```

#### 步骤 3: 创建 SoftRoCE 设备

```bash
# 查看可用的网络接口
ip link show

# 将 SoftRoCE 绑定到网络接口（替换 eth0 为你的接口名）
sudo rdma link add rxe0 type rxe netdev eth0

# 验证设备
rdma link show
ibv_devices
```

#### 步骤 4: 设置内存锁定

```bash
# 临时设置（当前 shell）
ulimit -l unlimited

# 永久设置（写入 limits.conf，需重新登录）
sudo bash -c 'echo "* soft memlock unlimited" >> /etc/security/limits.conf'
sudo bash -c 'echo "* hard memlock unlimited" >> /etc/security/limits.conf'
```

#### 步骤 5: 验证

```bash
# 查看设备信息
ibv_devinfo

# 预期输出中的关键字段:
#   link_layer: Ethernet    ← SoftRoCE 是以太网链路层
#   state:      PORT_ACTIVE ← 端口应为活动状态
#   port_lid:   0           ← RoCE 模式下 LID 始终为 0（正常!）
```

---

## 3. SoftRoCE 关键概念

### 3.1 LID = 0 是正常的

在 SoftRoCE (RoCE 模式) 下:

```
$ ibv_devinfo | grep lid
    port_lid:  0      ← 这是正常的！RoCE 不使用 LID
    sm_lid:    0      ← 没有 Subnet Manager（只有 IB 需要）
```

**InfiniBand** 使用 **LID (Local Identifier)** 进行子网内寻址（由 Subnet Manager 分配）。
**RoCE** 使用 **GID (Global Identifier)** 进行寻址（基于 IP 地址生成）。

本教程的所有程序都会自动检测传输层类型，选择正确的寻址方式：

```c
// 自动检测（已内置在所有程序中）
enum rdma_transport t = detect_transport(ctx, port);
if (t == RDMA_TRANSPORT_ROCE) {
    // 使用 GID 寻址: ah_attr.is_global = 1, grh.dgid = remote_gid
} else {
    // 使用 LID 寻址: ah_attr.dlid = remote_lid
}
```

### 3.2 GID 从 IP 地址生成

SoftRoCE 的 GID 表中的条目来自网卡的 IP 地址:

```bash
# 查看 GID 表
cat /sys/class/infiniband/rxe0/ports/1/gids/0

# 典型输出: fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx (link-local IPv6)
# 或: 0000:0000:0000:0000:0000:ffff:c0a8:0164 (IPv4-mapped, 即 192.168.1.100)
```

**GID 索引选择:**

| GID 索引 | 类型 | 说明 |
|----------|------|------|
| 0 | RoCE v1 (link-local) | 仅子网内通信 |
| 1 | RoCE v2 (IPv4-mapped) | **推荐使用** — 支持 IP 路由 |
| 2+ | RoCE v2 (IPv6) | 如果配了 IPv6 |

程序中默认使用 `gid_index = 0`，如需调整可修改 `RDMA_DEFAULT_GID_INDEX`。

### 3.3 MTU 注意事项

```bash
# 查看 SoftRoCE 的 MTU
ibv_devinfo | grep mtu
#   max_mtu:    4096
#   active_mtu: 1024    ← SoftRoCE 默认 MTU
```

SoftRoCE 的 active_mtu 受限于底层网卡的 MTU:
- 普通以太网 MTU = 1500 → RDMA active_mtu ≈ 1024
- Jumbo Frame MTU = 9000 → RDMA active_mtu ≈ 4096

本教程程序默认使用 `IBV_MTU_1024`，兼容所有环境。

### 3.4 ulimit -l（内存锁定限制）

RDMA 需要 **pin 内存**（锁定物理内存页，防止被交换出去），
这要求系统允许进程锁定足够的内存。

```bash
# 查看当前限制
ulimit -l
# 如果输出 64 或其他小数字，需要调整

# 临时调整
ulimit -l unlimited

# 如果报 "Operation not permitted"，说明 hard limit 也需调整
# 编辑 /etc/security/limits.conf:
#   * soft memlock unlimited
#   * hard memlock unlimited
# 然后重新登录
```

---

## 4. 持久化配置

默认情况下，SoftRoCE 设备在重启后会消失。要持久化:

### 方法 1: 开机自动加载模块

```bash
# 添加到模块自动加载列表
echo "rdma_rxe" | sudo tee -a /etc/modules-load.d/rdma.conf

# 创建 udev 规则或启动脚本来自动创建 rxe0 设备
sudo bash -c 'cat > /etc/systemd/system/softrce.service << EOF
[Unit]
Description=SoftRoCE Setup
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/rdma link add rxe0 type rxe netdev eth0
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF'

sudo systemctl enable softrce.service
```

### 方法 2: 每次手动创建

```bash
# 重启后运行
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0
```

---

## 5. 程序运行方式速查表

### 单进程程序（直接运行，无需对端）

| 程序 | 路径 | 说明 |
|------|------|------|
| `hello_rdma` | ch09-quickref/ | 最小 RDMA Hello World |
| `01_init_resources` | ch03-verbs-api/01-initialization/ | 六步初始化 |
| `02_multi_device` | ch03-verbs-api/01-initialization/ | 设备枚举 |
| `qp_state` | ch03-verbs-api/02-qp-state/ | QP 状态机 (loopback) |
| `01_qp_error_recovery` | ch03-verbs-api/02-qp-state/ | QP 错误恢复 |
| `pd_isolation` | ch03-verbs-api/03-pd/ | PD 隔离演示 |
| `mr_access_flags` | ch03-verbs-api/04-mr/ | MR 权限标志 |
| `mr_multi_reg` | ch03-verbs-api/04-mr/ | 多次注册 MR |
| `cq_event_driven` | ch03-verbs-api/05-cq/ | 事件驱动 CQ |
| `cq_overflow` | ch03-verbs-api/05-cq/ | CQ 溢出 |
| `01_loopback_send_recv` | ch05-communication/02-send-recv/ | Send/Recv loopback |
| `02_sge_demo` | ch05-communication/02-send-recv/ | Scatter-Gather |
| `ud_loopback` | ch06-connection/03-ud-mode/ | UD 模式 |
| `inline_data` | ch07-engineering/02-tuning/ | Inline 优化 |
| `unsignaled_send` | ch07-engineering/02-tuning/ | 无信号发送 |
| `srq_demo` | ch07-engineering/02-tuning/ | 共享接收队列 |
| `trigger_errors` | ch07-engineering/04-error-handling/ | 错误触发 |
| `error_diagnosis` | ch07-engineering/04-error-handling/ | 错误诊断 |
| `error_cheatsheet` | ch09-quickref/ | 错误速查 |

运行方式：
```bash
ulimit -l unlimited
./ch09-quickref/hello_rdma
```

### 双进程程序（需要两个终端，server + client）

| 程序 | 路径 | 终端1 (Server) | 终端2 (Client) |
|------|------|---------------|----------------|
| `rdma_write` | ch05/.../01-rdma-write/ | `./rdma_write server` | `./rdma_write client 127.0.0.1` |
| `01_write_imm` | ch05/.../01-rdma-write/ | `./01_write_imm server` | `./01_write_imm client 127.0.0.1` |
| `send_recv` | ch05/.../02-send-recv/ | `./send_recv server` | `./send_recv client 127.0.0.1` |
| `03_rnr_error_demo` | ch05/.../02-send-recv/ | `./03_rnr_error_demo server` | `./03_rnr_error_demo client 127.0.0.1` |
| `rdma_read` | ch05/.../03-rdma-read/ | `./rdma_read server` | `./rdma_read client 127.0.0.1` |
| `01_batch_read` | ch05/.../03-rdma-read/ | `./01_batch_read server` | `./01_batch_read client 127.0.0.1` |
| `atomic_ops` | ch05/.../04-atomic/ | `./atomic_ops server` | `./atomic_ops client 127.0.0.1` |
| `01_alignment_error` | ch05/.../04-atomic/ | `./01_alignment_error server` | `./01_alignment_error client 127.0.0.1` |
| `02_spinlock` | ch05/.../04-atomic/ | `./02_spinlock server` | `./02_spinlock client 127.0.0.1` |
| `manual_connect` | ch06/.../01-manual-connect/ | `./manual_connect -s` | `./manual_connect -c 127.0.0.1` |
| `rdma_cm_example` | ch06/.../02-rdma-cm/ | `./rdma_cm_example -s 7471` | `./rdma_cm_example -c 127.0.0.1 7471` |

运行方式：
```bash
# 两个终端都需要
ulimit -l unlimited

# 终端 1: 启动 server
./rdma_write server

# 终端 2: 启动 client (连接到 127.0.0.1)
./rdma_write client 127.0.0.1
```

### 自动化测试

```bash
# 运行所有测试（自动处理 server/client）
./scripts/run_all_tests.sh
```

---

## 6. 常见问题排查

### Q1: `ibv_devices` 没有输出

**原因**: SoftRoCE 未配置
```bash
# 检查模块
lsmod | grep rdma_rxe
# 如果没有输出:
sudo modprobe rdma_rxe

# 检查设备
rdma link show
# 如果没有 rxe 设备:
sudo rdma link add rxe0 type rxe netdev eth0
```

### Q2: `Cannot allocate memory` 或 `ibv_reg_mr failed`

**原因**: 内存锁定限制不足
```bash
ulimit -l
# 如果不是 unlimited:
ulimit -l unlimited

# 永久修复:
sudo bash -c 'echo "* soft memlock unlimited" >> /etc/security/limits.conf'
sudo bash -c 'echo "* hard memlock unlimited" >> /etc/security/limits.conf'
# 重新登录
```

### Q3: QP 状态转换失败 `INIT->RTR failed`

**原因**: GID 配置问题
```bash
# 检查 GID 表是否有有效条目
cat /sys/class/infiniband/rxe0/ports/1/gids/0
# 如果全零，说明网卡没有 IP 地址

# 解决: 确保网卡有 IP
ip addr show
# 如果没有 IP，配置一个
sudo ip addr add 192.168.1.100/24 dev eth0
```

### Q4: `ibv_devinfo` 显示 `state: PORT_DOWN`

**原因**: 底层网卡未 UP
```bash
# 检查网卡状态
ip link show eth0
# 如果是 DOWN:
sudo ip link set eth0 up
```

### Q5: 双进程测试中 client 连不上 server

**原因**: TCP 端口冲突或 server 未启动
```bash
# 检查端口是否被占用
ss -tlnp | grep 9876

# 确保先启动 server，等 1-2 秒再启动 client
# 某些程序使用不同的 TCP 端口:
#   rdma_write: 9876
#   send_recv:  9999
#   rdma_read:  8888
#   atomic_ops: 7777
```

### Q6: SoftRoCE 性能很低

**这是正常的!** SoftRoCE 走的是内核软件协议栈，不是真正的硬件旁路。

| 指标 | SoftRoCE | 真实硬件 |
|------|----------|---------|
| 延迟 | ~50-100 μs | ~1-2 μs |
| 带宽 | ~1-10 Gbps | ~100-400 Gbps |
| CPU 占用 | 高 | 极低 |

SoftRoCE 的价值在于 **学习 API**，而非测试性能。

### Q7: `rdma_cm_example` 连接失败

**原因**: rdma_cm 需要网络可达
```bash
# 确保 127.0.0.1 可用（loopback 接口 UP）
ping -c 1 127.0.0.1

# 确保 rdma_cm 模块已加载
sudo modprobe rdma_cm
```

### Q8: `modprobe rdma_rxe` 失败

**原因**: 内核不支持或模块未编译
```bash
# 检查内核版本（需要 4.8+）
uname -r

# 检查模块是否存在
find /lib/modules/$(uname -r) -name "rdma_rxe*"

# 如果模块不存在，可能需要安装:
sudo apt install linux-modules-extra-$(uname -r)
```

### Q9: Docker/容器中无法使用 SoftRoCE

```bash
# 容器需要额外权限:
docker run --privileged --cap-add=IPC_LOCK ...

# 或者在宿主机上配置 SoftRoCE，容器共享:
docker run --device=/dev/infiniband/uverbs0 ...
```

### Q10: 编译报错 `rdma/rdma_cma.h: No such file`

```bash
sudo apt install librdmacm-dev
```

---

## 7. 一键测试

配置完环境后，验证所有程序:

```bash
# 编译
make all

# 运行全量测试
./scripts/run_all_tests.sh
```

预期输出:
```
=================================================
   测试汇总
=================================================

  总计:   ~30 个测试
  通过:   ~30
  失败:   0
  跳过:   0

  所有测试通过! SoftRoCE 环境完全可用。
```

---

## 下一步

环境就绪后，进入第一章：[RDMA 基础概念](../ch01-intro/README.md)
