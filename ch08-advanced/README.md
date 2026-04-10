# Chapter 8: Advanced Topics

**GPUDirect RDMA, NCCL, and Userspace Driver Internals**

---

## Chapter Overview

This chapter covers three advanced RDMA topics to help you understand how RDMA is used
in real-world AI/HPC scenarios, as well as the inner workings of the userspace driver.
These concepts are not required for writing basic RDMA programs, but they are very important
for understanding modern AI cluster communication and performance tuning.

---

## Table of Contents

| Section | Content | Description |
|---------|---------|-------------|
| [01-gpudirect](./01-gpudirect/) | GPUDirect RDMA | Direct RDMA access to GPU memory, bypassing CPU memory copies |
| [02-nccl](./02-nccl/) | NCCL and RDMA | How NVIDIA Collective Communication Library leverages RDMA |
| [03-userspace-driver](./03-userspace-driver/) | Userspace Driver Internals | Why ibv_post_send does not require a system call |

---

## 01 - GPUDirect RDMA

### What is GPUDirect RDMA?

Traditional GPU network communication path:

```
GPU Memory -> cudaMemcpy -> CPU Memory -> RDMA Send -> Network -> RDMA Recv -> CPU Memory -> cudaMemcpy -> GPU Memory
```

GPUDirect RDMA path:

```
GPU Memory -> RDMA Send -> Network -> RDMA Recv -> GPU Memory
```

**Key advantages**:
- Eliminates two CPU memory copies (cudaMemcpy)
- Reduces latency, improves bandwidth utilization
- Data is transferred directly between GPU and NIC via PCIe

### How It Works

```
+----------------+     PCIe     +----------------+
|   GPU (CUDA)   | <----------> |   RDMA NIC     |
|   VRAM         |  Direct DMA  |   (HCA)        |
+----------------+              +----------------+
       |                              |
       +------ Bypasses CPU Memory ---+
```

1. **nvidia_peermem module**: Allows RDMA NIC to directly access GPU memory
2. **CUDA API**: `cuMemAlloc()` allocates GPU memory
3. **Verbs API**: `ibv_reg_mr()` registers GPU memory as MR
4. **Data transfer**: NIC reads/writes data directly from/to GPU memory via PCIe

### Prerequisites

| Requirement | Description |
|-------------|-------------|
| NVIDIA GPU | CUDA-capable GPU (Kepler or newer) |
| nvidia_peermem | Kernel module, included with CUDA 11.4+ |
| Mellanox NIC | ConnectX-4 or newer RDMA NIC |
| PCIe topology | GPU and NIC should ideally be under the same PCIe switch |

### Files in This Section

| File | Description |
|------|-------------|
| `gpudirect_check.sh` | One-click GPUDirect environment readiness check |
| `gpudirect_framework.c` | GPUDirect RDMA code framework (supports compilation with/without CUDA) |

---

## 02 - NCCL (NVIDIA Collective Communications Library)

### What is NCCL?

NCCL is NVIDIA's collective communication library, designed for multi-GPU / multi-node AI training.
It is the underlying communication backend for PyTorch `torch.distributed`, TensorFlow, DeepSpeed, and other frameworks.

### NCCL and RDMA Relationship

```
+---------------------------------------+
|         PyTorch / TensorFlow          |  <- User code
+---------------------------------------+
|         torch.distributed / Horovod   |  <- Distributed framework
+---------------------------------------+
|                NCCL                   |  <- Collective communication library
+----------+------------+---------------+
|  IB Verbs|  Socket    |   NVLink      |  <- Transport layer
|  (RDMA)  |  (TCP/IP)  |  (Intra-node) |
+----------+------------+---------------+
```

When NCCL detects an RDMA NIC, it automatically uses IB Verbs for inter-node communication:
- Supports GPUDirect RDMA (GPU memory goes directly to the network)
- Supports RDMA Write (zero-copy transfer)
- Automatically selects optimal NIC-GPU affinity

### Key Environment Variables

| Variable | Purpose | Common Values |
|----------|---------|---------------|
| `NCCL_IB_DISABLE` | Disable IB | 0 (enable) / 1 (disable) |
| `NCCL_IB_HCA` | Specify HCA to use | `mlx5_0` |
| `NCCL_IB_GID_INDEX` | RoCE GID index | `3` (RoCE v2) |
| `NCCL_NET_GDR_LEVEL` | GPUDirect level | `5` (cross-node) |
| `NCCL_DEBUG` | Debug level | `INFO` / `WARN` / `TRACE` |
| `NCCL_DEBUG_SUBSYS` | Debug subsystem | `NET` / `INIT` / `ALL` |
| `NCCL_SOCKET_IFNAME` | Control plane interface | `eth0` |

### Files in This Section

| File | Description |
|------|-------------|
| `nccl_env_check.sh` | Check NCCL installation and environment variable configuration |

---

## 03 - Userspace Driver Internals

### Why doesn't ibv_post_send require a system call?

This is one of the core secrets behind RDMA's high performance. In traditional network programming,
each `send()` requires a system call (context switch ~1us). However, RDMA's `ibv_post_send()` is
a pure userspace operation.

### How It Works

```
+-------------------------------------------------+
|                   Userspace                      |
|                                                  |
|  Application                                     |
|    |                                             |
|    v                                             |
|  libibverbs (libmlx5.so / libhfi1.so)           |
|    |                                             |
|    v                                             |
|  Write WQE to Send Queue (mmap'd shared memory) |
|    |                                             |
|    v                                             |
|  Doorbell Write (write HCA register, also mmap'd)|
|    |                                             |
+----+---------------------------------------------+
|    |              Kernel space                    |
|    |  (ibv_post_send does NOT go through here!)  |
+----+---------------------------------------------+
|    v              Hardware                        |
|  HCA reads WQE -> DMA reads data -> sends to network |
+-------------------------------------------------+
```

### Key Steps

1. **Initialization phase** (requires kernel involvement via ioctl):
   - `ibv_open_device()` -> `open("/dev/infiniband/uverbs0")`
   - `ibv_create_qp()` -> `ioctl()` to have the kernel allocate QP resources
   - Kernel maps QP's Send/Recv Queue to userspace via `mmap()`

2. **Data path** (pure userspace, no system calls):
   - `ibv_post_send()` -> directly writes WQE to mmap'd SQ memory
   - Writes Doorbell register to notify HCA -> also an mmap'd MMIO address
   - HCA reads data via DMA and sends it

3. **Completion notification**:
   - `ibv_poll_cq()` -> directly reads mmap'd CQ memory, also pure userspace

### Which operations require the kernel and which don't?

| Operation | Requires Kernel | Reason |
|-----------|----------------|--------|
| `ibv_open_device` | Yes | Opens `/dev/infiniband/uverbs*` |
| `ibv_alloc_pd` | Yes | ioctl to allocate kernel resources |
| `ibv_reg_mr` | Yes | ioctl to pin physical pages |
| `ibv_create_cq` | Yes | ioctl to allocate + mmap mapping |
| `ibv_create_qp` | Yes | ioctl to allocate + mmap mapping |
| `ibv_modify_qp` | Yes | ioctl to modify QP state |
| `ibv_post_send` | **No** | Directly writes mmap'd memory + doorbell |
| `ibv_post_recv` | **No** | Directly writes mmap'd memory + doorbell |
| `ibv_poll_cq` | **No** | Directly reads mmap'd memory |

### Files in This Section

| File | Description |
|------|-------------|
| `userspace_driver_trace.sh` | Use strace to trace RDMA program system calls |

---

## Study Recommendations

1. **Understand the concepts first**: This chapter focuses on conceptual understanding; you don't need to write GPUDirect code right away
2. **Run the check scripts**: Even without a GPU, you can run the check scripts to understand the environment requirements
3. **strace experiment**: `userspace_driver_trace.sh` is the most worthwhile hands-on exercise — it lets you see the RDMA userspace driver in action with your own eyes
4. **Review earlier chapters**: Go back to the Send/Recv program in ch05 and think about what happens behind the scenes when `ibv_post_send()` is called

---

## Further Reading

- [NVIDIA GPUDirect RDMA Official Documentation](https://docs.nvidia.com/cuda/gpudirect-rdma/)
- [NCCL Official Documentation](https://docs.nvidia.com/deeplearning/nccl/)
- [Mellanox RDMA Aware Programming User Manual](https://docs.nvidia.com/networking/display/rdmacore/)
- [libibverbs Source Code (rdma-core)](https://github.com/linux-rdma/rdma-core)

---

*Next chapter: [ch09-quickref](../ch09-quickref/) - API Quick Reference and Toolkit*
