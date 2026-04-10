# Chapter 3: Getting Started with Verbs API

## Learning Objectives

| Objective | Description |
|-----------|-------------|
| Master the basic RDMA programming flow | Understand the complete flow: device discovery -> resource creation -> communication -> destruction |
| Learn to initialize hardware devices | Master ibv_get_device_list, ibv_open_device |
| Understand resource creation and destruction | Master creation and destruction of PD, CQ, QP, MR |
| Be able to read simple RDMA programs | Understand each part of the example code |

---

## 2.1 Verbs API Overview

### What is libibverbs?

**libibverbs** is the low-level API for RDMA programming on Linux, providing a hardware-independent programming interface.

```
┌─────────────────────────────────────────────┐
│           Application (Your Program)         │
└─────────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────┐
│           libibverbs (Verbs API)            │
│   - Device Management (ibv_*)               │
│   - Memory Management (ibv_*)               │
│   - Communication Operations (ibv_post_*)   │
└─────────────────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────┐
│         RDMA Hardware Driver                │
│   - Mellanox driver                        │
│   - Intel OPA driver                        │
└─────────────────────────────────────────────┘
```

### Core Header Files
```c
#include <infiniband/verbs.h>    // Verbs API
#include <rdma/rdma_cma.h>      // RDMA CM (High-level API)
```

---

## 2.2 Device Discovery and Initialization

### Complete Flow

```c
// 1. Get device list
struct ibv_device **device_list;
int num_devices;
device_list = ibv_get_device_list(&num_devices);

// 2. Select device
struct ibv_device *device = device_list[0];

// 3. Open device
struct ibv_context *context;
context = ibv_open_device(device);

// 4. Query device information
struct ibv_device_attr attr;
ibv_query_device(context, &attr);
```

### Code Walkthrough

```c
// Get all RDMA devices
device_list = ibv_get_device_list(&num_devices);
if (!device_list) {
    perror("Failed to get device list");
    return -1;
}
printf("Found %d RDMA devices\n", num_devices);

// Print device names
for (int i = 0; i < num_devices; i++) {
    printf("  %d: %s\n", i, ibv_get_device_name(device_list[i]));
}
```

---

## 2.3 Resource Creation Flow

### Creation Order

```
1. Protection Domain (PD)   - Protection domain, isolates resources of different applications
         │
         ▼
2. Completion Queue (CQ)    - Completion queue, stores operation completion notifications
         │
         ▼
3. Queue Pair (QP)          - Queue pair, basic communication unit
         │
         ▼
4. Memory Region (MR)       - Memory region, registers accessible memory
```

### Protection Domain (PD)

```c
// Allocate PD - container for all RDMA resources
struct ibv_pd *pd;
pd = ibv_alloc_pd(context);
if (!pd) {
    perror("Failed to allocate PD");
    return -1;
}
printf("PD allocated, handle=%d\n", pd->handle);
```

### Completion Queue (CQ)

```c
// Create CQ - stores operation completion events
struct ibv_cq *cq;
int cq_size = 128;  // Completion queue size

cq = ibv_create_cq(context, cq_size, NULL, NULL, 0);
if (!cq) {
    perror("Failed to create CQ");
    return -1;
}
printf("CQ created with %d entries\n", cq_size);
```

### Queue Pair (QP)

```c
// Create QP - basic communication unit
struct ibv_qp_init_attr qp_init_attr = {
    .send_cq = cq,           // Send completion queue
    .recv_cq = cq,           // Receive completion queue
    .qp_type = IBV_QPT_RC,   // Reliable connection
    .cap = {
        .max_send_wr = 128,  // Max send WR count
        .max_recv_wr = 128,  // Max receive WR count
        .max_send_sge = 1,   // Max send SGE count
        .max_recv_sge = 1,   // Max receive SGE count
    },
};

struct ibv_qp *qp;
qp = ibv_create_qp(pd, &qp_init_attr);
if (!qp) {
    perror("Failed to create QP");
    return -1;
}
printf("QP created, number=%u\n", qp->qp_num);
```

### Memory Region (MR)

```c
// Register memory - make it accessible to the RDMA device
#define BUFFER_SIZE 4096
char *buffer = malloc(BUFFER_SIZE);

struct ibv_mr *mr;
mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, 
                 IBV_ACCESS_LOCAL_WRITE |    // Local writable
                 IBV_ACCESS_REMOTE_READ |    // Remote readable
                 IBV_ACCESS_REMOTE_WRITE);   // Remote writable
if (!mr) {
    perror("Failed to register MR");
    return -1;
}
printf("MR registered: lkey=%u, rkey=%u\n", mr->lkey, mr->rkey);
```

### Memory Permission Flags

| Flag | Description |
|------|-------------|
| `IBV_ACCESS_LOCAL_WRITE` | Local CPU writable |
| `IBV_ACCESS_REMOTE_READ` | Remote readable |
| `IBV_ACCESS_REMOTE_WRITE` | Remote writable |
| `IBV_ACCESS_ATOMIC` | Remote atomic operations |

---

## 2.4 QP State Machine

### State Transition Diagram

```
         ┌─────────┐
         │ RESET   │ <── Initial state
         └────┬────┘
              │ ibv_modify_qp(IBV_QP_STATE)
              ▼
         ┌─────────┐
         │  INIT   │ <── Initialized, cannot communicate
         └────┬────┘
              │ ibv_modify_qp(IBV_QP_STATE)
              ▼
         ┌─────────┐
         │   RTR   │ <── Ready to Receive, can receive
         └────┬────┘
              │ ibv_modify_qp(IBV_QP_STATE)
              ▼
         ┌─────────┐
         │   RTS   │ <── Ready to Send, can send
         └─────────┘
```

### Transition to INIT

```c
struct ibv_qp_attr attr = {
    .qp_state = IBV_QPS_INIT,
    .pkey_index = 0,
    .port_num = 1,
    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                      IBV_ACCESS_REMOTE_READ |
                      IBV_ACCESS_REMOTE_WRITE,
};

ibv_modify_qp(qp, &attr, IBV_QP_STATE);
```

### Transition to RTR (Ready to Receive)

```c
// Need to know the remote QP number and LID
attr.qp_state = IBV_QPS_RTR;
attr.path_mtu = IBV_MTU_256;
attr.dest_qp_num = remote_qp_num;    // Remote QP number
attr.rq_psn = 0;
attr.max_dest_rd_atomic = 1;
attr.min_rnr_timer = 12;
attr.ah_attr.dlid = remote_lid;      // Remote LID
attr.ah_attr.sl = 0;
attr.ah_attr.port_num = 1;

ibv_modify_qp(qp, &attr, IBV_QP_STATE);
```

### Transition to RTS (Ready to Send)

```c
attr.qp_state = IBV_QPS_RTS;
attr.sq_psn = 0;
attr.timeout = 14;
attr.retry_cnt = 7;
attr.rnr_retry = 7;
attr.max_rd_atomic = 1;

ibv_modify_qp(qp, &attr, IBV_QP_STATE);
```

---

## 2.5 Communication Operations

### Post Send

```c
// Prepare SGE (Scatter/Gather Element)
struct ibv_sge sge = {
    .addr = (uint64_t)buffer,
    .length = BUFFER_SIZE,
    .lkey = mr->lkey,
};

// Prepare Work Request
struct ibv_send_wr wr = {
    .wr_id = 0,
    .sg_list = &sge,
    .num_sge = 1,
    .opcode = IBV_WR_SEND,           // Operation type
    .send_flags = IBV_SEND_SIGNALED, // Request completion notification
};

// Submit send request
struct ibv_send_wr *bad_wr;
ibv_post_send(qp, &wr, &bad_wr);
```

### Post Recv

```c
struct ibv_recv_wr wr = {
    .wr_id = 0,
    .sg_list = &sge,
    .num_sge = 1,
};

struct ibv_recv_wr *bad_wr;
ibv_post_recv(qp, &wr, &bad_wr);
```

### Polling Completion

```c
struct ibv_wc wc;
int ne = ibv_poll_cq(cq, 1, &wc);

if (ne > 0) {
    if (wc.status == IBV_WC_SUCCESS) {
        printf("Operation completed successfully\n");
        printf("  opcode: %d\n", wc.opcode);
        printf("  byte_len: %d\n", wc.byte_len);
    } else {
        printf("Operation failed: %s\n", ibv_wc_status_str(wc.status));
    }
}
```

### WC Status Codes

| Status Code | Description |
|-------------|-------------|
| `IBV_WC_SUCCESS` | Success |
| `IBV_WC_LOC_LEN_ERR` | Length error |
| `IBV_WC_LOC_QP_OP_ERR` | QP operation error |
| `IBV_WC_WR_FLUSH_ERR` | Queue flushed |
| `IBV_WC_REM_INV_REQ_ERR` | Remote invalid request |

---

## 2.6 Resource Destruction

### Destruction Order (Reverse of Creation Order)

```c
// 1. Deregister memory
ibv_dereg_mr(mr);
free(buffer);

// 2. Destroy QP
ibv_destroy_qp(qp);

// 3. Destroy CQ
ibv_destroy_cq(cq);

// 4. Deallocate PD
ibv_dealloc_pd(pd);

// 5. Close device
ibv_close_device(context);

// 6. Free device list
ibv_free_device_list(device_list);
```

---

## 2.7 Complete Example Run Verification

### Compile
```bash
cd ch02-verbs-api/01-initialization
make
```

### Run (Requires RDMA Hardware)
```bash
./rdma_init
```

### Expected Output (With RDMA Device)
```
QP State Transition Example
============================

Found 1 RDMA device(s)
  0: mlx5_0
Opened device: mlx5_0
=== Port 1 Info ===
Link Layer: IB
State: ACTIVE
LID: 1
SM LID: 0
===================
Allocated PD
Created CQ with 128 entries
Created QP with num=1

=== Local QP Info ===
QP Number: 1
State: 0 (RESET)
Port: 1
PKey Index: 0
=====================

Step 1: RESET -> INIT

=== Local QP Info ===
QP Number: 1
State: 1 (INIT)
Port: 1
PKey Index: 0
=====================

Note: INIT->RTR requires remote QP info.
Complete state transition sequence:
  1. RESET -> INIT (done)
  2. INIT -> RTR (need remote QP num + LID)
  3. RTR -> RTS (done)

All resources cleaned up
```

### If No RDMA Device
```
Found 0 RDMA device(s)
No RDMA devices found
```

---

## 2.8 Common Error Troubleshooting

| Error | Cause | Solution |
|-------|-------|----------|
| `No RDMA devices found` | No RDMA hardware or driver | Check `ibv_devices` |
| `Failed to open device` | Insufficient permissions | Run with sudo |
| `Failed to allocate PD` | Insufficient resources | Check kernel limits |
| `Failed to register MR` | Invalid memory or insufficient permissions | Check memory alignment and permissions |

---

## Exercises

1. **Short Answer**: Why use ibv_get_device_list instead of directly opening a device?
2. **Conceptual**: What is the relationship between PD, QP, and MR?
3. **Diagram**: Draw the QP state transition diagram
4. **Programming**: Modify the example code to print device attribute information

---

## Next Step

Go to the next chapter: [Chapter 4: Deep Dive into QP and MR](../ch04-qp-mr/README.md)
