#!/bin/bash
#
# strace RDMA Program Analysis Script
#
# Features:
#   - Uses strace to trace system calls of RDMA programs
#   - Auto-annotates RDMA-related system call meanings
#   - Distinguishes which operations go through kernel vs user space
#
# Usage:
#   ./strace_verbs.sh <program_path> [program_args...]
#
# Examples:
#   ./strace_verbs.sh ./inline_data
#   ./strace_verbs.sh /usr/bin/ibv_devinfo
#
# Notes:
#   RDMA (libibverbs) is designed for "kernel bypass".
#   But resource creation/destruction still goes through kernel (via ioctl),
#   while data transfer (post_send/post_recv/poll_cq) is pure user-space.
#
#   System calls traced by this script:
#   - open/openat: Open device files
#   - mmap/munmap: Map NIC registers to user space
#   - ioctl:       Kernel operations (create QP/CQ/MR, etc.)
#   - close:       Close file descriptors
#   - write:       Possible event notifications
#

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== Argument Check ==========
if [ $# -lt 1 ]; then
    echo -e "${BOLD}Usage: $0 <program_path> [program_args...]${NC}"
    echo ""
    echo "Examples:"
    echo "  $0 ./inline_data"
    echo "  $0 /usr/bin/ibv_devinfo"
    exit 1
fi

PROG="$1"
shift
PROG_ARGS="$@"

# Check if program exists
if [ ! -f "$PROG" ] && ! command -v "$PROG" &>/dev/null; then
    echo -e "${RED}[Error]${NC} Program '$PROG' does not exist"
    exit 1
fi

# Check if strace is installed
if ! command -v strace &>/dev/null; then
    echo -e "${RED}[Error]${NC} strace is not installed"
    echo "  Install: sudo apt-get install strace"
    exit 1
fi

# ========== Run strace ==========
echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║   strace RDMA Program System Call Analysis   ║${NC}"
echo -e "${BOLD}${CYAN}╚═══════════════════════════════════════╝${NC}"
echo ""
echo -e "${BLUE}[Info]${NC} Target program: $PROG $PROG_ARGS"
echo -e "${BLUE}[Info]${NC} Tracing calls: open, openat, close, mmap, munmap, ioctl, write"
echo ""

# Run strace and save output
STRACE_OUT=$(mktemp /tmp/strace_rdma_XXXXXX.log)
trap "rm -f $STRACE_OUT" EXIT

strace -e trace=open,openat,close,mmap,munmap,ioctl,write \
       -f -tt "$PROG" $PROG_ARGS 2>"$STRACE_OUT"

# ========== Analyze and Annotate Output ==========
echo ""
echo -e "${BOLD}${CYAN}━━━ RDMA-Related System Call Analysis ━━━${NC}"
echo ""

# Statistics counters
UVERBS_OPEN=0
MMAP_COUNT=0
IOCTL_COUNT=0

while IFS= read -r line; do
    # Filter and annotate RDMA-related calls

    # /dev/infiniband/uverbs* -> Opening Verbs device file
    if echo "$line" | grep -q '/dev/infiniband/uverbs'; then
        echo -e "${GREEN}[Verbs Device]${NC} $line"
        echo -e "    ${YELLOW}-> Opening Verbs device file (entry point for user-space Verbs)${NC}"
        UVERBS_OPEN=$((UVERBS_OPEN + 1))

    # /dev/infiniband/rdma_cm -> Opening RDMA CM device
    elif echo "$line" | grep -q '/dev/infiniband/rdma_cm'; then
        echo -e "${GREEN}[RDMA CM]${NC} $line"
        echo -e "    ${YELLOW}-> Opening RDMA CM device (connection management)${NC}"

    # mmap call -> Mapping NIC registers/memory
    elif echo "$line" | grep -q "^.*mmap("; then
        echo -e "${BLUE}[Memory Map]${NC} $line"
        echo -e "    ${YELLOW}-> Mapping NIC registers/DoorBell/buffers to user space${NC}"
        echo -e "    ${YELLOW}  (This is key to kernel bypass: data path no longer goes through kernel)${NC}"
        MMAP_COUNT=$((MMAP_COUNT + 1))

    # ioctl call -> Kernel operation
    elif echo "$line" | grep -q "^.*ioctl("; then
        echo -e "${RED}[Kernel Op]${NC} $line"
        echo -e "    ${YELLOW}-> Kernel operation (resource creation: PD/MR/CQ/QP, or state modification)${NC}"
        IOCTL_COUNT=$((IOCTL_COUNT + 1))

    # /sys/class/infiniband -> Querying device sysfs info
    elif echo "$line" | grep -q '/sys/class/infiniband'; then
        echo -e "${CYAN}[Device Query]${NC} $line"
        echo -e "    ${YELLOW}-> Querying device/port attributes via sysfs${NC}"
    fi

done < "$STRACE_OUT"

# ========== Statistics Summary ==========
echo ""
echo -e "${BOLD}${CYAN}━━━ System Call Statistics ━━━${NC}"
echo ""
echo -e "  Verbs device open count:  ${GREEN}$UVERBS_OPEN${NC}"
echo -e "  mmap mapping count:       ${BLUE}$MMAP_COUNT${NC}"
echo -e "  ioctl (kernel ops):       ${RED}$IOCTL_COUNT${NC}"
echo ""

echo -e "${BOLD}━━━ RDMA Data Path Explanation ━━━${NC}"
echo ""
echo "  ┌─────────────────────────────────────────────────────────┐"
echo "  │ Kernel operations (via ioctl):                          │"
echo "  │   - ibv_open_device()    Open device                    │"
echo "  │   - ibv_alloc_pd()       Create protection domain       │"
echo "  │   - ibv_reg_mr()         Register memory region         │"
echo "  │   - ibv_create_cq()      Create completion queue        │"
echo "  │   - ibv_create_qp()      Create queue pair              │"
echo "  │   - ibv_modify_qp()      Modify QP state                │"
echo "  │   - ibv_destroy_*()      Destroy resources              │"
echo "  ├─────────────────────────────────────────────────────────┤"
echo "  │ User-space operations (pure CPU, no kernel):            │"
echo "  │   - ibv_post_send()      Post send request (write DoorBell) │"
echo "  │   - ibv_post_recv()      Post recv request (write DoorBell) │"
echo "  │   - ibv_poll_cq()        Poll completion queue (read shared memory) │"
echo "  │   These operations will NOT appear in strace!           │"
echo "  └─────────────────────────────────────────────────────────┘"
echo ""
echo -e "${GREEN}Tip: The actual data transfer (post_send/recv/poll_cq) is pure user-space,${NC}"
echo -e "${GREEN}     strace cannot trace them. This is the core of RDMA 'kernel bypass'.${NC}"
echo ""
