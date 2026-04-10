# Chapter 6: RDMA Connection Establishment

## Learning Objectives

| Objective | Description |
|-----------|-------------|
| Master manual TCP exchange connection | Exchange QPN/LID/GID via Socket, manually complete QP state transitions |
| Master RDMA CM connection | Use high-level connection management API for automatic address resolution and QP management |
| Understand UD transport mode | Connectionless datagram mode, understand the Address Handle concept |
| Master RC/UC/UD differences | Choose the appropriate transport mode based on the scenario |
| Understand IB/RoCE dual-mode adaptation | Same program automatically adapts to InfiniBand and RoCE networks |

---

## 6.1 Why Is Connection Establishment Important?

RDMA's high performance comes from **bypassing the kernel in the data path**, but before data transfer, both ends must first complete a handshake on the **control path**:

```
                  Control Path (Slow Path)                  Data Path (Fast Path)
  ┌─────────────────────────────────────┐    ┌──────────────────────────┐
  │  Exchange QPN, LID/GID, PSN, rkey...│    │  RDMA Send/Write/Read    │
  │  QP state: RESET → INIT → RTR → RTS│    │  Zero-copy, kernel bypass│
  │  (One-time cost, via TCP/RDMA CM)   │    │  (Per-operation, ns latency)│
  └─────────────────────────────────────┘    └──────────────────────────┘
```

This chapter introduces three connection establishment methods, covering all scenarios from low-level to high-level, from connected to connectionless.

---

## 6.2 Comparison of Three Connection Methods

| Feature | Manual TCP Exchange | RDMA CM | UD (Connectionless) |
|---------|--------------------|---------|--------------------|
| **Complexity** | Medium | Low | High |
| **Flexibility** | Highest | Medium | High |
| **Applicable Transport** | RC / UC | RC / UC | UD |
| **Required Library** | libibverbs | librdmacm | libibverbs |
| **QP Management** | Manual state transition | CM auto-managed | Manual (no dest_qp) |
| **Address Resolution** | User handles manually | CM auto-resolves | User creates AH |
| **Multicast Support** | Not supported | Supported | Natively supported |
| **Typical Scenario** | High-perf storage, learning | General applications | High fan-out, multicast |

### Selection Guide

```
Need reliable transport (ordering, retransmission)?
     │
     ├─ Yes → RC mode (Reliable Connected)
     │         │
     │         ├─ Want maximum flexibility? → Manual TCP exchange (01-manual-connect)
     │         └─ Want simplicity?          → RDMA CM (02-rdma-cm)
     │
     └─ No → Need one-to-many communication?
              │
              ├─ Yes → UD mode (Unreliable Datagram)
              │         → Use Address Handle (03-ud-mode)
              │
              └─ No → UC mode (Unreliable Connected)
                       → Manual TCP exchange, but no reliability guarantee
```

---

## 6.3 RC / UC / UD Transport Mode Details

### RC (Reliable Connected)

- **One-to-one connection**: Each pair of communication endpoints requires an independent QP pair
- **Reliable transport**: Hardware guarantees ordering, retransmission, and deduplication
- **Supports all operations**: Send/Recv, RDMA Write, RDMA Read, Atomic
- **State transition**: RESET → INIT → RTR (requires dest_qp_num) → RTS

```c
/* RC's RTR requires specifying the remote QP number */
attr.dest_qp_num = remote->qp_num;   // ← RC specific
attr.rq_psn      = remote->psn;
```

### UC (Unreliable Connected)

- **One-to-one connection**: Same as RC, requires QP pair
- **Unreliable**: No retransmission, no acknowledgment, packet loss handled by upper layer
- **Supported operations**: Send/Recv, RDMA Write (RDMA Read and Atomic not supported)
- **Applicable scenario**: Bulk transfer that can tolerate packet loss

### UD (Unreliable Datagram)

- **Connectionless**: A single QP can send to multiple targets
- **Unreliable**: Same as UC, no retransmission
- **Only supports Send/Recv**: RDMA Write/Read/Atomic not supported
- **Message size limit**: Maximum MTU size (typically 4096 bytes)
- **GRH header**: Receive buffer needs extra 40 bytes for GRH (Global Route Header)

```c
/* Key differences of UD */

/* 1. QP type */
qp_init_attr.qp_type = IBV_QPT_UD;    // Not IBV_QPT_RC

/* 2. RTR does not need dest_qp_num (connectionless!) */
attr.qp_state = IBV_QPS_RTR;
// No need for attr.dest_qp_num, attr.rq_psn, etc.

/* 3. Specify Address Handle when sending */
wr.wr.ud.ah         = ah;              // Address handle (points to target)
wr.wr.ud.remote_qpn = remote_qpn;     // Target QP number
wr.wr.ud.remote_qkey = 0x11111111;     // Q_Key matching

/* 4. Receive buffer needs extra 40 bytes for GRH */
char recv_buf[40 + MSG_SIZE];          // First 40 bytes are GRH
char *payload = recv_buf + 40;         // Actual data starts at offset 40
```

---

## 6.4 Address Handle Concept

The core concept of UD mode is the **Address Handle (AH)**, which encapsulates routing information to reach the target:

```
┌─────────────────────────┐
│    Address Handle (AH)   │
├─────────────────────────┤
│ dlid     = remote LID    │  ← IB mode
│ is_global = 1            │  ← Required for RoCE
│ grh.dgid = remote GID    │  ← RoCE mode
│ sl       = Service Level │
│ port_num = local port    │
└─────────────────────────┘
```

**AH vs QP connection differences**:
- RC/UC: Routing information is bound in QP's RTR attributes (one-to-one)
- UD: Routing information is encapsulated in AH, specified at send time (one-to-many)

```c
/* Create Address Handle */
struct ibv_ah_attr ah_attr = {
    .dlid     = remote_lid,    /* IB: target LID */
    .sl       = 0,
    .port_num = 1,
    .is_global = is_roce,      /* RoCE: must be set to 1 */
};
if (is_roce) {
    ah_attr.grh.dgid       = remote_gid;
    ah_attr.grh.sgid_index = local_gid_index;
    ah_attr.grh.hop_limit  = 64;
}
struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
```

---

## 6.5 Example Programs

### 01-manual-connect: Manual TCP Exchange Connection

Classic dual-process RDMA connection program demonstrating the complete connection flow:

```bash
cd 01-manual-connect && make

# Terminal 1 (Server)
./manual_connect -s 7471

# Terminal 2 (Client)
./manual_connect -c 192.168.1.100 7471
```

**Flow**:
1. Create RDMA resources (PD, CQ, QP, MR)
2. Auto-detect IB/RoCE transport type
3. Fill local endpoint info (QPN, LID, GID, PSN)
4. Exchange endpoint info via TCP Socket
5. `qp_full_connect()` completes RESET → INIT → RTR → RTS
6. Bidirectional Send/Recv to verify connection

### 02-rdma-cm: RDMA CM Connection Management

Uses `librdmacm` high-level API for automatic address resolution and QP management:

```bash
cd 02-rdma-cm && make

# Terminal 1 (Server)
./rdma_cm_example -s 7471

# Terminal 2 (Client)
./rdma_cm_example -c 127.0.0.1 7471
```

**CM Event Flow**:

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
  
  ========== Connection established, ready to communicate ==========
```

### 03-ud-mode: UD Connectionless Mode

Single-machine loopback demonstration of UD mode core concepts:

```bash
cd 03-ud-mode && make

./ud_loopback
```

**Key differences from RC**:
- QP type: `IBV_QPT_UD` (not `IBV_QPT_RC`)
- RTR transition is simpler (no dest_qp_num needed)
- Sending requires an Address Handle
- Receive buffer has extra 40 bytes for GRH header

---

## 6.6 Connection Establishment Flow Details

### Complete Flow of Manual TCP Exchange

```
  Server                    TCP             Client
  ──────                   ─────            ──────
  1. Create RDMA resources                  1. Create RDMA resources
     (PD, CQ, QP, MR)                         (PD, CQ, QP, MR)
  
  2. Fill local endpoint:                   2. Fill local endpoint:
     {QPN, LID, GID, PSN}                      {QPN, LID, GID, PSN}
  
  3. TCP listen            ◄── TCP conn ──►  3. TCP connect to server
  
  4. Exchange endpoint info ◄── Bidirectional ──► 4. Exchange endpoint info
     Received remote_ep                        Received remote_ep
  
  5. qp_full_connect()                      5. qp_full_connect()
     RESET → INIT                              RESET → INIT
     INIT  → RTR                               INIT  → RTR
     RTR   → RTS                               RTR   → RTS
  
  6. Send/Recv comm        ◄── RDMA ──►      6. Send/Recv comm
```

### Complete Flow of RDMA CM

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
                                            Create QP (on cm_id)
                                            rdma_connect()
  → CONNECT_REQUEST
  Create QP (on new cm_id)
  rdma_accept()
  → ESTABLISHED                             → ESTABLISHED
  
  ============ QP is now in RTS state ============
```

---

## 6.7 FAQ

### Q1: Why not just use RDMA CM?

RDMA CM is simpler, but the manual approach has the following advantages:
- **Lower-level control**: Can precisely adjust every QP attribute
- **No librdmacm dependency**: May not be available in some embedded environments
- **Performance tuning**: Can optimize the exchange protocol for large-scale connections
- **Learning value**: Understanding the underlying principles is very important for debugging

### Q2: What is the 40-byte GRH in UD mode?

GRH (Global Route Header) contains routing information such as source/destination GID. In UD mode, the hardware prepends a 40-byte GRH to every received message:

```
Receive buffer layout:
┌──────────────────┬──────────────────────┐
│  GRH (40 bytes)  │  Actual data (payload)│
└──────────────────┴──────────────────────┘
  ↑                   ↑
  Auto-filled by NIC   Application data starts here
```

### Q3: Does the order of both ends matter during RC connection?

For the manual TCP exchange approach, the QP state transitions on both ends can be executed asynchronously, but the following must be ensured:
- **RTR requires remote info**: Must have obtained the remote QPN/LID/GID before transitioning to RTR
- **RTS can be done first**: One end reaching RTS first does not affect the other end

### Q4: What are the differences between IB and RoCE connection establishment?

| Step | InfiniBand | RoCE |
|------|-----------|------|
| Addressing | Uses LID (16-bit) | Uses GID (128-bit, IPv6 format) |
| AH attributes | `is_global = 0`, set `dlid` | `is_global = 1`, set `grh.dgid` |
| GID index | Usually not important | Must set `sgid_index` correctly |
| Subnet | SM assigns LID | IP stack configuration |

---

## 6.8 Build and Run

### Prerequisites

```bash
# Install development libraries
sudo apt-get install libibverbs-dev librdmacm-dev

# Verify device availability
ibv_devices
ibv_devinfo
```

### Build All Examples

```bash
# Build common library first
make -C ../../common

# Build all examples in this chapter
cd 01-manual-connect && make
cd ../02-rdma-cm && make
cd ../03-ud-mode && make
```

---

## 6.9 Exercises

1. **Conceptual**: Explain what scenarios RC, UC, and UD transport modes are each suitable for.
2. **Programming**: Modify `manual_connect.c` to attach Immediate Data to the Send message.
3. **Analysis**: What happens if the RDMA CM server does not call `rdma_ack_cm_event()`?
4. **Practical**: Implement a simple multicast demo using UD mode.
5. **Comparison**: Between manual TCP exchange and RDMA CM connection, which is more suitable for large-scale clusters? Why?

---

## Next Steps

After completing this chapter, you have mastered all RDMA connection establishment methods. Next:

- [ch07-engineering](../ch07-engineering/README.md) - Engineering Practice and Performance Tuning
- [ch08-advanced](../ch08-advanced/README.md) - Advanced Topics
