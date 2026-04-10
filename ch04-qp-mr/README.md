# Chapter 4: Deep Dive into QP and MR

## Learning Objectives

| Objective | Description |
|-----------|-------------|
| Deep understanding of Queue Pair | Master QP types, SGE, WR structures |
| Master Memory Region | Understand virtual-to-physical address translation |
| Understand RDMA operations | Master the principles of RDMA Write/Read |
| Understand CQ working modes | Master both polling and event-driven modes |

---

## 3.1 Queue Pair In-Depth

### QP Types

| Type | Name | Characteristics | Use Cases |
|------|------|----------------|-----------|
| **RC** | Reliable Connection | Guarantees order and delivery | Recommended |
| **UC** | Unreliable Connection | Order delivery only, no retransmission | Specific scenarios |
| **UD** | Unreliable Datagram | Connectionless, similar to UDP | Multicast, broadcast |

**RC (Reliable Connection) is recommended**, the most commonly used with complete functionality.

### QP Number

- Each QP has a unique numeric identifier
- Range: 0 ~ 2^24-1
- Local QP number is assigned at creation time
- Remote QP number needs to be exchanged through the connection process

### Relationship between QP and CQ

```
┌─────────────────────────────────────────┐
│              Queue Pair                  │
│  ┌───────────────┐  ┌───────────────┐  │
│  │  Send Queue  │  │ Receive Queue │  │
│  │    (SQ)      │  │     (RQ)     │  │
│  └───────┬───────┘  └───────┬───────┘  │
│          │                  │           │
│          └────────┬─────────┘           │
│                   ▼                    │
│          ┌───────────────┐             │
│          │  Completion   │             │
│          │    Queue      │             │
│          │     (CQ)      │             │
│          └───────────────┘             │
└─────────────────────────────────────────┘
```

---

## 3.2 Scatter/Gather Entry (SGE)

### What is SGE?

SGE allows **transferring multiple non-contiguous memory blocks in a single RDMA operation**:

```
Traditional mode:
[████████████]

SGE mode:
[██] + [██████] + [██]
```

### SGE Structure

```c
struct ibv_sge {
    uint64_t addr;      // Memory address
    uint32_t length;   // Length
    uint32_t lkey;     // lkey of the local MR
};
```

### Usage Example

```c
// Single-element SGE
struct ibv_sge sge = {
    .addr = (uint64_t)buffer,
    .length = BUFFER_SIZE,
    .lkey = mr->lkey,
};

// Multi-element SGE (for non-contiguous memory)
struct ibv_sge sge[2] = {
    {.addr = (uint64_t)buf1, .length = 100, .lkey = mr->lkey},
    {.addr = (uint64_t)buf2, .length = 200, .lkey = mr->lkey},
};

struct ibv_send_wr wr = {
    .sg_list = sge,
    .num_sge = 2,  // Using 2 SGEs
    ...
};
```

---

## 3.3 Memory Region In-Depth

### Virtual Address vs Physical Address

```
User Program          Kernel              RDMA NIC
   │               │                   │
   │ Virtual addr  │                   │
   ├──────────────▶│                   │
   │               │ Physical addr     │
   │               ├─────────────────▶│
   │               │                   │ DMA
   │               │                   ├──────────▶ Network
```

- The operating system uses **virtual addresses**
- The RDMA NIC uses **physical addresses** for DMA
- **The MR registration process** translates virtual addresses to physical addresses

### Keys

| Key | Description | Usage |
|-----|-------------|-------|
| **lkey** | Local key | Local operations (send, receive) |
| **rkey** | Remote key | Remote operations (RDMA Write/Read) |

```c
// Local operations use lkey
sge.lkey = mr->lkey;

// Remote operations need the peer's rkey
wr.rdma.rkey = remote_mr->rkey;
```

### Pre-registered Memory Pool

Frequent registration/deregistration of memory has significant overhead; pre-registration is recommended:

```c
#define POOL_SIZE (16 * 1024 * 1024)  // 16MB
char *memory_pool = malloc(POOL_SIZE);

// Register once
struct ibv_mr *pool_mr = ibv_reg_mr(pd, memory_pool, POOL_SIZE,
                                        IBV_ACCESS_REMOTE_WRITE);

// Reuse different regions of this memory
void *alloc_buf(size_t size) {
    static size_t offset = 0;
    void *ptr = memory_pool + offset;
    offset += size;
    return ptr;
}
```

---

## 3.4 RDMA Write Operation

### Principle

Directly writes to the remote host's memory, **without remote CPU involvement**.

```
Local                              Remote
┌─────────┐                      ┌─────────┐
│ Send Q  │───RDMA Write────────▶│ Memory  │
└─────────┘                      └─────────┘
    │                                ▲
    │    WC (completion notification)│
    └────────────────────────────────┘
```

### Code Example

```c
// Sender
struct ibv_sge sge = {
    .addr = (uint64_t)local_buffer,
    .length = data_size,
    .lkey = local_mr->lkey,
};

struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_WRITE,
    .wr.rdma = {
        .remote_addr = remote_buffer_addr,  // Remote address
        .rkey = remote_buffer_rkey,         // Remote key
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};

ibv_post_send(qp, &wr, &bad_wr);

// Wait for completion
poll_cq(cq, &wc);
```

### Characteristics

| Feature | Description |
|---------|-------------|
| Peer cooperation | No remote CPU involvement needed |
| Sender control | Sender decides when to write |
| Push mode | Suitable for actively pushing data to remote |
| Performance | Lowest latency |

---

## 3.5 RDMA Read Operation

### Principle

Directly reads the remote host's memory, **data is pulled to local**:

```
Local                              Remote
┌─────────┐                      ┌─────────┐
│ Send Q  │◀───RDMA Read────────│ Memory  │
└─────────┘                      └─────────┘
    │                                │
    │    WC + data arrives locally   │
    └────────────────────────────────┘
```

### Code Example

```c
// Reader
struct ibv_sge sge = {
    .addr = (uint64_t)local_buffer,   // Data will be placed here
    .length = data_size,
    .lkey = local_mr->lkey,
};

struct ibv_send_wr wr = {
    .opcode = IBV_WR_RDMA_READ,
    .wr.rdma = {
        .remote_addr = remote_buffer_addr,  // Remote address
        .rkey = remote_buffer_rkey,         // Remote key
    },
    .sg_list = &sge,
    .num_sge = 1,
    .send_flags = IBV_SEND_SIGNALED,
};

ibv_post_send(qp, &wr, &bad_wr);

// Wait for completion, then local_buffer contains the remote data
poll_cq(cq, &wc);
```

### Characteristics

| Feature | Description |
|---------|-------------|
| Peer cooperation | Remote host is unaware |
| Pull mode | Suitable for actively pulling remote data |
| Data location | Data is in local memory after completion |

---

## 3.6 Send/Recv Operation

### Principle

Traditional message passing mode, **requires cooperation from both sides**:

```
Sender                           Receiver
  │                                │
  │───post_send()────────────────▶│
  │                                │
  │        post_recv()             │
  │◀───────────────────────────────│
  │                                │
  │────post_send()───────────────▶│───WC
  │◀──────WC────────────────────────│
```

### Comparison Table

| Feature | Send/Recv | RDMA Write |
|---------|-----------|------------|
| Peer cooperation | Required | Not required |
| Control | Receiver | Sender |
| Immediate data | Supported (ImmData) | Not supported |
| Typical usage | Message notification | Data transfer |

---

## 3.7 Completion Queue (CQ) In-Depth

### Polling Mode

```c
while (1) {
    struct ibv_wc wc;
    int ne = ibv_poll_cq(cq, 1, &wc);
    
    if (ne < 0) {
        perror("poll CQ failed");
        break;
    }
    
    if (ne > 0) {
        if (wc.status != IBV_WC_SUCCESS) {
            printf("Error: %s\n", ibv_wc_status_str(wc.status));
            continue;
        }
        
        switch (wc.opcode) {
            case IBV_WC_SEND:
                printf("Send completed\n");
                break;
            case IBV_WC_RDMA_WRITE:
                printf("RDMA Write completed\n");
                break;
            case IBV_WC_RDMA_READ:
                printf("RDMA Read completed, got %d bytes\n", wc.byte_len);
                break;
        }
    }
}
```

### Event Mode

```c
// Create CQ with event channel
struct ibv_comp_channel *channel;
channel = ibv_create_comp_channel(context);
cq = ibv_create_cq(context, cq_size, NULL, channel, 0);

// Wait for event
struct ibv_cq *ev_cq;
void *ev_ctx;
ibv_get_cq_event(channel, &ev_cq, &ev_ctx);

// Process completion
ibv_ack_cq_event(ev_cq, 1);
```

---

## 3.8 Running Verification

### Compilation
```bash
cd ch04-communication/01-rdma-write
make

cd ../03-rdma-read
make
```

### Testing RDMA Read

```bash
# Machine A (Server)
./rdma_read server

# Machine B (Client)
./rdma_read client <A_ip>
```

### Expected Output

**Server side:**
```
Server: filling data...
Server: data ready at addr=..., rkey=...
Server: waiting...
```

**Client side:**
```
Client: reading from server...
Remote: addr=..., rkey=...
Client: read data = "Hello from server! Time: ..."
```

---

## Exercises

1. **Conceptual**: What is the difference between RC and UD QP?
2. **Programming**: Modify the RDMA Read example to implement bidirectional data exchange
3. **Analysis**: Why does RDMA Write have lower latency than Send/Recv?
4. **Hands-on**: Use multiple SGEs to implement non-contiguous memory transfer

---

## Next Step

Proceed to the next chapter: [Chapter 5: Communication Patterns in Practice](../ch05-communication/README.md)
