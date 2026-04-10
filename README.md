# RDMA 101 Tutorial

**Remote Direct Memory Access (RDMA) Programming Complete Knowledge Map**

---

## Tutorial Overview

This tutorial is based on [NVIDIA RDMA Aware Programming v1.7](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17) and covers the complete knowledge map of RDMA programming from beginner to engineering practice.

**Environment Requirements**:
- Linux Ubuntu 22.04
- rdma-core / libibverbs installed (`sudo apt-get install libibverbs-dev librdmacm-dev`)
- InfiniBand or RoCE physical NIC (or SoftRoCE software emulation)
- `ibv_devinfo` shows normal output

**Features**:
- All code supports both InfiniBand and RoCE environments (automatic transport layer detection)
- Every key line of code has English comments
- Includes complete build commands and expected output descriptions
- Provides Shell scripts for environment checking and diagnostics

---

## Chapter Directory

### Preparation Stage

| Chapter | Content | Description |
|---------|---------|-------------|
| [ch00-prerequisites](./ch00-prerequisites/README.md) | Prerequisites | Environment setup, C basics, Socket programming |

### Phase 1: Core Concepts

| Chapter | Content | Description |
|---------|---------|-------------|
| [ch01-intro](./ch01-intro/README.md) | RDMA Fundamentals | Understand what RDMA is and why it's fast |
| [ch02-network-layer](./ch02-network-layer/README.md) | Network Technology Layer | InfiniBand, RoCE, iWARP, Verbs abstraction |

### Phase 2: Core Programming Objects

| Chapter | Content | Description |
|---------|---------|-------------|
| [ch03-verbs-api](./ch03-verbs-api/README.md) | Verbs API Introduction | Device init, PD, MR, CQ, QP complete programming |
| [ch04-qp-mr](./ch04-qp-mr/README.md) | QP and MR Deep Dive | Deep understanding of Queue Pairs and Memory Regions |

### Phase 3: Communication Practice

| Chapter | Content | Description |
|---------|---------|-------------|
| [ch05-communication](./ch05-communication/README.md) | Communication Modes | Send/Recv, RDMA Write/Read, Atomic operations |
| [ch06-connection](./ch06-connection/README.md) | Connection Management | Manual connection, RDMA CM, UD mode |

### Phase 4: Engineering Practice

| Chapter | Content | Description |
|---------|---------|-------------|
| [ch07-engineering](./ch07-engineering/README.md) | Engineering Practice | perftest, tuning tips, debug tools, error handling |
| [ch08-advanced](./ch08-advanced/README.md) | Advanced Topics | GPUDirect RDMA, NCCL, userspace driver internals |
| [ch09-quickref](./ch09-quickref/README.md) | Quick Reference | API reference, error code reference, Hello World template |

---

## Quick Start

### No RDMA Hardware? No Problem! (SoftRoCE One-Click Setup)

This tutorial fully supports learning on a **regular Linux machine without RDMA physical NIC**,
using SoftRoCE software emulation to achieve API behavior consistent with real hardware.

```bash
# Three steps to complete environment setup and verification:

# Step 1: One-click SoftRoCE setup
sudo ./scripts/setup_softrce.sh

# Step 2: Build all examples
make all

# Step 3: Run full test suite to verify environment
./scripts/run_all_tests.sh
```

> Detailed SoftRoCE configuration instructions and troubleshooting:
> [ch00-prerequisites/00_environment.md](./ch00-prerequisites/00_environment.md)

### With RDMA Hardware

```bash
# 1. Install dependencies
sudo apt-get install libibverbs-dev librdmacm-dev rdma-core perftest

# 2. Check devices
ibv_devices
ibv_devinfo

# 3. Build and test
make all
./scripts/run_all_tests.sh
```

### Learning Order

```
ch00 -> ch01 -> ch02 -> ch03 -> ch04 -> ch05 -> ch06 -> ch07 -> ch08 -> ch09
```

It is recommended to study in order, building a solid foundation in each phase before moving on.

### Makefile Command Reference

```bash
make all       # Build all example programs
make setup     # One-click SoftRoCE environment setup (requires sudo)
make test      # Run full test suite
make check-env # Check RDMA environment status
make clean     # Clean build artifacts
```

---

## Example Code Overview

### ch00-prerequisites - Prerequisites

| File | Description |
|------|-------------|
| `01_struct_pointer.c` | C language: struct pointer exercises |
| `02_polling_pattern.c` | C language: polling pattern exercises |
| `tcp_server.c` / `tcp_client.c` | Socket: TCP client/server |

### ch02-network-layer - Network Technology Layer

| File | Description |
|------|-------------|
| `01-infiniband/ib_env_check.sh` | IB environment check script |
| `01-infiniband/ib_port_detail.c` | ibv_query_port() full field printing |
| `02-roce/roce_env_check.sh` | RoCE GID/PFC/ECN check |
| `02-roce/roce_gid_query.c` | ibv_query_gid() enumeration + IB/RoCE detection |
| `03-iwarp/iwarp_query.c` | iWARP transport type query |
| `04-verbs-abstraction/verbs_any_transport.c` | Verbs unified abstraction layer demo |

### ch03-verbs-api - Verbs API Introduction

| File | Description |
|------|-------------|
| `01-initialization/01_init_resources.c` | Six-step init + device capability query |
| `01-initialization/02_multi_device.c` | Multi-device multi-port enumeration |
| `02-qp-state/qp_state.c` | QP state transitions (IB/RoCE dual-mode) |
| `02-qp-state/01_qp_error_recovery.c` | QP error recovery demo |
| `03-pd/pd_isolation.c` | PD Protection Domain isolation demo |
| `04-mr/mr_access_flags.c` | MR access permission test |
| `04-mr/mr_multi_reg.c` | Same memory multiple registrations |
| `05-cq/cq_event_driven.c` | Event-driven CQ demo |
| `05-cq/cq_overflow.c` | CQ overflow demo |

### ch05-communication - Communication Modes

| File | Description |
|------|-------------|
| `01-rdma-write/rdma_write.c` | RDMA Write (C/S mode) |
| `01-rdma-write/01_write_imm.c` | RDMA Write with Immediate |
| `02-send-recv/01_loopback_send_recv.c` | Send/Recv Loopback (multi-SGE) |
| `02-send-recv/02_sge_demo.c` | Scatter-Gather demo |
| `02-send-recv/03_rnr_error_demo.c` | RNR error demo |
| `03-rdma-read/rdma_read.c` | RDMA Read (C/S mode) |
| `03-rdma-read/01_batch_read.c` | Batch RDMA Read |
| `04-atomic/atomic_ops.c` | FAA + CAS atomic operations |
| `04-atomic/01_alignment_error.c` | Atomic operation alignment error |
| `04-atomic/02_spinlock.c` | CAS distributed spinlock |

### ch06-connection - Connection Management

| File | Description |
|------|-------------|
| `01-manual-connect/manual_connect.c` | Manual connection (dual-process -s/-c) |
| `02-rdma-cm/rdma_cm_example.c` | RDMA CM API complete example |
| `03-ud-mode/ud_loopback.c` | UD mode Loopback + Address Handle |

### ch07-engineering - Engineering Practice

| File | Description |
|------|-------------|
| `01-perftest/run_perftest.sh` | Automated performance benchmarking |
| `02-tuning/inline_data.c` | Inline Data optimization |
| `02-tuning/unsignaled_send.c` | Unsignaled Send optimization |
| `02-tuning/srq_demo.c` | Shared Receive Queue demo |
| `03-debug/rdma_diag.sh` | RDMA environment diagnostic script |
| `04-error-handling/trigger_errors.c` | Deliberately trigger various WC errors |
| `04-error-handling/error_diagnosis.c` | Error code diagnostic tool |

### ch08-advanced - Advanced Topics

| File | Description |
|------|-------------|
| `01-gpudirect/gpudirect_check.sh` | GPUDirect environment check |
| `01-gpudirect/gpudirect_framework.c` | GPUDirect RDMA code framework |
| `02-nccl/nccl_env_check.sh` | NCCL environment check |
| `03-userspace-driver/userspace_driver_trace.sh` | Userspace driver strace trace |

### ch09-quickref - Quick Reference

| File | Description |
|------|-------------|
| `hello_rdma.c` | RDMA Hello World (minimal runnable template) |
| `env_check.sh` | One-click environment check script |
| `error_cheatsheet.c` | Common error lookup tool |

---

## Common Utility Library

`common/rdma_utils.h` provides IB/RoCE dual-mode support shared by all programs:

```c
#include "rdma_utils.h"

// Automatically detect transport layer type
enum rdma_transport t = detect_transport(ctx, port);

// One-call QP state transition (automatically handles IB/RoCE differences)
qp_full_connect(qp, &remote_ep, port, is_roce, access_flags);

// TCP out-of-band information exchange
exchange_endpoint_tcp(server_ip, tcp_port, &local_ep, &remote_ep);

// Print complete WC details
print_wc_detail(&wc);
```

---

## Core API Quick Reference

```c
// Initialization
ibv_get_device_list(&n)               // Get device list
ibv_open_device(dev)                   // Open device
ibv_query_device(ctx, &attr)           // Query device capabilities
ibv_alloc_pd(ctx)                      // Create Protection Domain
ibv_reg_mr(pd, buf, len, flags)        // Register memory
ibv_create_cq(ctx, depth, ...)         // Create Completion Queue
ibv_create_qp(pd, &attr)              // Create Queue Pair
ibv_modify_qp(qp, &attr, flags)       // Transition QP state

// Data Transfer
ibv_post_recv(qp, &wr, &bad_wr)       // Submit receive
ibv_post_send(qp, &wr, &bad_wr)       // Submit send
ibv_poll_cq(cq, n, &wc)               // Poll completions

// Release (reverse order)
ibv_destroy_qp(qp)
ibv_destroy_cq(cq)
ibv_dereg_mr(mr)
ibv_dealloc_pd(pd)
ibv_close_device(ctx)
```

---

## QP State Machine

```
RESET -> INIT -> RTR -> RTS
  ^                    |
  +------ ERROR <------+
```

---

## Learning Path

| Phase | Weeks | Chapters | Goal |
|-------|-------|----------|------|
| Preparation | 1-2 | ch00 | C basics, Socket programming, environment setup |
| Concepts | 1 | ch01-ch02 | Understand RDMA principles and network technologies |
| Programming | 2 | ch03-ch04 | Master Verbs API and core objects |
| Communication | 2-3 | ch05-ch06 | Implement four communication modes and connection management |
| Engineering | 2 | ch07-ch09 | Performance tuning, debugging, error handling |

---

## References

- [NVIDIA RDMA Programming Manual v1.7](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- [rdma-core GitHub](https://github.com/linux-rdma/rdma-core)
- [Linux Kernel RDMA Documentation](https://www.kernel.org/doc/html/latest/infiniband/)

---

## License

MIT License
