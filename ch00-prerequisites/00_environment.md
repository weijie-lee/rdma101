# Environment Setup Guide — SoftRoCE Complete Manual

## Learning Objectives

| Objective | Description |
|-----------|-------------|
| Understand SoftRoCE | What is SoftRoCE and why it can replace real RDMA hardware for learning |
| One-click environment setup | Use scripts to quickly set up the SoftRoCE development environment |
| Understand key differences | SoftRoCE vs real hardware differences (LID=0, GID, MTU) |
| Master troubleshooting | Solve common issues in the SoftRoCE environment |

---

## 1. SoftRoCE Overview

### What is SoftRoCE?

**SoftRoCE (Soft RDMA over Converged Ethernet)** is a software RDMA implementation in the Linux kernel.
Through the `rdma_rxe` kernel module, it simulates full RDMA functionality on **regular Ethernet NICs**.

```
┌─────────────────────────────────────────────────────┐
│              Real RDMA Hardware (Mellanox ConnectX)   │
│  ┌──────────┐    ┌────────────┐    ┌──────────────┐ │
│  │ Verbs API │ → │ Driver(mlx5)│ → │ HW NIC (HCA) │ │
│  └──────────┘    └────────────┘    └──────────────┘ │
│                                                     │
│  Latency: ~1-2 us    Bandwidth: 100-400 Gbps       │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│              SoftRoCE (Software Simulation)           │
│  ┌──────────┐    ┌───────────┐    ┌──────────────┐  │
│  │ Verbs API │ → │ rdma_rxe  │ → │ Regular NIC  │   │
│  └──────────┘    └───────────┘    └──────────────┘  │
│                                                     │
│  Latency: ~50-100 us    Bandwidth: depends on NIC (1-25 Gbps) │
└─────────────────────────────────────────────────────┘
```

### Why Use SoftRoCE for Learning?

| Advantage | Description |
|-----------|-------------|
| **Zero cost** | No need to purchase RDMA NICs and switches |
| **Identical API** | Same Verbs API code, fully consistent behavior |
| **Single-machine learning** | One laptop/VM is enough to complete all exercises |
| **Real validation** | All code runs on real hardware without modification |

### SoftRoCE vs Real Hardware Differences

| Item | SoftRoCE | Real RDMA Hardware |
|------|----------|-------------------|
| Latency | ~50-100 us (goes through kernel stack) | ~1-2 us (hardware direct) |
| Bandwidth | Limited by regular NIC | 100-400+ Gbps |
| CPU usage | High (software processing) | Low (hardware offload) |
| **LID** | **Always 0** | Assigned by SM in IB mode |
| **GID** | Generated from NIC IP address | Generated from HCA GUID |
| Link layer | Ethernet | IB or Ethernet |
| Kernel Bypass | No (still goes through kernel) | Yes (true kernel bypass) |
| **API behavior** | **Fully consistent** | **Fully consistent** |

> **Key point**: Although performance differs, the **API programming interface and behavior are fully consistent**.
> Code written on SoftRoCE can run directly on real hardware.

---

## 2. One-Click Setup

### Automatic Setup (Recommended)

```bash
# One-click SoftRoCE environment setup
sudo ./scripts/setup_softrce.sh
```

The script automatically completes: Install dependencies -> Load modules -> Create device -> Set ulimit -> Verify environment -> Build project

### Manual Setup

If the automatic script cannot run, follow these steps manually:

#### Step 1: Install Dependencies

```bash
sudo apt update
sudo apt install -y libibverbs-dev librdmacm-dev rdma-core \
                    ibverbs-utils perftest build-essential
```

#### Step 2: Load Kernel Module

```bash
# Load SoftRoCE module
sudo modprobe rdma_rxe

# Verify module is loaded
lsmod | grep rdma_rxe
```

#### Step 3: Create SoftRoCE Device

```bash
# View available network interfaces
ip link show

# Bind SoftRoCE to network interface (replace eth0 with your interface name)
sudo rdma link add rxe0 type rxe netdev eth0

# Verify device
rdma link show
ibv_devices
```

#### Step 4: Set Memory Locking

```bash
# Temporary setting (current shell)
ulimit -l unlimited

# Permanent setting (write to limits.conf, requires re-login)
sudo bash -c 'echo "* soft memlock unlimited" >> /etc/security/limits.conf'
sudo bash -c 'echo "* hard memlock unlimited" >> /etc/security/limits.conf'
```

#### Step 5: Verify

```bash
# View device information
ibv_devinfo

# Key fields in expected output:
#   link_layer: Ethernet    <- SoftRoCE uses Ethernet link layer
#   state:      PORT_ACTIVE <- Port should be in active state
#   port_lid:   0           <- LID is always 0 in RoCE mode (normal!)
```

---

## 3. SoftRoCE Key Concepts

### 3.1 LID = 0 is Normal

In SoftRoCE (RoCE mode):

```
$ ibv_devinfo | grep lid
    port_lid:  0      <- This is normal! RoCE does not use LID
    sm_lid:    0      <- No Subnet Manager (only IB needs one)
```

**InfiniBand** uses **LID (Local Identifier)** for subnet addressing (assigned by Subnet Manager).
**RoCE** uses **GID (Global Identifier)** for addressing (generated from IP address).

All programs in this tutorial automatically detect the transport type and choose the correct addressing method:

```c
// Automatic detection (built into all programs)
enum rdma_transport t = detect_transport(ctx, port);
if (t == RDMA_TRANSPORT_ROCE) {
    // Use GID addressing: ah_attr.is_global = 1, grh.dgid = remote_gid
} else {
    // Use LID addressing: ah_attr.dlid = remote_lid
}
```

### 3.2 GID is Generated from IP Address

Entries in SoftRoCE's GID table come from the NIC's IP address:

```bash
# View GID table
cat /sys/class/infiniband/rxe0/ports/1/gids/0

# Typical output: fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx (link-local IPv6)
# Or: 0000:0000:0000:0000:0000:ffff:c0a8:0164 (IPv4-mapped, i.e. 192.168.1.100)
```

**GID Index Selection:**

| GID Index | Type | Description |
|-----------|------|-------------|
| 0 | RoCE v1 (link-local) | Subnet-only communication |
| 1 | RoCE v2 (IPv4-mapped) | **Recommended** — supports IP routing |
| 2+ | RoCE v2 (IPv6) | If IPv6 is configured |

Programs use `gid_index = 0` by default. Adjust `RDMA_DEFAULT_GID_INDEX` if needed.

### 3.3 MTU Considerations

```bash
# View SoftRoCE MTU
ibv_devinfo | grep mtu
#   max_mtu:    4096
#   active_mtu: 1024    <- SoftRoCE default MTU
```

SoftRoCE's active_mtu is limited by the underlying NIC's MTU:
- Regular Ethernet MTU = 1500 -> RDMA active_mtu ~ 1024
- Jumbo Frame MTU = 9000 -> RDMA active_mtu ~ 4096

Programs in this tutorial use `IBV_MTU_1024` by default, compatible with all environments.

### 3.4 ulimit -l (Memory Locking Limit)

RDMA requires **pinned memory** (locking physical memory pages to prevent swapping),
which requires the system to allow processes to lock sufficient memory.

```bash
# View current limit
ulimit -l
# If the output is 64 or another small number, adjustment is needed

# Temporary adjustment
ulimit -l unlimited

# If you get "Operation not permitted", the hard limit also needs adjustment
# Edit /etc/security/limits.conf:
#   * soft memlock unlimited
#   * hard memlock unlimited
# Then re-login
```

---

## 4. Persistent Configuration

By default, SoftRoCE devices disappear after reboot. To make them persistent:

### Method 1: Auto-load Module at Boot

```bash
# Add to module auto-load list
echo "rdma_rxe" | sudo tee -a /etc/modules-load.d/rdma.conf

# Create udev rule or startup script to automatically create rxe0 device
sudo bash -c 'cat > /etc/systemd/system/softrce.service << EOF
[Unit]
Description=SoftRoCE Setup
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/rdma link add rxe0 type rxe netdev eth0
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF'

sudo systemctl enable softrce.service
```

### Method 2: Manual Creation Each Time

```bash
# Run after reboot
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0
```

---

## 5. Program Execution Quick Reference

### Single-Process Programs (Run directly, no peer needed)

| Program | Path | Description |
|---------|------|-------------|
| `hello_rdma` | ch09-quickref/ | Minimal RDMA Hello World |
| `01_init_resources` | ch03-verbs-api/01-initialization/ | Six-step initialization |
| `02_multi_device` | ch03-verbs-api/01-initialization/ | Device enumeration |
| `qp_state` | ch03-verbs-api/02-qp-state/ | QP state machine (loopback) |
| `01_qp_error_recovery` | ch03-verbs-api/02-qp-state/ | QP error recovery |
| `pd_isolation` | ch03-verbs-api/03-pd/ | PD isolation demo |
| `mr_access_flags` | ch03-verbs-api/04-mr/ | MR access flags |
| `mr_multi_reg` | ch03-verbs-api/04-mr/ | Multiple MR registration |
| `cq_event_driven` | ch03-verbs-api/05-cq/ | Event-driven CQ |
| `cq_overflow` | ch03-verbs-api/05-cq/ | CQ overflow |
| `01_loopback_send_recv` | ch05-communication/02-send-recv/ | Send/Recv loopback |
| `02_sge_demo` | ch05-communication/02-send-recv/ | Scatter-Gather |
| `ud_loopback` | ch06-connection/03-ud-mode/ | UD mode |
| `inline_data` | ch07-engineering/02-tuning/ | Inline optimization |
| `unsignaled_send` | ch07-engineering/02-tuning/ | Unsignaled send |
| `srq_demo` | ch07-engineering/02-tuning/ | Shared Receive Queue |
| `trigger_errors` | ch07-engineering/04-error-handling/ | Error triggering |
| `error_diagnosis` | ch07-engineering/04-error-handling/ | Error diagnosis |
| `error_cheatsheet` | ch09-quickref/ | Error quick reference |

How to run:
```bash
ulimit -l unlimited
./ch09-quickref/hello_rdma
```

### Dual-Process Programs (Two terminals needed, server + client)

| Program | Path | Terminal 1 (Server) | Terminal 2 (Client) |
|---------|------|---------------------|---------------------|
| `rdma_write` | ch05/.../01-rdma-write/ | `./rdma_write server` | `./rdma_write client 127.0.0.1` |
| `01_write_imm` | ch05/.../01-rdma-write/ | `./01_write_imm server` | `./01_write_imm client 127.0.0.1` |
| `send_recv` | ch05/.../02-send-recv/ | `./send_recv server` | `./send_recv client 127.0.0.1` |
| `03_rnr_error_demo` | ch05/.../02-send-recv/ | `./03_rnr_error_demo server` | `./03_rnr_error_demo client 127.0.0.1` |
| `rdma_read` | ch05/.../03-rdma-read/ | `./rdma_read server` | `./rdma_read client 127.0.0.1` |
| `01_batch_read` | ch05/.../03-rdma-read/ | `./01_batch_read server` | `./01_batch_read client 127.0.0.1` |
| `atomic_ops` | ch05/.../04-atomic/ | `./atomic_ops server` | `./atomic_ops client 127.0.0.1` |
| `01_alignment_error` | ch05/.../04-atomic/ | `./01_alignment_error server` | `./01_alignment_error client 127.0.0.1` |
| `02_spinlock` | ch05/.../04-atomic/ | `./02_spinlock server` | `./02_spinlock client 127.0.0.1` |
| `manual_connect` | ch06/.../01-manual-connect/ | `./manual_connect -s` | `./manual_connect -c 127.0.0.1` |
| `rdma_cm_example` | ch06/.../02-rdma-cm/ | `./rdma_cm_example -s 7471` | `./rdma_cm_example -c 127.0.0.1 7471` |

How to run:
```bash
# Both terminals need this
ulimit -l unlimited

# Terminal 1: Start server
./rdma_write server

# Terminal 2: Start client (connect to 127.0.0.1)
./rdma_write client 127.0.0.1
```

### Automated Testing

```bash
# Run all tests (automatically handles server/client)
./scripts/run_all_tests.sh
```

---

## 6. Common Troubleshooting

### Q1: `ibv_devices` shows no output

**Cause**: SoftRoCE not configured
```bash
# Check module
lsmod | grep rdma_rxe
# If no output:
sudo modprobe rdma_rxe

# Check device
rdma link show
# If no rxe device:
sudo rdma link add rxe0 type rxe netdev eth0
```

### Q2: `Cannot allocate memory` or `ibv_reg_mr failed`

**Cause**: Insufficient memory locking limit
```bash
ulimit -l
# If not unlimited:
ulimit -l unlimited

# Permanent fix:
sudo bash -c 'echo "* soft memlock unlimited" >> /etc/security/limits.conf'
sudo bash -c 'echo "* hard memlock unlimited" >> /etc/security/limits.conf'
# Re-login
```

### Q3: QP state transition failed `INIT->RTR failed`

**Cause**: GID configuration issue
```bash
# Check if GID table has valid entries
cat /sys/class/infiniband/rxe0/ports/1/gids/0
# If all zeros, the NIC has no IP address

# Solution: Ensure NIC has an IP
ip addr show
# If no IP, configure one
sudo ip addr add 192.168.1.100/24 dev eth0
```

### Q4: `ibv_devinfo` shows `state: PORT_DOWN`

**Cause**: Underlying NIC is not UP
```bash
# Check NIC status
ip link show eth0
# If DOWN:
sudo ip link set eth0 up
```

### Q5: Client cannot connect to server in dual-process test

**Cause**: TCP port conflict or server not started
```bash
# Check if port is in use
ss -tlnp | grep 9876

# Make sure to start server first, wait 1-2 seconds before starting client
# Some programs use different TCP ports:
#   rdma_write: 9876
#   send_recv:  9999
#   rdma_read:  8888
#   atomic_ops: 7777
```

### Q6: SoftRoCE performance is very low

**This is normal!** SoftRoCE uses the kernel software protocol stack, not true hardware bypass.

| Metric | SoftRoCE | Real Hardware |
|--------|----------|--------------|
| Latency | ~50-100 us | ~1-2 us |
| Bandwidth | ~1-10 Gbps | ~100-400 Gbps |
| CPU usage | High | Very low |

The value of SoftRoCE lies in **learning the API**, not testing performance.

### Q7: `rdma_cm_example` connection failed

**Cause**: rdma_cm requires network reachability
```bash
# Ensure 127.0.0.1 is available (loopback interface UP)
ping -c 1 127.0.0.1

# Ensure rdma_cm module is loaded
sudo modprobe rdma_cm
```

### Q8: `modprobe rdma_rxe` failed

**Cause**: Kernel does not support it or module not compiled
```bash
# Check kernel version (requires 4.8+)
uname -r

# Check if module exists
find /lib/modules/$(uname -r) -name "rdma_rxe*"

# If module does not exist, you may need to install:
sudo apt install linux-modules-extra-$(uname -r)
```

### Q9: Cannot use SoftRoCE in Docker/container

```bash
# Container requires additional privileges:
docker run --privileged --cap-add=IPC_LOCK ...

# Or configure SoftRoCE on the host and share with container:
docker run --device=/dev/infiniband/uverbs0 ...
```

### Q10: Build error `rdma/rdma_cma.h: No such file`

```bash
sudo apt install librdmacm-dev
```

---

## 7. One-Click Testing

After setting up the environment, verify all programs:

```bash
# Build
make all

# Run full test suite
./scripts/run_all_tests.sh
```

Expected output:
```
=================================================
   Test Summary
=================================================

  Total:   ~30 tests
  Passed:  ~30
  Failed:  0
  Skipped: 0

  All tests passed! SoftRoCE environment is fully functional.
```

---

## Next Step

Once the environment is ready, proceed to Chapter 1: [RDMA Basic Concepts](../ch01-intro/README.md)
