# Chapter 9: API Quick Reference and Toolkit

**RDMA Verbs API Quick Reference, Environment Check, Error Troubleshooting**

---

## Chapter Overview

This chapter is a practical toolkit that includes:
1. **Complete Verbs API Quick Reference** — Parameters, return values, and notes for all commonly used functions
2. **Minimal Hello World Program** — A ready-to-copy RDMA template code
3. **One-Click Environment Check Script** — 10 checks to ensure RDMA environment readiness
4. **Error Code Lookup Tool** — Causes and fix suggestions for 20 WC errors

## Files in This Chapter

| File | Description |
|------|-------------|
| `hello_rdma.c` | Minimal complete RDMA Send/Recv program (Loopback) |
| `env_check.sh` | One-click environment check (10 items) |
| `error_cheatsheet.c` | WC error code lookup tool |
| `Makefile` | Build hello_rdma and error_cheatsheet |

---

## I. Initialization API

### ibv_get_device_list

```c
struct ibv_device **ibv_get_device_list(int *num_devices);
```

| Field | Description |
|-------|-------------|
| Function | Get a list of all RDMA devices in the system |
| Parameter | `num_devices` — outputs the number of devices (can be NULL) |
| Return | Device pointer array (NULL-terminated), returns NULL on failure |
| Free | **Must** call `ibv_free_device_list()` to free |
| Common Error | Returns NULL — no RDMA devices or driver not loaded |

### ibv_open_device

```c
struct ibv_context *ibv_open_device(struct ibv_device *device);
```

| Field | Description |
|-------|-------------|
| Function | Open a specified RDMA device and get a context |
| Parameter | `device` — device pointer from `ibv_get_device_list()` |
| Return | Device context, returns NULL on failure |
| Underlying | `open("/dev/infiniband/uverbs0")` |
| Common Error | Permission denied — need root or `rdma` user group |

### ibv_query_device

```c
int ibv_query_device(struct ibv_context *context,
                     struct ibv_device_attr *device_attr);
```

| Field | Description |
|-------|-------------|
| Function | Query device capabilities and limits |
| Key Fields | `max_qp`, `max_cq`, `max_mr`, `max_qp_wr`, `max_sge`, `max_cqe` |
| Return | 0 on success, non-zero on failure |

### ibv_query_port

```c
int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr);
```

| Field | Description |
|-------|-------------|
| Function | Query port attributes |
| Key Fields | `state` (PORT_ACTIVE=4), `lid`, `link_layer`, `active_mtu` |
| `port_num` | Starts from 1 (not 0!) |
| Common Error | port_num=0 — returns EINVAL |

### ibv_query_gid

```c
int ibv_query_gid(struct ibv_context *context, uint8_t port_num,
                  int index, union ibv_gid *gid);
```

| Field | Description |
|-------|-------------|
| Function | Query a GID table entry for the specified port |
| `index` | GID index. RoCE v2 typically uses 1 or 3 |
| Purpose | Get local GID for connection establishment in RoCE mode |

---

## II. Resource Management API

### ibv_alloc_pd

```c
struct ibv_pd *ibv_alloc_pd(struct ibv_context *context);
```

| Field | Description |
|-------|-------------|
| Function | Allocate a Protection Domain |
| Return | PD pointer, returns NULL on failure |
| Note | PD is a container for QP/MR/AH; resources under the same PD can access each other |
| Free | `ibv_dealloc_pd()` — must first destroy all QP/MR under the PD |

### ibv_reg_mr

```c
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr,
                          size_t length, int access);
```

| Field | Description |
|-------|-------------|
| Function | Register a Memory Region |
| `addr` | Memory start address |
| `length` | Memory size (bytes) |
| `access` | Access permission flag combination |
| Return | MR pointer (contains `lkey` and `rkey`), returns NULL on failure |
| Free | `ibv_dereg_mr()` |

**access flags:**

| Flag | Value | Description |
|------|-------|-------------|
| `IBV_ACCESS_LOCAL_WRITE` | 1 | Local write (almost always needed) |
| `IBV_ACCESS_REMOTE_WRITE` | 2 | Allow remote RDMA Write |
| `IBV_ACCESS_REMOTE_READ` | 4 | Allow remote RDMA Read |
| `IBV_ACCESS_REMOTE_ATOMIC` | 8 | Allow remote Atomic |

**Common errors:**
- Returns NULL + ENOMEM — `ulimit -l` too low, need to increase locked memory limit
- Returns NULL + EINVAL — `access` includes REMOTE_WRITE but not LOCAL_WRITE

### ibv_create_cq

```c
struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                              void *cq_context,
                              struct ibv_comp_channel *channel,
                              int comp_vector);
```

| Field | Description |
|-------|-------------|
| Function | Create a Completion Queue |
| `cqe` | CQ capacity (max entries), recommended >= QP's send_wr + recv_wr |
| `cq_context` | User-defined context pointer (for callbacks), usually NULL |
| `channel` | Completion notification channel (for event mode), NULL for polling mode |
| `comp_vector` | Completion vector (interrupt affinity), usually 0 |
| Free | `ibv_destroy_cq()` — must first destroy all QPs referencing this CQ |

### ibv_create_qp

```c
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                              struct ibv_qp_init_attr *qp_init_attr);
```

| Field | Description |
|-------|-------------|
| Function | Create a Queue Pair |
| Return | QP pointer (initial state is RESET), returns NULL on failure |
| Free | `ibv_destroy_qp()` |

**ibv_qp_init_attr key fields:**

```c
struct ibv_qp_init_attr qp_init_attr = {
    .send_cq = cq,                  /* Send Completion Queue */
    .recv_cq = cq,                  /* Recv Completion Queue (can be same as send_cq) */
    .qp_type = IBV_QPT_RC,          /* QP type: RC (Reliable Connection) */
    .cap = {
        .max_send_wr  = 128,        /* Max send WR count */
        .max_recv_wr  = 128,        /* Max recv WR count */
        .max_send_sge = 1,          /* Max SGE per send WR */
        .max_recv_sge = 1,          /* Max SGE per recv WR */
    },
};
```

---

## III. QP State Machine — Complete Parameter Templates

QP must transition in the order RESET -> INIT -> RTR -> RTS before it can send data.

### RESET -> INIT

```c
struct ibv_qp_attr attr = {
    .qp_state        = IBV_QPS_INIT,
    .pkey_index      = 0,           /* P_Key index, usually 0 */
    .port_num        = 1,           /* Port number, starts from 1 */
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                       IBV_ACCESS_REMOTE_WRITE |
                       IBV_ACCESS_REMOTE_READ,
};

int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
           IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

ibv_modify_qp(qp, &attr, mask);
```

### INIT -> RTR (Ready to Receive)

```c
struct ibv_qp_attr attr = {
    .qp_state              = IBV_QPS_RTR,
    .path_mtu              = IBV_MTU_1024,    /* Path MTU */
    .dest_qp_num           = remote_qp_num,   /* Remote QP number */
    .rq_psn                = remote_psn,       /* Remote starting PSN */
    .max_dest_rd_atomic    = 1,                /* Remote max concurrent RDMA Read */
    .min_rnr_timer         = 12,               /* RNR NAK retry timeout */
    .ah_attr = {
        .dlid          = remote_lid,           /* Remote LID (IB mode) */
        .sl            = 0,                    /* Service Level */
        .src_path_bits = 0,
        .port_num      = 1,                    /* Local port number */
        /* RoCE mode also needs: */
        .is_global     = 1,                    /* Enable GRH (required for RoCE) */
        .grh = {
            .dgid          = remote_gid,       /* Remote GID */
            .sgid_index    = local_gid_index,  /* Local GID index */
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

### RTR -> RTS (Ready to Send)

```c
struct ibv_qp_attr attr = {
    .qp_state      = IBV_QPS_RTS,
    .timeout       = 14,      /* Local ACK timeout: 4.096us * 2^14 ~ 67ms */
    .retry_cnt     = 7,       /* Retry count (0-7, 7=infinite retry) */
    .rnr_retry     = 7,       /* RNR retry count (7=infinite retry) */
    .sq_psn        = local_psn, /* Local starting PSN */
    .max_rd_atomic = 1,       /* Local max RDMA Read initiations */
};

int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
           IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

ibv_modify_qp(qp, &attr, mask);
```

---

## IV. Data Transfer API

### ibv_post_send

```c
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr);
```

| Field | Description |
|-------|-------------|
| Function | Submit a send work request |
| `wr` | Work request linked list (can submit multiple at once) |
| `bad_wr` | Points to the first failed WR on error |
| Return | 0 on success, non-zero on failure (EINVAL/ENOMEM) |
| **Important** | This is a pure userspace operation, does not go through the kernel! |

**Send operation template:**

```c
struct ibv_sge sge = {
    .addr   = (uint64_t)buf,      /* Data buffer address */
    .length = msg_size,            /* Data length */
    .lkey   = mr->lkey,           /* MR's lkey */
};

struct ibv_send_wr wr = {
    .wr_id      = 0,              /* User-defined ID (returned in WC) */
    .sg_list    = &sge,           /* SGE list */
    .num_sge    = 1,              /* Number of SGEs */
    .opcode     = IBV_WR_SEND,    /* Operation type */
    .send_flags = IBV_SEND_SIGNALED, /* Generate completion event */
    .next       = NULL,           /* Next WR (linked list) */
};
```

**RDMA Write operation template:**

```c
struct ibv_send_wr wr = {
    .wr_id      = 0,
    .sg_list    = &sge,
    .num_sge    = 1,
    .opcode     = IBV_WR_RDMA_WRITE,
    .send_flags = IBV_SEND_SIGNALED,
    .wr.rdma = {
        .remote_addr = remote_buf_addr,  /* Remote MR virtual address */
        .rkey        = remote_rkey,      /* Remote MR's rkey */
    },
};
```

**RDMA Read operation template:**

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

| Field | Description |
|-------|-------------|
| Function | Submit a receive work request (must be called before the remote side sends!) |
| **Important** | RC mode: each Send consumes one Recv WR; insufficient Recv WRs cause RNR |

**Recv operation template:**

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

| Field | Description |
|-------|-------------|
| Function | Poll the Completion Queue |
| `num_entries` | Max number of WCs to retrieve |
| Return | Positive=number of WCs retrieved, 0=CQ empty, negative=error |
| **Important** | This is a pure userspace operation, does not go through the kernel! |
| **Important** | Must check `wc.status == IBV_WC_SUCCESS` |

---

## V. Cleanup API (Release in Reverse Order)

Resources must be released in reverse order of creation, otherwise EBUSY is returned:

```c
/* Cleanup order: QP -> MR -> CQ -> PD -> Device -> Device List */
ibv_destroy_qp(qp);              /* 1. Destroy QP */
ibv_dereg_mr(mr);                /* 2. Deregister MR */
ibv_destroy_cq(cq);              /* 3. Destroy CQ */
ibv_dealloc_pd(pd);              /* 4. Deallocate PD */
ibv_close_device(ctx);           /* 5. Close device */
ibv_free_device_list(dev_list);  /* 6. Free device list */
```

**Common errors:**
- `ibv_dealloc_pd()` returns EBUSY — there are still QPs or MRs under the PD
- `ibv_destroy_cq()` returns EBUSY — there are still QPs referencing this CQ

---

## VI. Key Data Structures

### ibv_sge (Scatter/Gather Element)

```c
struct ibv_sge {
    uint64_t addr;    /* Buffer virtual address */
    uint32_t length;  /* Data length (bytes) */
    uint32_t lkey;    /* MR's lkey */
};
```

### ibv_send_wr (Send Work Request)

```c
struct ibv_send_wr {
    uint64_t             wr_id;       /* User-defined ID */
    struct ibv_send_wr  *next;        /* Linked list pointer */
    struct ibv_sge      *sg_list;     /* SGE array */
    int                  num_sge;     /* Number of SGEs */
    enum ibv_wr_opcode   opcode;      /* Operation code */
    int                  send_flags;  /* Flags */
    union {
        struct { uint64_t remote_addr; uint32_t rkey; } rdma;  /* RDMA R/W */
        struct { uint64_t remote_addr; uint64_t compare_add; uint64_t swap;
                 uint32_t rkey; } atomic;  /* Atomic */
    } wr;
    uint32_t imm_data;  /* Immediate data (IBV_WR_SEND_WITH_IMM) */
};
```

**Opcodes:**

| Opcode | Description | Requires Remote Recv WR |
|--------|-------------|------------------------|
| `IBV_WR_SEND` | Send | Yes |
| `IBV_WR_SEND_WITH_IMM` | Send + Immediate Data | Yes |
| `IBV_WR_RDMA_WRITE` | RDMA Write | No |
| `IBV_WR_RDMA_WRITE_WITH_IMM` | RDMA Write + Immediate Data | Yes |
| `IBV_WR_RDMA_READ` | RDMA Read | No |
| `IBV_WR_ATOMIC_CMP_AND_SWP` | Atomic CAS | No |
| `IBV_WR_ATOMIC_FETCH_AND_ADD` | Atomic FAA | No |

**send_flags flags:**

| Flag | Description |
|------|-------------|
| `IBV_SEND_SIGNALED` | Generate CQE when operation completes |
| `IBV_SEND_FENCE` | Wait for previous RDMA Read to complete |
| `IBV_SEND_INLINE` | Inline data into WQE (small message optimization, <= 64B) |

### ibv_recv_wr (Receive Work Request)

```c
struct ibv_recv_wr {
    uint64_t             wr_id;     /* User-defined ID */
    struct ibv_recv_wr  *next;      /* Linked list pointer */
    struct ibv_sge      *sg_list;   /* SGE array */
    int                  num_sge;   /* Number of SGEs */
};
```

### ibv_wc (Work Completion)

```c
struct ibv_wc {
    uint64_t              wr_id;       /* User-defined ID from corresponding WR */
    enum ibv_wc_status    status;      /* Completion status (0=success) */
    enum ibv_wc_opcode    opcode;      /* Operation type */
    uint32_t              vendor_err;  /* Vendor-specific error code */
    uint32_t              byte_len;    /* Bytes received (Recv only) */
    uint32_t              imm_data;    /* Immediate data (network byte order) */
    uint32_t              qp_num;      /* Local QP number */
    uint32_t              src_qp;      /* Remote QP number (UD/Recv only) */
    int                   wc_flags;    /* Flags (IBV_WC_WITH_IMM, etc.) */
};
```

---

## VII. WC Status Code Table

| Enum | Value | Meaning | Most Common Cause |
|------|-------|---------|-------------------|
| `IBV_WC_SUCCESS` | 0 | Success | — |
| `IBV_WC_LOC_LEN_ERR` | 1 | Local length error | Recv buffer too small |
| `IBV_WC_LOC_QP_OP_ERR` | 2 | Local QP operation error | QP not in RTS when sending |
| `IBV_WC_LOC_EEC_OP_ERR` | 3 | Local EEC operation error | (RD QP related, rare) |
| `IBV_WC_LOC_PROT_ERR` | 4 | Local protection error | MR permission mismatch |
| `IBV_WC_WR_FLUSH_ERR` | 5 | WR flushed | QP entered Error state |
| `IBV_WC_MW_BIND_ERR` | 6 | MW bind error | Memory Window operation failed |
| `IBV_WC_BAD_RESP_ERR` | 7 | Bad response error | Received unexpected response packet |
| `IBV_WC_LOC_ACCESS_ERR` | 8 | Local access error | lkey wrong or MR already deregistered |
| `IBV_WC_REM_INV_REQ_ERR` | 9 | Remote invalid request | Remote QP parameter error |
| `IBV_WC_REM_ACCESS_ERR` | 10 | Remote access error | Remote rkey/address/permission error |
| `IBV_WC_REM_OP_ERR` | 11 | Remote operation error | Remote internal error |
| `IBV_WC_RETRY_EXC_ERR` | 12 | Retry exceeded | Network unreachable / remote down |
| `IBV_WC_RNR_RETRY_EXC_ERR` | 13 | RNR retry exceeded | Remote did not Post Recv |
| `IBV_WC_LOC_RDD_VIOL_ERR` | 14 | Local RDD violation | (RD QP related) |
| `IBV_WC_REM_INV_RD_REQ_ERR` | 15 | Remote invalid RD request | (RD QP related) |
| `IBV_WC_REM_ABORT_ERR` | 16 | Remote abort | Remote QP was destroyed |
| `IBV_WC_INV_EECN_ERR` | 17 | Invalid EECN | (RD QP related) |
| `IBV_WC_INV_EEC_STATE_ERR` | 18 | Invalid EEC state | (RD QP related) |
| `IBV_WC_FATAL_ERR` | 19 | Fatal error | HCA hardware error |
| `IBV_WC_RESP_TIMEOUT_ERR` | 20 | Response timeout | Similar to RETRY_EXC |
| `IBV_WC_GENERAL_ERR` | 21 | General error | Other uncategorized error |

---

## VIII. Common Shell Commands

### Device Viewing

```bash
# List all RDMA devices
ibv_devices

# View device details
ibv_devinfo
ibv_devinfo -d mlx5_0         # Specify device
ibv_devinfo -d mlx5_0 -v      # Verbose mode

# View port status (like ifconfig for IB)
ibstat
ibstat mlx5_0

# View IB subnet management info
ibstatus
```

### Performance Testing (perftest toolset)

```bash
# Bandwidth test
# Server:
ib_write_bw
# Client:
ib_write_bw <server_ip>

# Latency test
# Server:
ib_write_lat
# Client:
ib_write_lat <server_ip>

# Other tests:
ib_read_bw / ib_read_lat     # RDMA Read
ib_send_bw / ib_send_lat     # Send/Recv
ib_atomic_bw / ib_atomic_lat # Atomic

# Common parameters:
#   -d mlx5_0    Specify device
#   -s 4096      Message size
#   -n 1000      Number of iterations
#   -F           Don't use events (polling mode)
#   --report_gbits  Report in Gbit/s
```

### Diagnostic Tools

```bash
# View RDMA kernel modules
lsmod | grep -E "rdma|ib_|mlx"

# View rdma-core version
dpkg -l | grep rdma-core     # Debian/Ubuntu
rpm -qa | grep rdma-core     # CentOS/RHEL

# View locked memory limit (needed for RDMA MR registration)
ulimit -l

# View /dev/infiniband/ device files
ls -la /dev/infiniband/

# View GID table (for RoCE mode)
cat /sys/class/infiniband/mlx5_0/ports/1/gids/0
cat /sys/class/infiniband/mlx5_0/ports/1/gid_attrs/types/0

# Use rdma tool (iproute2)
rdma dev show
rdma link show
rdma res show qp
rdma stat show
```

---

## Study Recommendations

1. **Bookmark this page** — Refer to API parameters and error codes anytime while programming
2. **Start with hello_rdma.c** — This is the minimal runnable template; modify it for experiments
3. **Run env_check.sh first** — Make sure the environment is correct before writing code
4. **Look up error_cheatsheet when encountering errors** — It will tell you the cause and fix

---

*This is the last chapter of the RDMA 101 tutorial. Happy RDMA programming!*
