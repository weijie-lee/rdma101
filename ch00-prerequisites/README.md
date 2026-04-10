# Chapter 0: Prerequisites

## Learning Objectives

| Objective | Description |
|-----------|-------------|
| Set up development environment | Master RDMA development environment setup on Ubuntu |
| Configure SoftRoCE | Use software-simulated RDMA environment |
| Master C language basics | Structs, pointers, polling patterns |
| Understand Socket programming | TCP/UDP programming fundamentals |

---

## 0.1 Environment Setup

### Install Development Dependencies

```bash
# Update package sources
sudo apt update

# Install RDMA development libraries
sudo apt install -y libibverbs-dev librdmacm-dev

# Install tools and test software
sudo apt install -y ibverbs-utils rdma-core perftest

# Install build tools
sudo apt install -y build-essential gcc gdb make
```

### Configure SoftRoCE (Software Simulation)

```bash
# Load kernel module
sudo modprobe rdma_rxe

# View network interfaces
ip link show

# Bind network interface (assuming eth0)
sudo rdma link add rxe0 type rxe netdev eth0

# Verify
ibv_devices
ibv_devinfo -d rxe0
```

See: [00_environment.md](./00_environment.md)

---

## 0.2 C Language Prerequisites

### Structs and Pointers

RDMA programming heavily uses struct pointers:

```c
struct ibv_context *ctx;
struct ibv_qp *qp;
struct ibv_mr *mr;
```

**Example code**: [01_struct_pointer.c](./01_struct_pointer.c)

```bash
gcc -o 01_struct_pointer 01_struct_pointer.c
./01_struct_pointer
```

### Polling Pattern

RDMA uses non-blocking polling:

```c
while (1) {
    ne = ibv_poll_cq(cq, 1, &wc);
    if (ne > 0) {
        // Handle completion event
    }
}
```

**Example code**: [02_polling_pattern.c](./02_polling_pattern.c)

```bash
gcc -o 02_polling_pattern 02_polling_pattern.c
./02_polling_pattern
```

---

## 0.3 Socket Programming Basics

### TCP Server/Client Model

See: [tcp_server.c](./tcp_server.c) and [tcp_client.c](./tcp_client.c)

---

## 0.4 Build and Run

### Build Commands

```bash
# Exercises that don't require RDMA libraries
gcc -o program program.c

# Programs that require RDMA libraries
gcc -o program program.c -libverbs

# Using Makefile
make
make clean
```

---

## Next Step

Proceed to the next chapter: [Chapter 1: RDMA Basic Concepts](../ch01-intro/README.md)
