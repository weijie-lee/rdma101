# 第九章：API 速查手册与工具集

**RDMA Verbs API 快速参考、环境检查、错误排查**

---

## 章节概述

本章是一份实用工具集，包含：
1. **完整的 Verbs API 速查表** — 所有常用函数的参数、返回值、注意事项
2. **最小 Hello World 程序** — 可直接复制使用的 RDMA 模板代码
3. **一键环境检查脚本** — 10 项检查确保 RDMA 环境就绪
4. **错误码速查工具** — 20 种 WC 错误的原因和修复建议

## 本章文件

| 文件 | 说明 |
|------|------|
| `hello_rdma.c` | 最小完整 RDMA Send/Recv 程序 (Loopback) |
| `env_check.sh` | 一键环境检查 (10 项) |
| `error_cheatsheet.c` | WC 错误码速查工具 |
| `Makefile` | 编译 hello_rdma 和 error_cheatsheet |

---

## 一、初始化 API

### ibv_get_device_list

```c
struct ibv_device **ibv_get_device_list(int *num_devices);
```

| 项目 | 说明 |
|------|------|
| 功能 | 获取系统中所有 RDMA 设备的列表 |
| 参数 | `num_devices` — 输出设备数量 (可为 NULL) |
| 返回 | 设备指针数组 (NULL 结尾)，失败返回 NULL |
| 释放 | **必须**调用 `ibv_free_device_list()` 释放 |
| 常见错误 | 返回 NULL → 没有 RDMA 设备或驱动未加载 |

### ibv_open_device

```c
struct ibv_context *ibv_open_device(struct ibv_device *device);
```

| 项目 | 说明 |
|------|------|
| 功能 | 打开指定 RDMA 设备，获取上下文 |
| 参数 | `device` — 从 `ibv_get_device_list()` 获取的设备指针 |
| 返回 | 设备上下文，失败返回 NULL |
| 底层 | `open("/dev/infiniband/uverbs0")` |
| 常见错误 | 权限不足 → 需要 root 或 `rdma` 用户组 |

### ibv_query_device

```c
int ibv_query_device(struct ibv_context *context,
                     struct ibv_device_attr *device_attr);
```

| 项目 | 说明 |
|------|------|
| 功能 | 查询设备能力和限制 |
| 关键字段 | `max_qp`, `max_cq`, `max_mr`, `max_qp_wr`, `max_sge`, `max_cqe` |
| 返回 | 0 成功，非零失败 |

### ibv_query_port

```c
int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr);
```

| 项目 | 说明 |
|------|------|
| 功能 | 查询端口属性 |
| 关键字段 | `state` (PORT_ACTIVE=4), `lid`, `link_layer`, `active_mtu` |
| `port_num` | 从 1 开始 (不是 0！) |
| 常见错误 | port_num=0 → 返回 EINVAL |

### ibv_query_gid

```c
int ibv_query_gid(struct ibv_context *context, uint8_t port_num,
                  int index, union ibv_gid *gid);
```

| 项目 | 说明 |
|------|------|
| 功能 | 查询指定端口的 GID 表条目 |
| `index` | GID 索引。RoCE v2 通常用 1 或 3 |
| 用途 | RoCE 模式下获取本地 GID 用于建连 |

---

## 二、资源管理 API

### ibv_alloc_pd

```c
struct ibv_pd *ibv_alloc_pd(struct ibv_context *context);
```

| 项目 | 说明 |
|------|------|
| 功能 | 分配保护域 (Protection Domain) |
| 返回 | PD 指针，失败返回 NULL |
| 说明 | PD 是 QP/MR/AH 的容器，同一 PD 下的资源可互相访问 |
| 释放 | `ibv_dealloc_pd()` — 必须先销毁 PD 下的所有 QP/MR |

### ibv_reg_mr

```c
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr,
                          size_t length, int access);
```

| 项目 | 说明 |
|------|------|
| 功能 | 注册内存区域 (Memory Region) |
| `addr` | 内存起始地址 |
| `length` | 内存大小 (字节) |
| `access` | 访问权限标志位组合 |
| 返回 | MR 指针 (含 `lkey` 和 `rkey`)，失败返回 NULL |
| 释放 | `ibv_dereg_mr()` |

**access 标志位：**

| 标志 | 值 | 说明 |
|------|----|------|
| `IBV_ACCESS_LOCAL_WRITE` | 1 | 本地写 (几乎总是需要) |
| `IBV_ACCESS_REMOTE_WRITE` | 2 | 允许远端 RDMA Write |
| `IBV_ACCESS_REMOTE_READ` | 4 | 允许远端 RDMA Read |
| `IBV_ACCESS_REMOTE_ATOMIC` | 8 | 允许远端 Atomic |

**常见错误：**
- 返回 NULL + ENOMEM → `ulimit -l` 不够，需要增大锁定内存限制
- 返回 NULL + EINVAL → `access` 包含 REMOTE_WRITE 但没有 LOCAL_WRITE

### ibv_create_cq

```c
struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                              void *cq_context,
                              struct ibv_comp_channel *channel,
                              int comp_vector);
```

| 项目 | 说明 |
|------|------|
| 功能 | 创建完成队列 (Completion Queue) |
| `cqe` | CQ 容量 (最大条目数)，建议 >= QP 的 send_wr + recv_wr |
| `cq_context` | 用户自定义上下文指针 (回调用)，一般填 NULL |
| `channel` | 完成通知通道 (事件模式用)，轮询模式填 NULL |
| `comp_vector` | 完成向量 (中断亲和性)，一般填 0 |
| 释放 | `ibv_destroy_cq()` — 必须先销毁引用此 CQ 的所有 QP |

### ibv_create_qp

```c
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                              struct ibv_qp_init_attr *qp_init_attr);
```

| 项目 | 说明 |
|------|------|
| 功能 | 创建队列对 (Queue Pair) |
| 返回 | QP 指针 (初始状态为 RESET)，失败返回 NULL |
| 释放 | `ibv_destroy_qp()` |

**ibv_qp_init_attr 关键字段：**

```c
struct ibv_qp_init_attr qp_init_attr = {
    .send_cq = cq,                  /* 发送完成队列 */
    .recv_cq = cq,                  /* 接收完成队列 (可以和 send_cq 相同) */
    .qp_type = IBV_QPT_RC,          /* QP 类型: RC (可靠连接) */
    .cap = {
        .max_send_wr  = 128,        /* 最大发送 WR 数 */
        .max_recv_wr  = 128,        /* 最大接收 WR 数 */
        .max_send_sge = 1,          /* 每个发送 WR 最大 SGE 数 */
        .max_recv_sge = 1,          /* 每个接收 WR 最大 SGE 数 */
    },
};
```

---

## 三、QP 状态机 — 完整参数模板

QP 必须按 RESET → INIT → RTR → RTS 的顺序转换才能发送数据。

### RESET → INIT

```c
struct ibv_qp_attr attr = {
    .qp_state        = IBV_QPS_INIT,
    .pkey_index      = 0,           /* P_Key 索引，通常为 0 */
    .port_num        = 1,           /* 端口号，从 1 开始 */
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                       IBV_ACCESS_REMOTE_WRITE |
                       IBV_ACCESS_REMOTE_READ,
};

int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
           IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

ibv_modify_qp(qp, &attr, mask);
```

### INIT → RTR (Ready to Receive)

```c
struct ibv_qp_attr attr = {
    .qp_state              = IBV_QPS_RTR,
    .path_mtu              = IBV_MTU_1024,    /* 路径 MTU */
    .dest_qp_num           = remote_qp_num,   /* 对端 QP 编号 */
    .rq_psn                = remote_psn,       /* 对端起始 PSN */
    .max_dest_rd_atomic    = 1,                /* 对端最大 RDMA Read 并发数 */
    .min_rnr_timer         = 12,               /* RNR NAK 重试超时 */
    .ah_attr = {
        .dlid          = remote_lid,           /* 对端 LID (IB 模式) */
        .sl            = 0,                    /* Service Level */
        .src_path_bits = 0,
        .port_num      = 1,                    /* 本地端口号 */
        /* RoCE 模式还需要设置: */
        .is_global     = 1,                    /* 启用 GRH (RoCE 必须) */
        .grh = {
            .dgid          = remote_gid,       /* 对端 GID */
            .sgid_index    = local_gid_index,  /* 本地 GID 索引 */
            .flow_label    = 0,
            .hop_limit     = 1,
            .traffic_class = 0,
        },
    },
};

int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
           IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

ibv_modify_qp(qp, &attr, mask);
```

### RTR → RTS (Ready to Send)

```c
struct ibv_qp_attr attr = {
    .qp_state      = IBV_QPS_RTS,
    .timeout       = 14,      /* 本地 ACK 超时: 4.096μs * 2^14 ≈ 67ms */
    .retry_cnt     = 7,       /* 重传次数 (0-7, 7=无限重试) */
    .rnr_retry     = 7,       /* RNR 重试次数 (7=无限重试) */
    .sq_psn        = local_psn, /* 本地起始 PSN */
    .max_rd_atomic = 1,       /* 本地最大 RDMA Read 发起数 */
};

int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
           IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

ibv_modify_qp(qp, &attr, mask);
```

---

## 四、数据传输 API

### ibv_post_send

```c
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr);
```

| 项目 | 说明 |
|------|------|
| 功能 | 提交发送工作请求 |
| `wr` | 工作请求链表 (可一次提交多个) |
| `bad_wr` | 出错时指向第一个失败的 WR |
| 返回 | 0 成功，非零失败 (EINVAL/ENOMEM) |
| **重要** | 这是纯用户态操作，不经过内核！ |

**Send 操作模板：**

```c
struct ibv_sge sge = {
    .addr   = (uint64_t)buf,      /* 数据缓冲区地址 */
    .length = msg_size,            /* 数据长度 */
    .lkey   = mr->lkey,           /* MR 的 lkey */
};

struct ibv_send_wr wr = {
    .wr_id      = 0,              /* 用户自定义 ID (在 WC 中返回) */
    .sg_list    = &sge,           /* SGE 列表 */
    .num_sge    = 1,              /* SGE 数量 */
    .opcode     = IBV_WR_SEND,    /* 操作类型 */
    .send_flags = IBV_SEND_SIGNALED, /* 产生完成事件 */
    .next       = NULL,           /* 下一个 WR (链表) */
};
```

**RDMA Write 操作模板：**

```c
struct ibv_send_wr wr = {
    .wr_id      = 0,
    .sg_list    = &sge,
    .num_sge    = 1,
    .opcode     = IBV_WR_RDMA_WRITE,
    .send_flags = IBV_SEND_SIGNALED,
    .wr.rdma = {
        .remote_addr = remote_buf_addr,  /* 远端 MR 虚拟地址 */
        .rkey        = remote_rkey,      /* 远端 MR 的 rkey */
    },
};
```

**RDMA Read 操作模板：**

```c
struct ibv_send_wr wr = {
    .wr_id      = 0,
    .sg_list    = &sge,
    .num_sge    = 1,
    .opcode     = IBV_WR_RDMA_READ,
    .send_flags = IBV_SEND_SIGNALED,
    .wr.rdma = {
        .remote_addr = remote_buf_addr,
        .rkey        = remote_rkey,
    },
};
```

### ibv_post_recv

```c
int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                  struct ibv_recv_wr **bad_wr);
```

| 项目 | 说明 |
|------|------|
| 功能 | 提交接收工作请求 (必须在对端 Send 之前调用！) |
| **重要** | RC 模式: 每次 Send 消耗一个 Recv WR，不足会导致 RNR |

**Recv 操作模板：**

```c
struct ibv_sge sge = {
    .addr   = (uint64_t)recv_buf,
    .length = buf_size,
    .lkey   = recv_mr->lkey,
};

struct ibv_recv_wr wr = {
    .wr_id   = 0,
    .sg_list = &sge,
    .num_sge = 1,
    .next    = NULL,
};
```

### ibv_poll_cq

```c
int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc);
```

| 项目 | 说明 |
|------|------|
| 功能 | 轮询完成队列 |
| `num_entries` | 最多取回的 WC 数量 |
| 返回 | 正数=取回的 WC 数量，0=CQ 为空，负数=错误 |
| **重要** | 这是纯用户态操作，不经过内核！ |
| **重要** | 必须检查 `wc.status == IBV_WC_SUCCESS` |

---

## 五、清理 API (逆序释放)

资源释放必须按创建的逆序进行，否则会返回 EBUSY：

```c
/* 清理顺序: QP → MR → CQ → PD → 设备 → 设备列表 */
ibv_destroy_qp(qp);              /* 1. 销毁 QP */
ibv_dereg_mr(mr);                /* 2. 注销 MR */
ibv_destroy_cq(cq);              /* 3. 销毁 CQ */
ibv_dealloc_pd(pd);              /* 4. 释放 PD */
ibv_close_device(ctx);           /* 5. 关闭设备 */
ibv_free_device_list(dev_list);  /* 6. 释放设备列表 */
```

**常见错误：**
- `ibv_dealloc_pd()` 返回 EBUSY → PD 下还有 QP 或 MR 未释放
- `ibv_destroy_cq()` 返回 EBUSY → 还有 QP 引用此 CQ

---

## 六、关键数据结构

### ibv_sge (Scatter/Gather Element)

```c
struct ibv_sge {
    uint64_t addr;    /* 缓冲区虚拟地址 */
    uint32_t length;  /* 数据长度 (字节) */
    uint32_t lkey;    /* MR 的 lkey */
};
```

### ibv_send_wr (Send Work Request)

```c
struct ibv_send_wr {
    uint64_t             wr_id;       /* 用户自定义 ID */
    struct ibv_send_wr  *next;        /* 链表指针 */
    struct ibv_sge      *sg_list;     /* SGE 数组 */
    int                  num_sge;     /* SGE 数量 */
    enum ibv_wr_opcode   opcode;      /* 操作码 */
    int                  send_flags;  /* 标志位 */
    union {
        struct { uint64_t remote_addr; uint32_t rkey; } rdma;  /* RDMA R/W */
        struct { uint64_t remote_addr; uint64_t compare_add; uint64_t swap;
                 uint32_t rkey; } atomic;  /* Atomic */
    } wr;
    uint32_t imm_data;  /* 立即数据 (IBV_WR_SEND_WITH_IMM) */
};
```

**操作码 (opcode)：**

| 操作码 | 说明 | 需要对端 Recv WR |
|--------|------|-----------------|
| `IBV_WR_SEND` | Send | 是 |
| `IBV_WR_SEND_WITH_IMM` | Send + 立即数据 | 是 |
| `IBV_WR_RDMA_WRITE` | RDMA Write | 否 |
| `IBV_WR_RDMA_WRITE_WITH_IMM` | RDMA Write + 立即数据 | 是 |
| `IBV_WR_RDMA_READ` | RDMA Read | 否 |
| `IBV_WR_ATOMIC_CMP_AND_SWP` | Atomic CAS | 否 |
| `IBV_WR_ATOMIC_FETCH_AND_ADD` | Atomic FAA | 否 |

**send_flags 标志：**

| 标志 | 说明 |
|------|------|
| `IBV_SEND_SIGNALED` | 操作完成后产生 CQE |
| `IBV_SEND_FENCE` | 等待之前的 RDMA Read 完成 |
| `IBV_SEND_INLINE` | 数据内联到 WQE (小消息优化, ≤ 64B) |

### ibv_recv_wr (Receive Work Request)

```c
struct ibv_recv_wr {
    uint64_t             wr_id;     /* 用户自定义 ID */
    struct ibv_recv_wr  *next;      /* 链表指针 */
    struct ibv_sge      *sg_list;   /* SGE 数组 */
    int                  num_sge;   /* SGE 数量 */
};
```

### ibv_wc (Work Completion)

```c
struct ibv_wc {
    uint64_t              wr_id;       /* 对应 WR 的用户自定义 ID */
    enum ibv_wc_status    status;      /* 完成状态 (0=成功) */
    enum ibv_wc_opcode    opcode;      /* 操作类型 */
    uint32_t              vendor_err;  /* 厂商特定错误码 */
    uint32_t              byte_len;    /* 接收到的字节数 (仅 Recv) */
    uint32_t              imm_data;    /* 立即数据 (网络字节序) */
    uint32_t              qp_num;      /* 本地 QP 编号 */
    uint32_t              src_qp;      /* 远端 QP 编号 (仅 UD/Recv) */
    int                   wc_flags;    /* 标志位 (IBV_WC_WITH_IMM 等) */
};
```

---

## 七、WC 状态码表

| 枚举值 | 数值 | 含义 | 最常见原因 |
|--------|------|------|-----------|
| `IBV_WC_SUCCESS` | 0 | 成功 | — |
| `IBV_WC_LOC_LEN_ERR` | 1 | 本地长度错误 | Recv 缓冲区太小 |
| `IBV_WC_LOC_QP_OP_ERR` | 2 | 本地 QP 操作错误 | QP 未转到 RTS 就发送 |
| `IBV_WC_LOC_EEC_OP_ERR` | 3 | 本地 EEC 操作错误 | (RD QP 相关，很少见) |
| `IBV_WC_LOC_PROT_ERR` | 4 | 本地保护错误 | MR 权限不匹配 |
| `IBV_WC_WR_FLUSH_ERR` | 5 | WR 被冲刷 | QP 进入 Error 状态 |
| `IBV_WC_MW_BIND_ERR` | 6 | MW 绑定错误 | Memory Window 操作失败 |
| `IBV_WC_BAD_RESP_ERR` | 7 | 响应错误 | 收到意外的响应包 |
| `IBV_WC_LOC_ACCESS_ERR` | 8 | 本地访问错误 | lkey 错误或 MR 已注销 |
| `IBV_WC_REM_INV_REQ_ERR` | 9 | 远端无效请求 | 对端 QP 参数错误 |
| `IBV_WC_REM_ACCESS_ERR` | 10 | 远端访问错误 | 对端 rkey/地址/权限错误 |
| `IBV_WC_REM_OP_ERR` | 11 | 远端操作错误 | 对端内部错误 |
| `IBV_WC_RETRY_EXC_ERR` | 12 | 重传超限 | 网络不通/对端宕机 |
| `IBV_WC_RNR_RETRY_EXC_ERR` | 13 | RNR 重试超限 | 对端没有 Post Recv |
| `IBV_WC_LOC_RDD_VIOL_ERR` | 14 | 本地 RDD 违规 | (RD QP 相关) |
| `IBV_WC_REM_INV_RD_REQ_ERR` | 15 | 远端无效 RD 请求 | (RD QP 相关) |
| `IBV_WC_REM_ABORT_ERR` | 16 | 远端中止 | 对端 QP 被销毁 |
| `IBV_WC_INV_EECN_ERR` | 17 | 无效 EECN | (RD QP 相关) |
| `IBV_WC_INV_EEC_STATE_ERR` | 18 | 无效 EEC 状态 | (RD QP 相关) |
| `IBV_WC_FATAL_ERR` | 19 | 致命错误 | HCA 硬件错误 |
| `IBV_WC_RESP_TIMEOUT_ERR` | 20 | 响应超时 | 类似 RETRY_EXC |
| `IBV_WC_GENERAL_ERR` | 21 | 通用错误 | 其他未分类错误 |

---

## 八、常用 Shell 命令

### 设备查看

```bash
# 列出所有 RDMA 设备
ibv_devices

# 查看设备详细信息
ibv_devinfo
ibv_devinfo -d mlx5_0         # 指定设备
ibv_devinfo -d mlx5_0 -v      # 详细模式

# 查看端口状态 (类似 ifconfig for IB)
ibstat
ibstat mlx5_0

# 查看 IB 子网管理信息
ibstatus
```

### 性能测试 (perftest 工具集)

```bash
# 带宽测试
# 服务端:
ib_write_bw
# 客户端:
ib_write_bw <server_ip>

# 延迟测试
# 服务端:
ib_write_lat
# 客户端:
ib_write_lat <server_ip>

# 其他测试:
ib_read_bw / ib_read_lat     # RDMA Read
ib_send_bw / ib_send_lat     # Send/Recv
ib_atomic_bw / ib_atomic_lat # Atomic

# 常用参数:
#   -d mlx5_0    指定设备
#   -s 4096      消息大小
#   -n 1000      迭代次数
#   -F           不使用 event (轮询模式)
#   --report_gbits  以 Gbit/s 报告
```

### 诊断工具

```bash
# 查看 RDMA 内核模块
lsmod | grep -E "rdma|ib_|mlx"

# 查看 rdma-core 版本
dpkg -l | grep rdma-core     # Debian/Ubuntu
rpm -qa | grep rdma-core     # CentOS/RHEL

# 查看锁定内存限制 (RDMA 注册 MR 需要)
ulimit -l

# 查看 /dev/infiniband/ 设备文件
ls -la /dev/infiniband/

# 查看 GID 表 (RoCE 模式用)
cat /sys/class/infiniband/mlx5_0/ports/1/gids/0
cat /sys/class/infiniband/mlx5_0/ports/1/gid_attrs/types/0

# 用 rdma 工具查看 (iproute2)
rdma dev show
rdma link show
rdma res show qp
rdma stat show
```

---

## 学习建议

1. **收藏本页** — 编程时随时查阅 API 参数和错误码
2. **从 hello_rdma.c 开始** — 这是最小可运行模板，修改它来做实验
3. **先运行 env_check.sh** — 确保环境正确再写代码
4. **遇到错误查 error_cheatsheet** — 它会告诉你原因和修复方法

---

*本章是 RDMA 101 教程的最后一章。祝你 RDMA 编程愉快！*
