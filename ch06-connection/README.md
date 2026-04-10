# 第六章：RDMA 连接建立

## 学习目标

| 目标 | 说明 |
|------|------|
| 掌握手动 TCP 交换建连 | 通过 Socket 交换 QPN/LID/GID，手动完成 QP 状态转换 |
| 掌握 RDMA CM 建连 | 使用高级连接管理 API，自动完成地址解析和 QP 管理 |
| 理解 UD 传输模式 | 无连接的数据报模式，理解 Address Handle 概念 |
| 掌握 RC/UC/UD 区别 | 根据场景选择合适的传输模式 |
| 理解 IB/RoCE 双模适配 | 同一程序自动适配 InfiniBand 和 RoCE 网络 |

---

## 6.1 为什么连接建立很重要？

RDMA 的高性能来自于 **绕过内核的数据通路 (data path)**，但在数据传输之前，两端必须先完成 **控制通路 (control path)** 上的握手：

```
                  控制通路 (慢路径)                     数据通路 (快路径)
  ┌─────────────────────────────────────┐    ┌──────────────────────────┐
  │  交换 QPN, LID/GID, PSN, rkey...   │    │  RDMA Send/Write/Read    │
  │  QP 状态: RESET → INIT → RTR → RTS │    │  零拷贝, 内核旁路        │
  │  (一次性开销, 通常走 TCP/RDMA CM)    │    │  (每次操作, 纳秒级延迟)   │
  └─────────────────────────────────────┘    └──────────────────────────┘
```

本章介绍三种连接建立方式，覆盖从底层到高层、从有连接到无连接的全部场景。

---

## 6.2 三种连接方式对比

| 特性 | 手动 TCP 交换 | RDMA CM | UD (无连接) |
|------|--------------|---------|-------------|
| **复杂度** | 中等 | 低 | 高 |
| **灵活性** | 最高 | 中等 | 高 |
| **适用传输** | RC / UC | RC / UC | UD |
| **依赖库** | libibverbs | librdmacm | libibverbs |
| **QP 管理** | 手动状态转换 | CM 自动管理 | 手动 (无 dest_qp) |
| **地址解析** | 用户自行处理 | CM 自动解析 | 用户创建 AH |
| **多播支持** | 不支持 | 支持 | 原生支持 |
| **典型场景** | 高性能存储、学习 | 通用应用 | 高扇出、多播 |

### 选择指南

```
需要可靠传输 (保序、重传)?
     │
     ├─ Yes → RC 模式 (Reliable Connected)
     │         │
     │         ├─ 想要最大灵活性? → 手动 TCP 交换 (01-manual-connect)
     │         └─ 想要简单易用?   → RDMA CM (02-rdma-cm)
     │
     └─ No → 需要一对多通信?
              │
              ├─ Yes → UD 模式 (Unreliable Datagram)
              │         → 使用 Address Handle (03-ud-mode)
              │
              └─ No → UC 模式 (Unreliable Connected)
                       → 手动 TCP 交换, 但不保证可靠
```

---

## 6.3 RC / UC / UD 传输模式详解

### RC (Reliable Connected)

- **一对一连接**：每对通信端点需要一个独立的 QP 对
- **可靠传输**：硬件保证保序、重传、去重
- **支持所有操作**：Send/Recv、RDMA Write、RDMA Read、Atomic
- **状态转换**：RESET → INIT → RTR (需指定 dest_qp_num) → RTS

```c
/* RC 的 RTR 需要指定对端 QP 编号 */
attr.dest_qp_num = remote->qp_num;   // ← RC 特有
attr.rq_psn      = remote->psn;
```

### UC (Unreliable Connected)

- **一对一连接**：同 RC，需要 QP 对
- **不可靠**：无重传、无确认，丢包由上层处理
- **支持操作**：Send/Recv、RDMA Write（不支持 RDMA Read、Atomic）
- **适用场景**：可容忍丢包的批量传输

### UD (Unreliable Datagram)

- **无连接**：一个 QP 可以向多个目标发送
- **不可靠**：同 UC，无重传
- **仅支持 Send/Recv**：不支持 RDMA Write/Read/Atomic
- **消息大小限制**：最大 MTU 大小 (通常 4096 字节)
- **GRH 头部**：接收缓冲区需额外 40 字节用于 GRH (Global Route Header)

```c
/* UD 的关键区别 */

/* 1. QP 类型 */
qp_init_attr.qp_type = IBV_QPT_UD;    // 不是 IBV_QPT_RC

/* 2. RTR 不需要 dest_qp_num (无连接!) */
attr.qp_state = IBV_QPS_RTR;
// 不需要 attr.dest_qp_num, attr.rq_psn 等

/* 3. 发送时指定 Address Handle */
wr.wr.ud.ah         = ah;              // 地址句柄 (指向目标)
wr.wr.ud.remote_qpn = remote_qpn;     // 目标 QP 编号
wr.wr.ud.remote_qkey = 0x11111111;     // Q_Key 匹配

/* 4. 接收缓冲区需额外 40 字节 GRH */
char recv_buf[40 + MSG_SIZE];          // 前 40 字节是 GRH
char *payload = recv_buf + 40;         // 实际数据从偏移 40 开始
```

---

## 6.4 Address Handle (地址句柄) 概念

UD 模式的核心概念是 **Address Handle (AH)**，它封装了到达目标的路由信息：

```
┌─────────────────────────┐
│    Address Handle (AH)   │
├─────────────────────────┤
│ dlid     = 远端 LID      │  ← IB 模式
│ is_global = 1            │  ← RoCE 必须
│ grh.dgid = 远端 GID      │  ← RoCE 模式
│ sl       = Service Level │
│ port_num = 本地端口       │
└─────────────────────────┘
```

**AH vs QP 连接的区别**：
- RC/UC：路由信息绑定在 QP 的 RTR 属性中 (一对一)
- UD：路由信息封装在 AH 中，发送时指定 (一对多)

```c
/* 创建 Address Handle */
struct ibv_ah_attr ah_attr = {
    .dlid     = remote_lid,    /* IB: 目标 LID */
    .sl       = 0,
    .port_num = 1,
    .is_global = is_roce,      /* RoCE: 必须设为 1 */
};
if (is_roce) {
    ah_attr.grh.dgid       = remote_gid;
    ah_attr.grh.sgid_index = local_gid_index;
    ah_attr.grh.hop_limit  = 64;
}
struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
```

---

## 6.5 示例程序

### 01-manual-connect：手动 TCP 交换建连

经典的双进程 RDMA 连接程序，展示完整的建连流程：

```bash
cd 01-manual-connect && make

# 终端1 (服务器)
./manual_connect -s 7471

# 终端2 (客户端)
./manual_connect -c 192.168.1.100 7471
```

**流程**：
1. 创建 RDMA 资源 (PD, CQ, QP, MR)
2. 自动检测 IB/RoCE 传输类型
3. 填充本地端点信息 (QPN, LID, GID, PSN)
4. 通过 TCP Socket 交换端点信息
5. `qp_full_connect()` 完成 RESET → INIT → RTR → RTS
6. 双向 Send/Recv 验证连接

### 02-rdma-cm：RDMA CM 连接管理

使用 `librdmacm` 高级 API，自动完成地址解析和 QP 管理：

```bash
cd 02-rdma-cm && make

# 终端1 (服务器)
./rdma_cm_example -s 7471

# 终端2 (客户端)
./rdma_cm_example -c 127.0.0.1 7471
```

**CM 事件流**：

```
  Server                                Client
  ──────                                ──────
  rdma_listen()
                                        rdma_resolve_addr()
                                        ← RDMA_CM_EVENT_ADDR_RESOLVED
                                        rdma_resolve_route()
                                        ← RDMA_CM_EVENT_ROUTE_RESOLVED
                                        rdma_connect()
  ← RDMA_CM_EVENT_CONNECT_REQUEST
  rdma_accept()
  ← RDMA_CM_EVENT_ESTABLISHED          ← RDMA_CM_EVENT_ESTABLISHED
  
  ========== 连接已建立，可以通信 ==========
```

### 03-ud-mode：UD 无连接模式

单机 Loopback 演示 UD 模式的核心概念：

```bash
cd 03-ud-mode && make

./ud_loopback
```

**与 RC 的关键区别**：
- QP 类型: `IBV_QPT_UD` (不是 `IBV_QPT_RC`)
- RTR 转换更简单 (无需 dest_qp_num)
- 发送需指定 Address Handle
- 接收缓冲区多 40 字节 GRH 头部

---

## 6.6 连接建立流程详解

### 手动 TCP 交换的完整流程

```
  Server                    TCP             Client
  ──────                   ─────            ──────
  1. 创建 RDMA 资源                         1. 创建 RDMA 资源
     (PD, CQ, QP, MR)                         (PD, CQ, QP, MR)
  
  2. 填充本地端点:                           2. 填充本地端点:
     {QPN, LID, GID, PSN}                      {QPN, LID, GID, PSN}
  
  3. TCP 监听          ◄── TCP连接 ──►      3. TCP 连接服务器
  
  4. 交换端点信息       ◄── 双向交换 ──►      4. 交换端点信息
     收到 remote_ep                            收到 remote_ep
  
  5. qp_full_connect()                      5. qp_full_connect()
     RESET → INIT                              RESET → INIT
     INIT  → RTR                               INIT  → RTR
     RTR   → RTS                               RTR   → RTS
  
  6. Send/Recv 通信     ◄── RDMA ──►         6. Send/Recv 通信
```

### RDMA CM 的完整流程

```
  Server                    CM              Client
  ──────                   ────             ──────
  rdma_create_event_channel()               rdma_create_event_channel()
  rdma_create_id()                          rdma_create_id()
  rdma_bind_addr()
  rdma_listen()
                                            rdma_resolve_addr()
                                            → ADDR_RESOLVED
                                            rdma_resolve_route()
                                            → ROUTE_RESOLVED
                                            创建 QP (在 cm_id 上)
                                            rdma_connect()
  → CONNECT_REQUEST
  创建 QP (在新 cm_id 上)
  rdma_accept()
  → ESTABLISHED                             → ESTABLISHED
  
  ============ QP 已在 RTS 状态 ============
```

---

## 6.7 常见问题

### Q1: 为什么不直接用 RDMA CM？

RDMA CM 更简单，但手动方式有以下优势：
- **更底层的控制**：可以精确调整每个 QP 属性
- **无 librdmacm 依赖**：某些嵌入式环境可能没有
- **性能调优**：大规模连接时可以优化交换协议
- **学习价值**：理解底层原理对调试非常重要

### Q2: UD 模式的 40 字节 GRH 是什么？

GRH (Global Route Header) 包含源/目的 GID 等路由信息。UD 模式下，硬件会在每个接收到的消息前面附加 40 字节的 GRH：

```
接收缓冲区布局:
┌──────────────────┬──────────────────────┐
│  GRH (40 bytes)  │  实际数据 (payload)   │
└──────────────────┴──────────────────────┘
  ↑                   ↑
  网卡自动填充         应用数据从这里开始
```

### Q3: RC 建连时两端的顺序重要吗？

对于手动 TCP 交换方式，两端的 QP 状态转换可以不同步执行，但必须确保：
- **RTR 需要对端信息**：转到 RTR 前必须已获得对端的 QPN/LID/GID
- **RTS 可以先执行**：一端先到 RTS 不影响另一端

### Q4: IB 和 RoCE 的建连有什么区别？

| 步骤 | InfiniBand | RoCE |
|------|-----------|------|
| 寻址 | 使用 LID (16-bit) | 使用 GID (128-bit, IPv6 格式) |
| AH 属性 | `is_global = 0`, 设 `dlid` | `is_global = 1`, 设 `grh.dgid` |
| GID 索引 | 通常不关心 | 必须正确设置 `sgid_index` |
| 子网 | SM 分配 LID | IP 协议栈配置 |

---

## 6.8 编译与运行

### 前置依赖

```bash
# 安装开发库
sudo apt-get install libibverbs-dev librdmacm-dev

# 确认设备可用
ibv_devices
ibv_devinfo
```

### 编译全部示例

```bash
# 先编译公共库
make -C ../../common

# 编译本章全部示例
cd 01-manual-connect && make
cd ../02-rdma-cm && make
cd ../03-ud-mode && make
```

---

## 6.9 练习题

1. **概念题**：解释 RC、UC、UD 三种传输模式各适合什么场景？
2. **编程题**：修改 `manual_connect.c`，在 Send 消息中附带 Immediate Data
3. **分析题**：如果 RDMA CM 服务端未调用 `rdma_ack_cm_event()`，会发生什么？
4. **实践题**：用 UD 模式实现一个简单的组播 (multicast) Demo
5. **对比题**：手动 TCP 交换和 RDMA CM 建连，哪种更适合大规模集群？为什么？

---

## 下一步

完成本章后，你已掌握 RDMA 连接建立的全部方式。接下来：

- [ch07-engineering](../ch07-engineering/README.md) - 工程实践与性能优化
- [ch08-advanced](../ch08-advanced/README.md) - 高级主题
