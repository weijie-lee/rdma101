#!/bin/bash
# =============================================================================
# RDMA Userspace Driver Trace Script
#
# Function: Use strace to trace system calls of RDMA programs,
#           revealing how the userspace driver (libibverbs) works
#
# Usage:
#   bash userspace_driver_trace.sh              # Default: trace ibv_devices
#   bash userspace_driver_trace.sh ibv_devinfo  # Trace specified program
#   bash userspace_driver_trace.sh ./my_rdma_program  # Trace custom program
#
# Key findings:
#   - ibv_open_device -> open("/dev/infiniband/uverbs0")
#   - ibv_create_qp  -> ioctl + mmap (maps QP to userspace)
#   - ibv_post_send  -> No system call! (pure userspace write to mmap'd memory)
#   - ibv_poll_cq    -> No system call! (pure userspace read from mmap'd memory)
# =============================================================================

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== Argument Handling ==========
# Default: trace ibv_devices
TARGET="${1:-ibv_devices}"

# Check if strace is installed
if ! command -v strace &>/dev/null; then
    echo -e "${RED}[Error] strace is not installed${NC}"
    echo "  Install: apt install strace or yum install strace"
    exit 1
fi

# Check if target program exists
if ! command -v "$TARGET" &>/dev/null && [ ! -f "$TARGET" ]; then
    echo -e "${RED}[Error] Program does not exist: $TARGET${NC}"
    exit 1
fi

echo ""
echo -e "${CYAN}======================================================${NC}"
echo -e "${CYAN}  RDMA Userspace Driver Trace${NC}"
echo -e "${CYAN}  Trace target: ${YELLOW}$TARGET${NC}"
echo -e "${CYAN}======================================================${NC}"
echo ""

# ========== Trace 1: uverbs device file opening ==========
echo -e "${BOLD}[Trace 1] Device File Opening (open/openat /dev/infiniband/*)${NC}"
echo -e "${BLUE}# First step of an RDMA program: open the uverbs character device${NC}"
echo -e "${BLUE}# ibv_open_device() -> kernel open(\"/dev/infiniband/uverbs0\")${NC}"
echo -e "${BLUE}# uverbs0 is the kernel interface for userspace Verbs${NC}"
echo ""

strace -f -e trace=open,openat "$TARGET" 2>&1 | \
    grep -E "infiniband|uverbs|rdma" | head -20 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""
echo -e "  ${YELLOW}Note: /dev/infiniband/uverbs* is the RDMA userspace entry point${NC}"
echo -e "  ${YELLOW}      Each HCA device corresponds to one uverbs character device${NC}"
echo ""

# ========== Trace 2: mmap calls ==========
echo -e "${BOLD}[Trace 2] mmap Calls (mapping HCA resources to userspace)${NC}"
echo -e "${BLUE}# After ibv_create_qp/cq, kernel maps the following to user space via mmap:${NC}"
echo -e "${BLUE}#   - Send Queue / Recv Queue memory -> userspace can directly write WQEs${NC}"
echo -e "${BLUE}#   - Completion Queue memory -> userspace can directly read CQEs${NC}"
echo -e "${BLUE}#   - Doorbell register (MMIO) -> userspace directly notifies HCA${NC}"
echo ""

strace -f -e trace=mmap "$TARGET" 2>&1 | \
    grep -v "PROT_NONE\|No such\|ENOENT" | head -20 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""
echo -e "  ${YELLOW}Note: mmap is the key to the userspace driver -- once shared memory${NC}"
echo -e "  ${YELLOW}      mappings are established, ibv_post_send/ibv_poll_cq no longer need system calls${NC}"
echo ""

# ========== Trace 3: ioctl calls ==========
echo -e "${BOLD}[Trace 3] ioctl Calls (kernel Verbs operations)${NC}"
echo -e "${BLUE}# Resource creation/modification is done via ioctl (control path, not data path):${NC}"
echo -e "${BLUE}#   ibv_alloc_pd   -> ioctl(fd, IB_USER_VERBS_CMD_ALLOC_PD, ...)${NC}"
echo -e "${BLUE}#   ibv_reg_mr     -> ioctl(fd, IB_USER_VERBS_CMD_REG_MR, ...)${NC}"
echo -e "${BLUE}#   ibv_create_cq  -> ioctl(fd, IB_USER_VERBS_CMD_CREATE_CQ, ...)${NC}"
echo -e "${BLUE}#   ibv_create_qp  -> ioctl(fd, IB_USER_VERBS_CMD_CREATE_QP, ...)${NC}"
echo -e "${BLUE}#   ibv_modify_qp  -> ioctl(fd, IB_USER_VERBS_CMD_MODIFY_QP, ...)${NC}"
echo ""

strace -f -e trace=ioctl "$TARGET" 2>&1 | \
    grep -v "TCGETS\|TIOCGWINSZ" | head -20 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""
echo -e "  ${YELLOW}Note: ioctl is only called during resource creation/destruction (control path)${NC}"
echo -e "  ${YELLOW}      Data path (post_send/poll_cq) does not use ioctl${NC}"
echo ""

# ========== Trace 4: write calls (Doorbell) ==========
echo -e "${BOLD}[Trace 4] write Calls (some drivers use write instead of MMIO doorbell)${NC}"
echo -e "${BLUE}# Some older drivers trigger doorbell via write() system call${NC}"
echo -e "${BLUE}# Modern mlx5 driver uses mmap'd MMIO doorbell (BlueFlame), no write()${NC}"
echo ""

strace -f -e trace=write "$TARGET" 2>&1 | \
    grep -v "EBADF\|pipe\|stdout\|stderr" | head -10 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""

# ========== Combined trace: system call statistics ==========
echo -e "${BOLD}[Summary] System Call Statistics${NC}"
echo -e "${BLUE}# Statistics of system calls during program execution${NC}"
echo -e "${BLUE}# Note: ibv_post_send and ibv_poll_cq will NOT appear here!${NC}"
echo ""

strace -f -c "$TARGET" 2>&1 | tail -30 | \
    while IFS= read -r line; do
        echo -e "  $line"
    done

echo ""

# ========== Principle Summary ==========
echo -e "${CYAN}======================================================${NC}"
echo -e "${CYAN}  RDMA Userspace Driver Principle Summary${NC}"
echo -e "${CYAN}======================================================${NC}"
echo ""
echo -e "  ${BOLD}Call chain:${NC}"
echo ""
echo -e "  Application"
echo -e "    |"
echo -e "    v"
echo -e "  libibverbs.so (generic Verbs interface)"
echo -e "    |"
echo -e "    v"
echo -e "  libmlx5.so / libhfi1.so (vendor driver library, provider)"
echo -e "    |"
echo -e "    +-- Control path: open() + ioctl() + mmap() -> kernel involved"
echo -e "    |"
echo -e "    +-- Data path: directly write mmap'd memory -> ${GREEN}pure userspace, zero system calls${NC}"
echo -e "          |"
echo -e "          v"
echo -e "        HCA hardware (reads data via DMA and sends it)"
echo ""
echo -e "  ${YELLOW}Key insights:${NC}"
echo -e "  ${YELLOW}  1. ibv_post_send() = write WQE to mmap'd SQ + write doorbell register${NC}"
echo -e "  ${YELLOW}  2. ibv_poll_cq()   = read mmap'd CQ memory${NC}"
echo -e "  ${YELLOW}  3. Data path is entirely in userspace, latency ~1us (vs TCP's ~10us)${NC}"
echo -e "  ${YELLOW}  4. This is why RDMA is called \"kernel bypass\"${NC}"
echo ""
