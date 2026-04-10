#!/bin/bash
# =============================================================================
# NCCL Environment Check Script
#
# Function: Check NCCL installation status, environment variable configuration,
#           and provide common command examples
#
# NCCL (NVIDIA Collective Communications Library) is the core communication
# library for AI distributed training. It can use RDMA (IB Verbs) as the
# underlying transport for high-speed inter-node communication.
#
# Usage: bash nccl_env_check.sh
# =============================================================================

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ========== Helper Functions ==========
print_ok()   { echo -e "  [${GREEN} OK ${NC}] $1"; }
print_warn() { echo -e "  [${YELLOW}WARN${NC}] $1"; }
print_fail() { echo -e "  [${RED}FAIL${NC}] $1"; }
print_info() { echo -e "         ${BLUE}↳ $1${NC}"; }

echo ""
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}    NCCL Environment Check${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""

# --- Check 1: NCCL library installation ---
echo -e "${CYAN}[Check 1] NCCL Library Installation${NC}"
NCCL_LIB=""
# Method 1: Search via ldconfig
NCCL_LIB=$(ldconfig -p 2>/dev/null | grep "libnccl.so" | head -1 | awk '{print $NF}')
if [ -z "$NCCL_LIB" ]; then
    # Method 2: Search common paths
    for dir in /usr/lib /usr/lib64 /usr/local/lib /usr/local/cuda/lib64; do
        if [ -f "$dir/libnccl.so" ] || [ -f "$dir/libnccl.so.2" ]; then
            NCCL_LIB="$dir/libnccl.so"
            break
        fi
    done
fi

if [ -n "$NCCL_LIB" ]; then
    print_ok "NCCL library installed: $NCCL_LIB"
else
    print_fail "NCCL library not found"
    print_info "Install: apt install libnccl2 libnccl-dev or download from NVIDIA website"
fi

# --- Check 2: NCCL header file ---
echo ""
echo -e "${CYAN}[Check 2] NCCL Header File${NC}"
NCCL_HEADER=""
for dir in /usr/include /usr/local/include /usr/local/cuda/include; do
    if [ -f "$dir/nccl.h" ]; then
        NCCL_HEADER="$dir/nccl.h"
        break
    fi
done

if [ -n "$NCCL_HEADER" ]; then
    print_ok "nccl.h found: $NCCL_HEADER"
    # Extract NCCL version
    NCCL_MAJOR=$(grep "NCCL_MAJOR" "$NCCL_HEADER" 2>/dev/null | head -1 | awk '{print $3}')
    NCCL_MINOR=$(grep "NCCL_MINOR" "$NCCL_HEADER" 2>/dev/null | head -1 | awk '{print $3}')
    NCCL_PATCH=$(grep "NCCL_PATCH" "$NCCL_HEADER" 2>/dev/null | head -1 | awk '{print $3}')
    if [ -n "$NCCL_MAJOR" ]; then
        print_info "NCCL version: ${NCCL_MAJOR}.${NCCL_MINOR}.${NCCL_PATCH}"
    fi
else
    print_warn "nccl.h not found"
    print_info "Install: apt install libnccl-dev"
fi

# --- Check 3: nccl-tests ---
echo ""
echo -e "${CYAN}[Check 3] nccl-tests (Performance Testing Tool)${NC}"
if command -v all_reduce_perf &>/dev/null; then
    print_ok "all_reduce_perf installed: $(which all_reduce_perf)"
elif [ -f "/usr/local/bin/all_reduce_perf" ]; then
    print_ok "all_reduce_perf: /usr/local/bin/all_reduce_perf"
else
    print_warn "nccl-tests not installed"
    print_info "Installation:"
    print_info "  git clone https://github.com/NVIDIA/nccl-tests.git"
    print_info "  cd nccl-tests && make MPI=1 MPI_HOME=/usr/local/mpi"
fi

# --- Check 4: Key environment variables ---
echo ""
echo -e "${CYAN}[Check 4] NCCL Key Environment Variables${NC}"
echo ""

# Define environment variables and their descriptions
declare -A ENV_DESC
ENV_DESC=(
    ["NCCL_IB_DISABLE"]="Whether to disable InfiniBand (0=enable RDMA, 1=disable fallback to TCP)"
    ["NCCL_IB_HCA"]="Specify HCA device name to use (e.g., mlx5_0, mlx5_1)"
    ["NCCL_IB_GID_INDEX"]="RoCE GID index (RoCE v2 typically uses 3)"
    ["NCCL_NET_GDR_LEVEL"]="GPUDirect RDMA level (0=disabled, 5=cross-node)"
    ["NCCL_DEBUG"]="Debug output level (WARN/INFO/TRACE)"
    ["NCCL_DEBUG_SUBSYS"]="Debug subsystem filter (NET/INIT/COLL/ALL)"
    ["NCCL_SOCKET_IFNAME"]="Network interface for TCP control plane (e.g., eth0)"
)

# Check each environment variable in order
for var in NCCL_IB_DISABLE NCCL_IB_HCA NCCL_IB_GID_INDEX \
           NCCL_NET_GDR_LEVEL NCCL_DEBUG NCCL_DEBUG_SUBSYS \
           NCCL_SOCKET_IFNAME; do
    VALUE="${!var}"    # Indirect reference to get variable value
    DESC="${ENV_DESC[$var]}"
    if [ -n "$VALUE" ]; then
        echo -e "  ${GREEN}$var${NC} = ${YELLOW}$VALUE${NC}"
    else
        echo -e "  ${BLUE}$var${NC} = (not set)"
    fi
    echo -e "    ${BLUE}# $DESC${NC}"
done

# --- Common command examples ---
echo ""
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}    Common NCCL Command Examples${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""

echo -e "${YELLOW}# 1. Enable NCCL debug output (view RDMA communication details)${NC}"
echo "NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=NET python train.py"
echo ""

echo -e "${YELLOW}# 2. PyTorch distributed init + NCCL debug${NC}"
echo 'NCCL_DEBUG=INFO python -c "'
echo '  import torch'
echo '  import torch.distributed as dist'
echo '  dist.init_process_group(backend=\"nccl\")'
echo '  print(f\"Rank {dist.get_rank()} initialized\")'
echo '"'
echo ""

echo -e "${YELLOW}# 3. nccl-tests performance test (single node, multi-GPU)${NC}"
echo "all_reduce_perf -b 1M -e 1G -f 2 -g 4"
echo "  # -b: starting message size (1MB)"
echo "  # -e: ending message size (1GB)"
echo "  # -f: size increment factor"
echo "  # -g: number of GPUs per node"
echo ""

echo -e "${YELLOW}# 4. nccl-tests multi-node test (launched via MPI)${NC}"
echo "mpirun -np 8 --host node1:4,node2:4 \\"
echo "  -x NCCL_IB_HCA=mlx5_0 \\"
echo "  -x NCCL_NET_GDR_LEVEL=5 \\"
echo "  all_reduce_perf -b 1M -e 1G -f 2 -g 1"
echo ""

echo -e "${YELLOW}# 5. Force RDMA usage (eliminate TCP interference)${NC}"
echo "NCCL_IB_DISABLE=0 NCCL_SOCKET_IFNAME=eth0 NCCL_IB_HCA=mlx5_0 python train.py"
echo ""

echo -e "${YELLOW}# 6. Force TCP usage (fallback when RDMA environment has issues)${NC}"
echo "NCCL_IB_DISABLE=1 NCCL_SOCKET_IFNAME=eth0 python train.py"
echo ""

echo -e "${BLUE}Tip: NCCL automatically detects and selects the optimal transport.${NC}"
echo -e "${BLUE}     Environment variables only need to be set manually when auto-detection is incorrect.${NC}"
echo ""
