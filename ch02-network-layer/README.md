# Chapter 2: Network Layer — InfiniBand / RoCE / iWARP

## Learning Objectives

- Deeply understand the technical details of three RDMA transport layer protocols
- Master the addressing differences of IB / RoCE / iWARP (LID vs GID)
- Learn to use Verbs API to detect and query the underlying transport type
- Understand how Verbs supports all transport protocols under a unified abstraction layer

---

## 2.1 Why Understand the Network Layer?

In Chapter 1, we introduced three RDMA protocols: InfiniBand, RoCE, and iWARP.
Although the Verbs API provides a unified programming interface, in actual programming, **transport layer differences still affect your code**.

```
┌─────────────────────────────────────────────────────────────────┐
│                    Your RDMA Application                         │
├─────────────────────────────────────────────────────────────────┤
│                    libibverbs (Verbs API)                       │
│                    ← Unified Abstraction Layer →                 │
├───────────────┬───────────────┬─────────────────────────────────┤
│  InfiniBand   │    RoCE v2    │           iWARP                 │
│  (LID Addr)   │  (GID Addr)   │        (TCP Stream)             │
├───────────────┼───────────────┼─────────────────────────────────┤
│  IB Dedicated  │   Ethernet    │          TCP/IP                 │
│  Network       │               │                                 │
└───────────────┴───────────────┴─────────────────────────────────┘
```

### Key Differences

| Difference | InfiniBand | RoCE v1/v2 | iWARP |
|--------|------------|------------|-------|
| **link_layer** | `IBV_LINK_LAYER_INFINIBAND` | `IBV_LINK_LAYER_ETHERNET` | `IBV_LINK_LAYER_ETHERNET` |
| **node_type** | `IBV_NODE_CA` | `IBV_NODE_CA` | `IBV_NODE_RNIC` |
| **Addressing** | LID (16-bit Local ID) | GID (128-bit Global ID) | IP Address |
| **ah_attr** | `dlid` field | `is_global=1` + `grh.dgid` | Typically uses RDMA CM |
| **Subnet Management** | Requires SM (Subnet Manager) | Not required | Not required |
| **Underlying Network** | IB Dedicated Switches | Standard Ethernet Switches | Standard TCP/IP Network |
| **Transport Layer** | IB Native Transport | UDP (Port 4791) | TCP |

---

## 2.2 InfiniBand In-Depth

### 2.2.1 IB Network Architecture

```
┌──────────┐    IB Link    ┌──────────┐    IB Link    ┌──────────┐
│  HCA      │◀────────────▶│  IB       │◀────────────▶│  HCA      │
│  (NIC)     │              │  Switch   │              │  (NIC)     │
│  LID=1    │              │           │              │  LID=2    │
└──────────┘              └──────────┘              └──────────┘
                                │
                          ┌─────▼─────┐
                          │  Subnet   │
                          │  Manager  │
                          │  (SM)     │
                          └───────────┘
```

### 2.2.2 LID Addressing

IB uses a 16-bit **LID (Local Identifier)** for intra-subnet addressing:

- LID is assigned by the **Subnet Manager (SM)**
- Each port is assigned at least one LID
- LID range: 1 ~ 65535 (0 is reserved)
- Cross-subnet communication requires GRH (Global Route Header)

```c
/* ah_attr setup for IB mode */
struct ibv_ah_attr ah_attr = {
    .dlid       = remote->lid,      /* Destination port LID */
    .sl         = 0,                /* Service Level */
    .port_num   = port,
    .is_global  = 0,                /* GRH not needed within IB subnet */
};
```

### 2.2.3 Port Attributes

IB ports provide rich attribute information:

| Field | Description |
|------|------|
| `state` | Port state: DOWN / INIT / ARMED / ACTIVE |
| `lid` | Local port LID |
| `sm_lid` | Subnet Manager LID |
| `active_mtu` | Active MTU (256/512/1024/2048/4096) |
| `active_speed` | Link speed |
| `active_width` | Link width (1x/4x/8x/12x) |
| `link_layer` | Link layer type |
| `gid_tbl_len` | GID table length |
| `pkey_tbl_len` | P_Key table length |

### 2.2.4 Practice Files

| File | Description |
|------|------|
| `01-infiniband/ib_env_check.sh` | IB environment check script |
| `01-infiniband/ib_port_detail.c` | IB port attribute full query program |

---

## 2.3 RoCE In-Depth

### 2.3.1 RoCE Protocol Stack

```
RoCE v1:                           RoCE v2:
┌─────────────┐                    ┌─────────────┐
│  IB Transport │                    │  IB Transport │
├─────────────┤                    ├─────────────┤
│  IB Network   │                    │  UDP:4791   │
├─────────────┤                    ├─────────────┤
│  Ethernet     │                    │  IP (v4/v6) │
└─────────────┘                    ├─────────────┤
                                   │  Ethernet     │
                                   └─────────────┘
```

**RoCE v2 is the current mainstream**, because it supports IP routing and can traverse Layer 3 networks.

### 2.3.2 GID Addressing

RoCE does not use LID; instead, it uses **128-bit GID (Global Identifier)**:

- GID format is the same as IPv6 addresses
- RoCE v2 GIDs are typically based on IPv4-mapped IPv6 or real IPv6
- Each port has a **GID table** containing multiple GID entries

```
GID Table Example:
  Index 0: fe80::xxxx:xxxx:xxxx:xxxx    (link-local, RoCE v1)
  Index 1: ::ffff:10.0.0.1              (IPv4-mapped, RoCE v2)
  Index 2: fe80::xxxx:xxxx:xxxx:xxxx    (link-local, RoCE v1)
  Index 3: ::ffff:10.0.0.1              (IPv4-mapped, RoCE v2)
```

**GID Index Selection Rules:**
- RoCE v1: Use `gid_index = 0` (link-local GID)
- RoCE v2: Typically use `gid_index = 1` or `gid_index = 3` (IPv4-mapped GID)

```c
/* ah_attr setup for RoCE mode */
struct ibv_ah_attr ah_attr = {
    .dlid       = 0,                /* RoCE does not use LID */
    .port_num   = port,
    .is_global  = 1,                /* GRH must be enabled */
    .grh = {
        .dgid       = remote->gid,  /* Destination port GID */
        .sgid_index = gid_index,     /* Local GID table index */
        .hop_limit  = 64,
        .traffic_class = 0,
    },
};
```

### 2.3.3 RoCE Network Requirements

RoCE has network quality requirements, because UDP does not retransmit like TCP:

| Mechanism | Description |
|------|------|
| **PFC (Priority Flow Control)** | Priority-based flow control to prevent packet loss |
| **ECN (Explicit Congestion Notification)** | Explicit congestion notification to prevent queue overflow |
| **DSCP** | Differentiated Services Code Point, QoS marking |

### 2.3.4 Practice Files

| File | Description |
|------|------|
| `02-roce/roce_env_check.sh` | RoCE environment check script |
| `02-roce/roce_gid_query.c` | GID table enumeration and analysis program |

---

## 2.4 iWARP In-Depth

### 2.4.1 iWARP Protocol Stack

```
┌─────────────┐
│  RDMA Layer   │  (RDMAP - Remote Direct Memory Access Protocol)
├─────────────┤
│  DDP         │  (Direct Data Placement)
├─────────────┤
│  MPA         │  (Marker PDU Aligned Framing)
├─────────────┤
│  TCP         │
├─────────────┤
│  IP          │
├─────────────┤
│  Ethernet     │
└─────────────┘
```

### 2.4.2 iWARP Characteristics

- **TCP-based**: Natively supports routing, NAT, and firewall traversal
- **No lossless network required**: TCP has built-in retransmission
- **Slightly lower performance**: TCP processing overhead > UDP
- **node_type**: `IBV_NODE_RNIC` (different from IB/RoCE's `IBV_NODE_CA`)

### 2.4.3 Common iWARP Hardware

| Vendor | Series | Driver |
|------|------|------|
| Chelsio | T5/T6 | `iw_cxgb4` |
| Intel | X722/E810 | `i40iw` / `irdma` |

### 2.4.4 iWARP Programming Notes

iWARP typically uses **RDMA CM** to establish connections (because the underlying layer is TCP):

```c
/* Typical connection method for iWARP */
rdma_create_id()     /* Create CM ID */
rdma_resolve_addr()  /* Resolve address */
rdma_resolve_route() /* Resolve route */
rdma_connect()       /* Establish connection (underlying TCP handshake) */
```

### 2.4.5 Practice Files

| File | Description |
|------|------|
| `03-iwarp/iwarp_env_check.sh` | iWARP environment check script |
| `03-iwarp/iwarp_query.c` | iWARP device query program |

---

## 2.5 Verbs Unified Abstraction

### 2.5.1 One Codebase, Three Transports

The Verbs API design philosophy is: **application-layer code should be as transport-agnostic as possible**.

```c
/* This code works on IB / RoCE / iWARP */
dev_list = ibv_get_device_list(&num_devices);   /* Enumerate all devices */
ctx = ibv_open_device(dev_list[0]);             /* Open device */
ibv_query_device(ctx, &dev_attr);               /* Query device capabilities */
ibv_query_port(ctx, 1, &port_attr);             /* Query port attributes */

/* Only need to differentiate transport type when setting ah_attr */
if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
    /* RoCE: Use GID addressing */
    ah_attr.is_global = 1;
    ah_attr.grh.dgid = remote_gid;
} else {
    /* IB: Use LID addressing */
    ah_attr.dlid = remote_lid;
}
```

### 2.5.2 Transport Type Detection Methods

```c
/* Method 1: Detect via link_layer */
struct ibv_port_attr port_attr;
ibv_query_port(ctx, port, &port_attr);
if (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET)
    /* RoCE or iWARP */
if (port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND)
    /* InfiniBand */

/* Method 2: Detect via node_type */
if (dev->node_type == IBV_NODE_CA)
    /* IB or RoCE (Channel Adapter) */
if (dev->node_type == IBV_NODE_RNIC)
    /* iWARP (RDMA NIC) */

/* Method 3: Combine both */
enum rdma_transport detect_transport(ctx, port) {
    if (node_type == IBV_NODE_RNIC)  return IWARP;
    if (link_layer == ETHERNET)       return ROCE;
    return IB;
}
```

### 2.5.3 /dev/infiniband/ Device Files

```bash
$ ls /dev/infiniband/
rdma_cm        # RDMA CM character device
uverbs0        # Verbs character device for the first RDMA device
uverbs1        # Second RDMA device (if available)

$ ls /sys/class/infiniband/
rxe_0          # SoftRoCE device
mlx5_0         # Mellanox CX-5 device
```

### 2.5.4 Practice Files

| File | Description |
|------|------|
| `04-verbs-abstraction/verbs_any_transport.c` | Unified abstraction demo program |

---

## 2.6 Practice in SoftRoCE Environment

If you are using an Alibaba Cloud ECS + SoftRoCE environment (as set up in ch00), then:

```
Device Name:  rxe_0
node_type:    IBV_NODE_CA        (same as physical RoCE)
link_layer:   IBV_LINK_LAYER_ETHERNET
transport:    RDMA_TRANSPORT_ROCE

GID Table:
  Index 0: fe80::xxxx:xxxx:xxxx:xxxx    (link-local)
  Index 1: ::ffff:x.x.x.x              (IPv4-mapped, use this for RoCE v2)
```

**Recommended GID Index:**
- SoftRoCE: `gid_index = 1` (corresponds to IPv4 address)
- Physical RoCE: Check the GID table and select the index corresponding to RoCE v2

---

## Chapter File Structure

```
ch02-network-layer/
├── README.md                              ← This file
├── 01-infiniband/
│   ├── ib_env_check.sh                    ← IB environment check script
│   ├── ib_port_detail.c                   ← IB port detailed query
│   └── Makefile
├── 02-roce/
│   ├── roce_env_check.sh                  ← RoCE environment check script
│   ├── roce_gid_query.c                   ← GID table enumeration analysis
│   └── Makefile
├── 03-iwarp/
│   ├── iwarp_env_check.sh                 ← iWARP environment check script
│   ├── iwarp_query.c                      ← iWARP device query
│   └── Makefile
└── 04-verbs-abstraction/
    ├── verbs_any_transport.c              ← Unified abstraction demo
    └── Makefile
```

---

## Recommended Learning Order

1. **Read this README**: Understand the differences between three protocols
2. **Run environment check scripts**: Confirm your environment type
3. **Compile and run C programs**: Hands-on practice with various queries
4. **Focus on 02-roce/**: Most cloud environments use RoCE
5. **Understand 04-verbs-abstraction/**: Master the unified programming model

---

## Exercises

1. What addressing method does InfiniBand use? Who assigns the addresses?
2. What is the biggest improvement of RoCE v2 over v1?
3. In a RoCE environment, what value must `ah_attr.is_global` be set to? Why?
4. How can you distinguish IB and RoCE devices using the Verbs API?
5. What is iWARP's `node_type`? How does it differ from IB/RoCE?
6. Why does RoCE need PFC while iWARP does not?
7. In a SoftRoCE environment, which GID index should you choose?

---

## Next Step

Proceed to the next chapter: [Chapter 3: Verbs API Getting Started](../ch03-verbs-api/README.md)
