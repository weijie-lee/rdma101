# Chapter 4: RDMA Communication Patterns in Practice

## Learning Objectives

| Objective | Description |
|-----------|-------------|
| Master four communication patterns | Send/Recv, Write, Read, Atomic |
| Understand applicable scenarios for each pattern | Choose the appropriate pattern based on requirements |
| Be able to write complete C/S programs | Exchange connection info via Socket |
| Understand immediate data usage | Master the use of ImmData |
| Master RDMA CM programming | Understand the advanced connection management API |
| Understand multi-threaded RDMA | Master thread-safe programming essentials |
| Master performance optimization techniques | Learn to improve RDMA performance |

---

## 4.1 Comparison of Four Communication Patterns

| Pattern | Peer Cooperation | Typical Usage | Performance | Complexity |
|---------|-----------------|---------------|-------------|------------|
| **Send/Recv** | Required | Message passing | High | Low |
| **RDMA Write** | Not required | Data push | Highest | Medium |
| **RDMA Read** | Not required | Data pull | High | Medium |
| **Atomic** | Not required | Counters, locks | Medium | High |

### Selection Guide

```
Does the peer need to know data has arrived?
     │
     ├─ Yes → Send/Recv
     │
     └─ No → Active push or passive pull?
              │
              ├─ Push → RDMA Write
              └─ Pull → RDMA Read

Need atomic operations?
     │
     └─ Yes → Atomic (CAS/FAA)
```

---

## 4.2 Send/Recv Pattern

### Applicable Scenarios

- Message-driven architecture
- Unknown data arrival time
- Need immediate data (ImmData)
- Simple request/response

### Example Code

- [02-send-recv/send_recv.c](./02-send-recv/send_recv.c) - Cross-machine communication
- [02-send-recv/01_loopback_send_recv.c](./02-send-recv/01_loopback_send_recv.c) - Local Loopback

### Build and Run

```bash
cd 02-send-recv
make

# Terminal 1 - Server
./send_recv server

# Terminal 2 - Client
./send_recv client 192.168.1.100
```

---

## 4.3 RDMA Write Pattern

### Applicable Scenarios

- High-performance data transfer
- Actively pushing data to remote
- Streaming data transfer
- Data synchronization without peer awareness

### Characteristics

| Feature | Description |
|---------|-------------|
| Zero CPU involvement | Remote host is unaware |
| Sender control | When to write is decided by the sender |
| Push mode | Actively pushes data to the receiver |
| Lowest latency | Faster than Send/Recv |

### Example Code

[01-rdma-write/rdma_write.c](./01-rdma-write/rdma_write.c)

---

## 4.4 RDMA Read Pattern

### Applicable Scenarios

- Remote data processing
- Data pulling in distributed computing
- Request/response pattern
- Reading remote configuration or state

### Example Code

[03-rdma-read/rdma_read.c](./03-rdma-read/rdma_read.c)

---

## 4.5 Atomic Operations

### Supported Operations

| Operation | Description | Usage |
|-----------|-------------|-------|
| **Fetch & Add (FAA)** | Read value, increment, write back | Counters, sequence number generation |
| **Compare & Swap (CAS)** | Swap if equal | Distributed locks, optimistic locking |

### Example Code

[04-atomic/atomic_ops.c](./04-atomic/atomic_ops.c)

---

## 4.6 Immediate Data (ImmData)

- 32-bit immediate data, sent along with the WR
- No additional memory operations needed

---

## 4.7 Connection Establishment Methods

### Method 1: ibverbs (Manual)

Configure QP attributes after exchanging QP info through Socket.

### Method 2: RDMA CM (Advanced API)

```c
// Server
rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
rdma_bind_addr(listen_id, &addr);
rdma_listen(listen_id, 1);
rdma_accept(event->id, NULL);

// Client
rdma_create_id(ec, &id, NULL, RDMA_PS_TCP);
rdma_resolve_addr(id, NULL, &server_addr, 1000);
rdma_connect(id, &conn_param);
```

---

## 4.8 Multi-threaded RDMA Programming

### Threading Models

| Model | Characteristics |
|-------|----------------|
| Single QP multi-threaded | Requires locking, high contention |
| Independent QP per thread | Lock-free, good performance (recommended) |

### Resource Management Strategy

| Resource | Strategy |
|----------|----------|
| QP | Independent per thread |
| CQ | Independent per thread |
| PD | Shared |
| MR | Pre-registered, partitioned on demand |

---

## 4.9 Performance Optimization Tips

### 1. Memory Optimization

```c
// Pre-registered memory pool
#define POOL_SIZE (16 * 1024 * 1024)
void *pool = aligned_alloc(4096, POOL_SIZE);
struct ibv_mr *mr = ibv_reg_mr(pd, pool, POOL_SIZE, 
    IBV_ACCESS_REMOTE_WRITE);
```

### 2. Batching

```c
// Chain multiple WRs
wr1.next = &wr2;
ibv_post_send(qp, &wr1, &bad_wr);
```

### 3. Asynchronous Operations

```c
// Batch submission
for (int i = 0; i < batch_size; i++) {
    ibv_post_send(qp, &wr_array[i], &bad_wr);
}
// Batch polling
int completed = 0;
while (completed < batch_size) {
    completed += ibv_poll_cq(cq, batch_size - completed, &wc_array[completed]);
}
```

---

## 4.10 Debugging Methods

### WC Status Codes

| Status Code | Description |
|------------|-------------|
| `IBV_WC_SUCCESS` | Success |
| `IBV_WC_LOC_LEN_ERR` | Length error |
| `IBV_WC_WR_FLUSH_ERR` | Queue flushed |
| `IBV_WC_REM_INV_REQ_ERR` | Remote invalid request |

### Debugging Commands

```bash
# View devices
ibv_devices
ibv_devinfo -d mlx5_0

# Bandwidth test
perftest -z -d mlx5_0 -c -n 100000 -s 4096

# Kernel log
dmesg | grep -i rdma
```

---

## Exercises

1. **Multiple choice**: Scenario: You need to implement a distributed lock, which operation should you use?
2. **Programming**: Modify the Send/Recv example, add ImmData to pass message types
3. **Analysis**: Reasons for the performance difference between RDMA Write and Send/Recv
4. **Hands-on**: Implement a simple distributed counter

---

## Next Step

Congratulations on completing the RDMA introduction! Next you can:

- Read the [NVIDIA official documentation](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- Study the application of RDMA in distributed machine learning
- Set up an actual RDMA test environment
