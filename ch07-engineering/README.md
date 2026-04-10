# Chapter 7: RDMA Engineering Practice

**Performance Testing, Tuning, Debugging, and Error Handling**

---

## Overview

In previous chapters, we learned the core concepts and communication modes of RDMA. This chapter focuses on **engineering practice** --
in real projects, you need to know how to measure performance, optimize throughput/latency, troubleshoot issues, and handle errors gracefully.

This chapter contains four modules:

| Module | Directory | Content |
|--------|-----------|---------|
| Performance Testing | `01-perftest/` | Measure RDMA performance using the perftest tool suite |
| Performance Tuning | `02-tuning/` | Three key optimizations: Inline Data, Unsignaled Send, SRQ |
| Debug & Diagnostics | `03-debug/` | One-click environment diagnostics, strace analysis of Verbs calls |
| Error Handling | `04-error-handling/` | Deliberately trigger errors + error code diagnostic manual |

---

## 01-perftest: Performance Testing

### Why perftest?

perftest is the standard performance testing tool in the RDMA community. It can measure:

- **Latency**: Time from sending a message to receiving a response (us)
- **Bandwidth**: Amount of data transferred per unit time (Gb/s)
- **Message Rate**: Number of messages processed per second (Mpps)

### File Description

| File | Description |
|------|-------------|
| `run_perftest.sh` | Automated perftest benchmark script |

### Quick Start

```bash
# Run all benchmarks (loopback mode)
./run_perftest.sh

# Specify device
./run_perftest.sh rxe0
```

### Key Metrics Explained

```
BW (Gb/s)   = Actual bandwidth (gigabits/second)
BW (MB/s)   = Actual bandwidth (megabytes/second), 1 Gb/s ~ 125 MB/s
Latency(us) = Half-round-trip latency (microseconds)
MsgRate     = Messages per second (millions)
```

---

## 02-tuning: Performance Tuning

### Three Core Optimization Techniques

#### 1. Inline Data

**Principle**: Embed small messages directly in the WQE (Work Queue Element), skipping the DMA read step.

```
Normal path: CPU -> WQE(pointer) -> NIC DMA reads memory -> NIC sends
Inline:      CPU -> WQE(with data) -> NIC sends directly (saves one DMA)
```

**Applicable scenario**: Messages < 256 bytes (depends on hardware max_inline_data)

**Note**: Inline send does not need lkey, because data is directly in the WQE.

#### 2. Unsignaled Completion

**Principle**: Not every Send operation produces a CQE, reducing CQ polling overhead.

```
Signaled:   Each Send -> produces CQE -> must poll_cq
Unsignaled: Every N Sends -> only the Nth produces CQE -> poll once
```

**Key constraints**:
- Must signal at least once before the SQ is full (otherwise SQ will overflow)
- Must poll before the CQ is full (otherwise CQ will overflow)
- Recommended: signal once every min(max_send_wr/2, 32) requests

#### 3. Shared Receive Queue (SRQ)

**Principle**: Multiple QPs share one receive queue, reducing total recv buffer requirements.

```
Without SRQ: Each QP needs independent recv buffers (N QPs x M buffers)
With SRQ:    All QPs share one recv pool (1 pool x M buffers)
```

**Applicable scenario**: Large number of QP connections (e.g., thousands of client connections in a database)

### File Description

| File | Description |
|------|-------------|
| `inline_data.c` | Inline Data optimization comparison experiment |
| `unsignaled_send.c` | Unsignaled Completion optimization comparison experiment |
| `srq_demo.c` | Shared Receive Queue demo |
| `Makefile` | Compile the above three programs |

### Build and Run

```bash
cd 02-tuning
make
./inline_data
./unsignaled_send
./srq_demo
```

---

## 03-debug: Debug & Diagnostics

### Challenges of RDMA Debugging

1. **Kernel bypass**: Most operations bypass the kernel, invisible to strace/ltrace
2. **Asynchronous errors**: Errors may be reported in WC or async events
3. **Hardware dependency**: Issues may be in the driver, firmware, cable, or switch
4. **Complex state machine**: QP state errors don't fail immediately, but cause failures in subsequent operations

### Diagnostic Tool Chain

```
Environment check:  rdma_diag.sh      -> One-click check of all RDMA configuration
System calls:       strace_verbs.sh   -> Trace Verbs system calls
Device info:        ibv_devinfo        -> View device details
Port status:        ibv_devinfo -v     -> View port status
Packet capture:     tcpdump / Wireshark -> Capture RoCE packets
Kernel logs:        dmesg              -> View driver-level errors
```

### File Description

| File | Description |
|------|-------------|
| `rdma_diag.sh` | One-click RDMA environment diagnostics (modules/devices/ports/GID/quotas) |
| `strace_verbs.sh` | Trace RDMA program system calls with annotated explanations |

### Quick Start

```bash
# One-click diagnostics
./rdma_diag.sh

# Trace Verbs calls
./strace_verbs.sh ./your_rdma_program
```

---

## 04-error-handling: Error Handling

### RDMA Error Types

| Type | Source | Description |
|------|--------|-------------|
| WC errors | `ibv_poll_cq` | Operation completed but failed (most common) |
| Async events | `ibv_get_async_event` | Device/port/QP level asynchronous errors |
| Verbs return values | API call returns | Parameter errors, insufficient resources |

### Common WC Error Codes

| Error Code | Meaning | Common Cause |
|------------|---------|--------------|
| `IBV_WC_LOC_LEN_ERR` | Local length error | recv buffer too small |
| `IBV_WC_LOC_PROT_ERR` | Local protection error | lkey error / PD mismatch |
| `IBV_WC_REM_ACCESS_ERR` | Remote access error | rkey error / insufficient permissions |
| `IBV_WC_RNR_RETRY_EXC_ERR` | RNR retry exceeded | Remote peer has no post recv |
| `IBV_WC_WR_FLUSH_ERR` | WR flush error | QP has entered ERROR state |
| `IBV_WC_RETRY_EXC_ERR` | Retry exceeded | Remote unreachable / QP state error |

### QP Error Recovery

```
Normal: RESET -> INIT -> RTR -> RTS
Error:  RTS -> ERROR (automatic transition)
Recovery: ERROR -> RESET -> INIT -> RTR -> RTS (go through the whole process again)
```

### File Description

| File | Description |
|------|-------------|
| `trigger_errors.c` | Deliberately trigger 4 typical RDMA errors and demonstrate recovery |
| `error_diagnosis.c` | RDMA error code diagnostic manual (interactive query) |
| `Makefile` | Compile the above two programs |

### Build and Run

```bash
cd 04-error-handling
make

# Trigger and observe various errors
./trigger_errors

# View all error code descriptions
./error_diagnosis

# View a specific error code
./error_diagnosis 5   # IBV_WC_REM_ACCESS_ERR
```

---

## Suggested Learning Path

```
1. Run perftest first to establish performance baseline  -> 01-perftest/
2. Apply tuning techniques to improve performance        -> 02-tuning/
3. Use diagnostic tools to troubleshoot issues           -> 03-debug/
4. Understand error codes, write robust error handling   -> 04-error-handling/
```

---

## Build Dependencies

```bash
# Basic build tools
sudo apt-get install gcc make

# RDMA development libraries
sudo apt-get install libibverbs-dev librdmacm-dev

# perftest tools
sudo apt-get install perftest

# strace (for debugging)
sudo apt-get install strace
```

---

## References

- [perftest GitHub](https://github.com/linux-rdma/perftest)
- [RDMA Aware Programming User Manual](https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17)
- [rdma-core Kernel Documentation](https://www.kernel.org/doc/Documentation/infiniband/)
- [SoftRoCE (RXE) Wiki](https://github.com/linux-rdma/rdma-core/blob/master/Documentation/rxe.md)
