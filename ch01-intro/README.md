# Chapter 1: RDMA Fundamentals

## Learning Objectives
- Understand the problems of traditional TCP/IP communication
- Master the core advantages of RDMA
- Learn about three RDMA protocols
- Familiarize with basic RDMA programming terminology

---

## 1.1 Problems of Traditional TCP/IP Communication

### Computer Network Communication Basics

Before understanding RDMA, let's review how traditional network communication works:

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│                     (Your Program)                               │
└─────────────────────────────────────────────────────────────────┘
                              │ write()/send()
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Transport Layer (TCP/UDP)                 │
│                   Kernel Protocol Stack Processing               │
└─────────────────────────────────────────────────────────────────┘
                              │ 
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        NIC Driver                                │
│                   (Copy to DMA Buffer)                           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                          NIC                                     │
│                   (Send to Network)                              │
└─────────────────────────────────────────────────────────────────┘
```

### Data Copies in Traditional Communication

```
User Space ──copy──▶ Kernel Buffer ──copy──▶ NIC DMA Buffer ──send──▶ Network
     ◀──copy── ◀──copy── ◀────────────────────────────────────────
                              receive
```

**How many data copies does the data go through?**
- **Send**: User Space → Kernel Space (1 copy)
- **Receive**: Kernel Space → User Space (1 copy)
- **Total**: At least **2** copies

### Sources of Traditional Communication Latency

| Operation | Typical Latency | Description |
|------|----------|------|
| System Call | 1-2 μs | Context switch from user mode to kernel mode |
| Data Copy | 1-2 μs | Memory copy overhead |
| TCP Processing | 5-10 μs | Protocol stack processing |
| Interrupt Handling | 1-5 μs | Hardware interrupt |
| Network Transmission | 100s-1000s μs | Depends on physical distance and bandwidth |

### Summary of Traditional Communication Problems

1. **CPU Involvement** - Every communication requires CPU intervention
2. **Data Copies** - Data is copied multiple times between different memory regions
3. **Kernel Bottleneck** - All data must go through the OS kernel
4. **High Latency** - Multiple context switches add overhead

---

## 1.2 Core Advantages of RDMA

### What is RDMA?

**Remote Direct Memory Access (RDMA)** - Direct remote memory access

Allows one computer to directly access another computer's memory without involving the remote CPU or operating system.

### Four Core Advantages

| Advantage | Description |
|------|------|
| **Zero Copy** | Data is transferred directly from one memory region to another without CPU involvement |
| **Kernel Bypass** | Bypasses the OS kernel, communicating directly with the NIC |
| **Low Latency** | End-to-end latency can be as low as 1-2 μs |
| **Low CPU Usage** | Communication does not consume CPU resources |

### RDMA vs TCP/IP Latency Comparison

```
TCP/IP:  10-100 μs (includes multiple copies and kernel processing)
RDMA:   1-5 μs   (direct memory access)

Performance Improvement: 10-20x
```

### RDMA Data Transfer

```
┌─────────────────────────────────────────────────────────────────┐
│                    Traditional TCP/IP Communication              │
│                                                                 │
│  Sender                              Receiver                    │
│  ┌─────┐                        ┌─────┐                        │
│  │CPU  │ copy                   │CPU  │ copy                  │
│  └──┬──┘   │                 ◀──└┬──┘   │                     │
│     │      ▼                        │      ▼                    │
│  ┌──▼───────────────────────────────▼──┐                        │
│  │         Kernel Protocol Stack        │                        │
│  └──────────────────────────────────────┘                        │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    RDMA Communication                            │
│                                                                 │
│  Sender                              Receiver                    │
│  ┌─────┐                        ┌─────┐                        │
│  │CPU  │ ──RDMA Write────────▶│Memory│                        │
│  └─────┘   (No CPU involved)    └─────┘                        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 1.3 Three RDMA Protocols

### 1. InfiniBand (IB)

- **Features**: Native RDMA protocol, designed for high-speed networking
- **Bandwidth**: HDR 400Gbps, NDR 800Gbps
- **Requires**: Dedicated IB switches
- **Pros**: Highest performance
- **Cons**: Requires dedicated hardware

### 2. RoCE (RDMA over Converged Ethernet)

Runs RDMA over Ethernet, currently the most popular solution.

**Versions**:
| Version | Description |
|------|------|
| RoCE v1 | Requires lossless network (Data Center Bridging) |
| RoCE v2 | Supports routing, based on UDP (port 4791) |

**Advantages**:
- Compatible with existing Ethernet equipment
- Can use standard Ethernet switches
- Supports cross-router communication

### 3. iWARP (Internet Wide Area RDMA Protocol)

- **Features**: TCP/IP-based RDMA
- **Advantages**: Can traverse firewalls and NAT
- **Performance**: Slightly lower than IB and RoCE
- **Use Case**: Wide area networks

### Protocol Comparison

| Feature | InfiniBand | RoCE v2 | iWARP |
|------|------------|---------|-------|
| Performance | Highest | High | Medium |
| Compatibility | Dedicated Hardware | Ethernet | TCP/IP |
| Routing Support | Yes | Yes | Yes |
| Firewall Traversal | No | No | Yes |
| Typical Bandwidth | 400/800 Gbps | 100/200 Gbps | 40/100 Gbps |

### Recommendation Guide

| Scenario | Recommended Protocol |
|------|----------|
| High Performance Computing (HPC) | InfiniBand |
| Data Center | RoCE v2 |
| Cross WAN Required | iWARP |

---

## 1.4 RDMA Core Terminology

### Queue Pair (QP)

The basic unit of RDMA communication, consisting of two parts:
- **SQ (Send Queue)** - Send Queue
- **RQ (Receive Queue)** - Receive Queue

```
┌─────────────────────────────────────┐
│            Queue Pair                │
│  ┌───────────┐   ┌───────────┐      │
│  │    SQ     │   │    RQ     │      │
│  │(Send Queue)│   │(Recv Queue)│      │
│  └───────────┘   └───────────┘      │
└─────────────────────────────────────┘
```

### Memory Region (MR)

- Virtual memory region registered with the RDMA device
- Contains: **Virtual Address**, **Physical Address**, **Access Permissions**

```
User Virtual Address ──register──▶ RDMA Device ──translate──▶ Physical Address
```

### Protection Domain (PD)

- Used to isolate resources of different applications
- QP and MR must belong to the same PD

### Completion Queue (CQ)

- Stores communication operation completion notifications
- Contains operation status, error information

### Work Request (WR)

- Operation request submitted to a queue
- Describes the operation to perform and its parameters

### Work Completion (WC)

- Status returned after an operation completes
- Contains: status, operation type, ImmData, etc.

---

## 1.5 RDMA Operation Types

| Operation | Description | Requires Remote Cooperation | Typical Use |
|------|------|--------------|----------|
| **Send/Recv** | Traditional message passing | Yes | Message notification |
| **RDMA Write** | Write directly to remote memory | No | Data push |
| **RDMA Read** | Read directly from remote memory | No | Data pull |
| **Atomic** | Atomic operations (CAS/FAA) | No | Counters, locks |

### Operation Timing Diagram

```
Send/Recv:
  A                          B
  │───post_recv()───────────▶│
  │                          │
  │───post_send()───────────▶│
  │◀──────completion─────────│
  │                          │
  │◀───post_recv()──────────│

RDMA Write:
  A                          B
  │───RDMA Write───────────▶│ (Directly writes to B's memory)
  │◀──────completion─────────│

RDMA Read:
  A                          B
  │───RDMA Read─────────────▶│
  │◀─completion (data at A)──│ (B is unaware)
```

---

## 1.6 Programming Framework

### libibverbs (Verbs API)

- Low-level API for RDMA programming
- Vendor-agnostic interface
- Native Linux support

```c
#include <infiniband/verbs.h>
```

### RDMA CM

- Connection-based communication management API
- Built on top of libibverbs
- Provides an easier-to-use interface

```c
#include <rdma/rdma_cma.h>
```

### Comparison

| Feature | Verbs API | RDMA CM |
|------|------------|---------|
| Complexity | High | Low |
| Control | Full | Limited |
| Use Case | High Performance | Simplified Development |

---

## 1.7 Hardware Components

### RDMA NICs

Common RDMA NICs:
- **Mellanox (NVIDIA)** - ConnectX Series
- **Intel** - Omni-Path Series
- **Broadcom** - NetXtreme Series

### Network Topology

```
┌──────────┐         ┌──────────┐
│  Server  │────────▶│  Server  │
│  (RDMA)  │◀────────│  (RDMA)  │
└──────────┘         └──────────┘
       │                   │
       └─────────┬─────────┘
                 │
          ┌──────▼──────┐
          │  Switch     │
          │ (RDMA Capable)│
          └─────────────┘
```

---

## Exercises

1. In traditional TCP/IP communication, how many data copies does the data go through?
2. What roles does the CPU play in traditional network communication?
3. What are the core advantages of RDMA compared to TCP/IP?
4. What are the differences between the three RDMA protocols? What scenarios are they suited for?
5. What two parts make up a QP?
6. Why does RDMA Write not require remote cooperation?

---

## Next Step

Proceed to the next chapter: [Chapter 2: Network Technology Layer](../ch02-network-layer/README.md)
