# 第二章：网络层 —— InfiniBand / RoCE / iWARP

## 学习目标

- 深入理解三种 RDMA 传输层协议的技术细节
- 掌握 IB / RoCE / iWARP 的寻址差异（LID vs GID）
- 学会使用 Verbs API 检测和查询底层传输类型
- 理解 Verbs 如何在统一抽象层下支持所有传输协议

---

## 2.1 为什么需要了解网络层？

在第一章中，我们介绍了三种 RDMA 协议：InfiniBand、RoCE 和 iWARP。
虽然 Verbs API 提供了统一的编程接口，但在实际编程中，**传输层的差异仍会影响你的代码**。

```
┌─────────────────────────────────────────────────────────────────┐
│                    你的 RDMA 应用程序                            │
├─────────────────────────────────────────────────────────────────┤
│                    libibverbs (Verbs API)                       │
│                    ← 统一抽象层 →                                │
├───────────────┬───────────────┬─────────────────────────────────┤
│  InfiniBand   │    RoCE v2    │           iWARP                 │
│  (LID 寻址)   │  (GID 寻址)   │        (TCP 流)                 │
├───────────────┼───────────────┼─────────────────────────────────┤
│  IB 专用网络   │   以太网       │          TCP/IP                 │
└───────────────┴───────────────┴─────────────────────────────────┘
```

### 关键差异

| 差异点 | InfiniBand | RoCE v1/v2 | iWARP |
|--------|------------|------------|-------|
| **link_layer** | `IBV_LINK_LAYER_INFINIBAND` | `IBV_LINK_LAYER_ETHERNET` | `IBV_LINK_LAYER_ETHERNET` |
| **node_type** | `IBV_NODE_CA` | `IBV_NODE_CA` | `IBV_NODE_RNIC` |
| **寻址方式** | LID (16-bit 本地 ID) | GID (128-bit 全局 ID) | IP 地址 |
| **ah_attr** | `dlid` 字段 | `is_global=1` + `grh.dgid` | 通常用 RDMA CM |
| **子网管理** | 需要 SM (Subnet Manager) | 不需要 | 不需要 |
| **底层网络** | IB 专用交换机 | 标准以太网交换机 | 标准 TCP/IP 网络 |
| **传输层** | IB 原生传输 | UDP (端口 4791) | TCP |

---

## 2.2 InfiniBand 详解

### 2.2.1 IB 网络架构

```
┌──────────┐    IB Link    ┌──────────┐    IB Link    ┌──────────┐
│  HCA      │◀────────────▶│  IB       │◀────────────▶│  HCA      │
│  (网卡)    │              │  Switch   │              │  (网卡)    │
│  LID=1    │              │           │              │  LID=2    │
└──────────┘              └──────────┘              └──────────┘
                                │
                          ┌─────▼─────┐
                          │  Subnet   │
                          │  Manager  │
                          │  (SM)     │
                          └───────────┘
```

### 2.2.2 LID 寻址

IB 使用 16-bit 的 **LID (Local Identifier)** 进行子网内寻址：

- LID 由 **Subnet Manager (SM)** 分配
- 每个端口至少分配一个 LID
- LID 范围：1 ~ 65535 (0 保留)
- 跨子网通信需要 GRH (Global Route Header)

```c
/* IB 模式的 ah_attr 设置 */
struct ibv_ah_attr ah_attr = {
    .dlid       = remote->lid,      /* 目的端口的 LID */
    .sl         = 0,                /* Service Level */
    .port_num   = port,
    .is_global  = 0,                /* IB 子网内不需要 GRH */
};
```

### 2.2.3 端口属性

IB 端口提供丰富的属性信息：

| 字段 | 说明 |
|------|------|
| `state` | 端口状态：DOWN / INIT / ARMED / ACTIVE |
| `lid` | 本地端口 LID |
| `sm_lid` | Subnet Manager 的 LID |
| `active_mtu` | 活动 MTU (256/512/1024/2048/4096) |
| `active_speed` | 链路速率 |
| `active_width` | 链路宽度 (1x/4x/8x/12x) |
| `link_layer` | 链路层类型 |
| `gid_tbl_len` | GID 表长度 |
| `pkey_tbl_len` | P_Key 表长度 |

### 2.2.4 实践文件

| 文件 | 说明 |
|------|------|
| `01-infiniband/ib_env_check.sh` | IB 环境检查脚本 |
| `01-infiniband/ib_port_detail.c` | IB 端口属性完整查询程序 |

---

## 2.3 RoCE 详解

### 2.3.1 RoCE 协议栈

```
RoCE v1:                           RoCE v2:
┌─────────────┐                    ┌─────────────┐
│  IB 传输层    │                    │  IB 传输层    │
├─────────────┤                    ├─────────────┤
│  IB 网络层    │                    │  UDP:4791   │
├─────────────┤                    ├─────────────┤
│  以太网       │                    │  IP (v4/v6) │
└─────────────┘                    ├─────────────┤
                                   │  以太网       │
                                   └─────────────┘
```

**RoCE v2 是目前主流**，因为它支持 IP 路由，可以跨三层网络。

### 2.3.2 GID 寻址

RoCE 不使用 LID，而是使用 **128-bit GID (Global Identifier)**：

- GID 格式与 IPv6 地址相同
- RoCE v2 的 GID 通常基于 IPv4-mapped IPv6 或真实 IPv6
- 每个端口有一个 **GID 表**，包含多个 GID 条目

```
GID 表示例:
  Index 0: fe80::xxxx:xxxx:xxxx:xxxx    (link-local, RoCE v1)
  Index 1: ::ffff:10.0.0.1              (IPv4-mapped, RoCE v2)
  Index 2: fe80::xxxx:xxxx:xxxx:xxxx    (link-local, RoCE v1)
  Index 3: ::ffff:10.0.0.1              (IPv4-mapped, RoCE v2)
```

**GID 索引选择规则：**
- RoCE v1: 使用 `gid_index = 0` (link-local GID)
- RoCE v2: 通常使用 `gid_index = 1` 或 `gid_index = 3` (IPv4-mapped GID)

```c
/* RoCE 模式的 ah_attr 设置 */
struct ibv_ah_attr ah_attr = {
    .dlid       = 0,                /* RoCE 不使用 LID */
    .port_num   = port,
    .is_global  = 1,                /* 必须开启 GRH */
    .grh = {
        .dgid       = remote->gid,  /* 目的端口的 GID */
        .sgid_index = gid_index,     /* 本地 GID 表索引 */
        .hop_limit  = 64,
        .traffic_class = 0,
    },
};
```

### 2.3.3 RoCE 网络要求

RoCE 对网络质量有要求，因为 UDP 不像 TCP 那样重传：

| 机制 | 说明 |
|------|------|
| **PFC (Priority Flow Control)** | 基于优先级的流控，防止丢包 |
| **ECN (Explicit Congestion Notification)** | 显式拥塞通知，防止队列溢出 |
| **DSCP** | 差分服务代码点，QoS 标记 |

### 2.3.4 实践文件

| 文件 | 说明 |
|------|------|
| `02-roce/roce_env_check.sh` | RoCE 环境检查脚本 |
| `02-roce/roce_gid_query.c` | GID 表枚举与分析程序 |

---

## 2.4 iWARP 详解

### 2.4.1 iWARP 协议栈

```
┌─────────────┐
│  RDMA 层     │  (RDMAP - Remote Direct Memory Access Protocol)
├─────────────┤
│  DDP         │  (Direct Data Placement)
├─────────────┤
│  MPA         │  (Marker PDU Aligned Framing)
├─────────────┤
│  TCP         │
├─────────────┤
│  IP          │
├─────────────┤
│  以太网       │
└─────────────┘
```

### 2.4.2 iWARP 特点

- **基于 TCP**：天然支持路由、NAT、防火墙穿越
- **无需无损网络**：TCP 自带重传机制
- **性能略低**：TCP 处理开销 > UDP
- **node_type**：`IBV_NODE_RNIC` (区别于 IB/RoCE 的 `IBV_NODE_CA`)

### 2.4.3 iWARP 常见硬件

| 厂商 | 系列 | 驱动 |
|------|------|------|
| Chelsio | T5/T6 | `iw_cxgb4` |
| Intel | X722/E810 | `i40iw` / `irdma` |

### 2.4.4 iWARP 编程注意

iWARP 通常使用 **RDMA CM** 建立连接（因为底层是 TCP）：

```c
/* iWARP 的典型连接方式 */
rdma_create_id()     /* 创建 CM ID */
rdma_resolve_addr()  /* 解析地址 */
rdma_resolve_route() /* 解析路由 */
rdma_connect()       /* 建立连接 (底层是 TCP handshake) */
```

### 2.4.5 实践文件

| 文件 | 说明 |
|------|------|
| `03-iwarp/iwarp_env_check.sh` | iWARP 环境检查脚本 |
| `03-iwarp/iwarp_query.c` | iWARP 设备查询程序 |

---

## 2.5 Verbs 统一抽象

### 2.5.1 一套代码，三种传输

Verbs API 的设计哲学是：**应用层代码尽量不感知底层传输类型**。

```c
/* 这段代码在 IB / RoCE / iWARP 上都能运行 */
dev_list = ibv_get_device_list(&num_devices);   /* 枚举所有设备 */
ctx = ibv_open_device(dev_list[0]);             /* 打开设备 */
ibv_query_device(ctx, &dev_attr);               /* 查询设备能力 */
ibv_query_port(ctx, 1, &port_attr);             /* 查询端口属性 */

/* 仅在设置 ah_attr 时需要区分传输类型 */
if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
    /* RoCE: 使用 GID 寻址 */
    ah_attr.is_global = 1;
    ah_attr.grh.dgid = remote_gid;
} else {
    /* IB: 使用 LID 寻址 */
    ah_attr.dlid = remote_lid;
}
```

### 2.5.2 传输类型检测方法

```c
/* 方法 1: 通过 link_layer 判断 */
struct ibv_port_attr port_attr;
ibv_query_port(ctx, port, &port_attr);
if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)
    /* RoCE 或 iWARP */
if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND)
    /* InfiniBand */

/* 方法 2: 通过 node_type 判断 */
if (dev->node_type == IBV_NODE_CA)
    /* IB 或 RoCE (Channel Adapter) */
if (dev->node_type == IBV_NODE_RNIC)
    /* iWARP (RDMA NIC) */

/* 方法 3: 结合两者 */
enum rdma_transport detect_transport(ctx, port) {
    if (node_type == IBV_NODE_RNIC)  return IWARP;
    if (link_layer == ETHERNET)       return ROCE;
    return IB;
}
```

### 2.5.3 /dev/infiniband/ 设备文件

```bash
$ ls /dev/infiniband/
rdma_cm        # RDMA CM 字符设备
uverbs0        # 第一个 RDMA 设备的 Verbs 字符设备
uverbs1        # 第二个 RDMA 设备 (如有)

$ ls /sys/class/infiniband/
rxe_0          # SoftRoCE 设备
mlx5_0         # Mellanox CX-5 设备
```

### 2.5.4 实践文件

| 文件 | 说明 |
|------|------|
| `04-verbs-abstraction/verbs_any_transport.c` | 统一抽象演示程序 |

---

## 2.6 SoftRoCE 环境下的实践

如果你使用的是阿里云 ECS + SoftRoCE 环境（如 ch00 中搭建），那么：

```
设备名:      rxe_0
node_type:   IBV_NODE_CA        (与物理 RoCE 相同)
link_layer:  IBV_LINK_LAYER_ETHERNET
transport:   RDMA_TRANSPORT_ROCE

GID 表:
  Index 0: fe80::xxxx:xxxx:xxxx:xxxx    (link-local)
  Index 1: ::ffff:x.x.x.x              (IPv4-mapped, RoCE v2 用这个)
```

**推荐的 GID 索引：**
- SoftRoCE: `gid_index = 1`（对应 IPv4 地址）
- 物理 RoCE: 查看 GID 表选择 RoCE v2 对应的索引

---

## 本章文件结构

```
ch02-network-layer/
├── README.md                              ← 本文件
├── 01-infiniband/
│   ├── ib_env_check.sh                    ← IB 环境检查脚本
│   ├── ib_port_detail.c                   ← IB 端口详细查询
│   └── Makefile
├── 02-roce/
│   ├── roce_env_check.sh                  ← RoCE 环境检查脚本
│   ├── roce_gid_query.c                   ← GID 表枚举分析
│   └── Makefile
├── 03-iwarp/
│   ├── iwarp_env_check.sh                 ← iWARP 环境检查脚本
│   ├── iwarp_query.c                      ← iWARP 设备查询
│   └── Makefile
└── 04-verbs-abstraction/
    ├── verbs_any_transport.c              ← 统一抽象演示
    └── Makefile
```

---

## 学习顺序建议

1. **阅读本 README**：理解三种协议的差异
2. **运行环境检查脚本**：确认你的环境类型
3. **编译并运行 C 程序**：动手实践各种查询
4. **重点关注 02-roce/**：大部分云环境使用 RoCE
5. **理解 04-verbs-abstraction/**：掌握统一编程模型

---

## 练习题

1. InfiniBand 使用什么寻址方式？由谁分配地址？
2. RoCE v2 相比 v1 的最大改进是什么？
3. 在 RoCE 环境中，`ah_attr.is_global` 必须设为几？为什么？
4. 如何通过 Verbs API 区分 IB 和 RoCE 设备？
5. iWARP 的 `node_type` 是什么？与 IB/RoCE 有何不同？
6. 为什么 RoCE 需要 PFC 而 iWARP 不需要？
7. 在 SoftRoCE 环境下，应该选择哪个 GID 索引？

---

## 下一步

进入下一章：[第三章：Verbs API 入门](../ch03-verbs-api/README.md)
