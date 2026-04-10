#!/bin/bash
# =============================================================================
# GPUDirect RDMA Environment Check Script
#
# Function: Check all environment conditions required for GPUDirect RDMA
#           Including GPU driver, kernel modules, CUDA version, RDMA devices, etc.
#
# Usage: bash gpudirect_check.sh
#
# Check results for each item:
#   [OK]   - Condition met
#   [WARN] - Condition partially met, may affect functionality
#   [FAIL] - Condition not met, GPUDirect will not work
# =============================================================================

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No color

# ========== Counters ==========
PASS=0      # Number of passed checks
WARN=0      # Number of warning checks
FAIL=0      # Number of failed checks
TOTAL=0     # Total number of checks

# ========== Helper Functions ==========

# Print header
print_header() {
    echo ""
    echo -e "${CYAN}=============================================${NC}"
    echo -e "${CYAN}    GPUDirect RDMA Environment Check${NC}"
    echo -e "${CYAN}=============================================${NC}"
    echo ""
}

# Print check result: OK
print_ok() {
    echo -e "  [${GREEN} OK ${NC}] $1"
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
}

# Print check result: WARN
print_warn() {
    echo -e "  [${YELLOW}WARN${NC}] $1"
    WARN=$((WARN + 1))
    TOTAL=$((TOTAL + 1))
}

# Print check result: FAIL
print_fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
}

# Print info message
print_info() {
    echo -e "         ${BLUE}↳ $1${NC}"
}

# ========== Check Items ==========

print_header

# --- Check 1: Is nvidia-smi available ---
echo -e "${CYAN}[Check 1] NVIDIA GPU Driver${NC}"
if command -v nvidia-smi &>/dev/null; then
    # nvidia-smi exists, try running it
    GPU_INFO=$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null)
    if [ $? -eq 0 ]; then
        print_ok "nvidia-smi is available"
        # Print GPU info line by line
        echo "$GPU_INFO" | while IFS= read -r line; do
            print_info "GPU: $line"
        done
    else
        print_fail "nvidia-smi exists but cannot query GPU"
        print_info "Possible cause: GPU driver not loaded correctly, run dmesg | grep -i nvidia to check"
    fi
else
    print_fail "nvidia-smi not found"
    print_info "Please install NVIDIA driver: https://www.nvidia.com/drivers"
fi

echo ""

# --- Check 2: nvidia_peermem / nv_peer_mem kernel module ---
echo -e "${CYAN}[Check 2] PeerMem Kernel Module (GPU VRAM <-> RDMA NIC direct access)${NC}"
if lsmod 2>/dev/null | grep -q "nvidia_peermem"; then
    # Built-in module from CUDA 11.4+
    print_ok "nvidia_peermem module loaded (CUDA built-in version)"
    print_info "This is the recommended way, CUDA 11.4+ automatically includes this module"
elif lsmod 2>/dev/null | grep -q "nv_peer_mem"; then
    # Legacy module provided by Mellanox
    print_ok "nv_peer_mem module loaded (Mellanox standalone version)"
    print_info "Consider upgrading to CUDA 11.4+ to use the built-in nvidia_peermem"
else
    print_fail "PeerMem module not loaded"
    print_info "nvidia_peermem: modprobe nvidia_peermem (CUDA 11.4+)"
    print_info "nv_peer_mem:    requires separate installation https://github.com/Mellanox/nv_peer_memory"
    print_info "Without this module, ibv_reg_mr() cannot register GPU memory"
fi

echo ""

# --- Check 3: CUDA version ---
echo -e "${CYAN}[Check 3] CUDA Toolkit${NC}"
if command -v nvcc &>/dev/null; then
    CUDA_VER=$(nvcc --version 2>/dev/null | grep "release" | awk '{print $6}' | tr -d ',')
    print_ok "CUDA Toolkit installed: $CUDA_VER"

    # Check if version >= 11.4 (nvidia_peermem built-in version)
    MAJOR=$(echo "$CUDA_VER" | cut -d. -f1 | tr -d 'V')
    MINOR=$(echo "$CUDA_VER" | cut -d. -f2)
    if [ "$MAJOR" -gt 11 ] || ([ "$MAJOR" -eq 11 ] && [ "$MINOR" -ge 4 ]); then
        print_info "CUDA >= 11.4, supports built-in nvidia_peermem"
    else
        print_warn "CUDA < 11.4, requires separate nv_peer_mem module installation"
    fi
else
    # nvcc not in PATH, check common install paths
    if [ -d "/usr/local/cuda" ]; then
        print_warn "CUDA directory exists (/usr/local/cuda) but nvcc is not in PATH"
        print_info "Run: export PATH=/usr/local/cuda/bin:\$PATH"
    else
        print_fail "CUDA Toolkit not installed"
        print_info "GPUDirect RDMA requires CUDA, version 11.4+ recommended"
        print_info "Install: https://developer.nvidia.com/cuda-downloads"
    fi
fi

echo ""

# --- Check 4: /sys/kernel/mm/memory_peers/ ---
echo -e "${CYAN}[Check 4] Memory Peers Kernel Interface${NC}"
if [ -d "/sys/kernel/mm/memory_peers/" ]; then
    print_ok "/sys/kernel/mm/memory_peers/ exists"
    # List registered peer memory clients
    PEERS=$(ls /sys/kernel/mm/memory_peers/ 2>/dev/null)
    if [ -n "$PEERS" ]; then
        print_info "Registered peer memory clients: $PEERS"
    fi
else
    print_warn "/sys/kernel/mm/memory_peers/ does not exist"
    print_info "This directory appears after the PeerMem module is loaded"
    print_info "Some kernel versions may use a different path"
fi

echo ""

# --- Check 5: RDMA devices ---
echo -e "${CYAN}[Check 5] RDMA Devices${NC}"
if command -v ibv_devinfo &>/dev/null; then
    # Check for active RDMA devices
    ACTIVE_PORTS=$(ibv_devinfo 2>/dev/null | grep -c "state:.*PORT_ACTIVE")
    DEVICE_COUNT=$(ibv_devinfo -l 2>/dev/null | grep -c "^\s")

    if [ "$ACTIVE_PORTS" -gt 0 ]; then
        print_ok "RDMA devices available, $ACTIVE_PORTS port(s) in ACTIVE state"
        # Show device names and port status
        ibv_devinfo 2>/dev/null | grep -E "hca_id|port:|state:" | while IFS= read -r line; do
            print_info "$(echo "$line" | xargs)"
        done
    elif [ "$DEVICE_COUNT" -gt 0 ]; then
        print_warn "RDMA devices found but no ACTIVE ports"
        print_info "Check network connection and port configuration"
    else
        print_fail "No RDMA devices found"
        print_info "GPUDirect RDMA requires an RDMA-capable NIC (e.g., Mellanox ConnectX)"
    fi
else
    print_fail "ibv_devinfo not installed"
    print_info "Install: apt install ibverbs-utils or yum install libibverbs-utils"
fi

echo ""

# --- Check 6: PCIe topology (GPU and NIC affinity) ---
echo -e "${CYAN}[Check 6] PCIe Topology (GPU-NIC Affinity)${NC}"
if command -v nvidia-smi &>/dev/null && command -v lspci &>/dev/null; then
    # Get GPU PCIe address
    GPU_PCI=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader 2>/dev/null | head -1)
    if [ -n "$GPU_PCI" ]; then
        print_ok "GPU PCIe address: $GPU_PCI"
        # Get Mellanox NIC PCIe address
        NIC_PCI=$(lspci 2>/dev/null | grep -i "mellanox\|infiniband" | head -1 | awk '{print $1}')
        if [ -n "$NIC_PCI" ]; then
            print_info "NIC PCIe address: $NIC_PCI"
            print_info "Best performance: GPU and NIC should be under the same PCIe switch / NUMA node"
            print_info "View detailed topology: nvidia-smi topo -m"
        else
            print_warn "No Mellanox/InfiniBand NIC found"
        fi
    fi
else
    print_warn "Cannot check PCIe topology (requires nvidia-smi and lspci)"
fi

echo ""

# ========== Summary ==========
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}    Check Results Summary${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""
echo -e "  ${GREEN}Passed: $PASS${NC}  ${YELLOW}Warnings: $WARN${NC}  ${RED}Failed: $FAIL${NC}  Total: $TOTAL"
echo ""

if [ "$FAIL" -eq 0 ] && [ "$WARN" -eq 0 ]; then
    echo -e "  ${GREEN}✓ GPUDirect RDMA environment is fully ready!${NC}"
elif [ "$FAIL" -eq 0 ]; then
    echo -e "  ${YELLOW}△ GPUDirect RDMA is basically usable, but there are warnings to address${NC}"
else
    echo -e "  ${RED}✗ GPUDirect RDMA environment is not ready, please resolve the FAIL items above${NC}"
fi

echo ""
echo -e "${BLUE}Note: GPUDirect RDMA requires all of the following:${NC}"
echo -e "${BLUE}  1. NVIDIA GPU + driver${NC}"
echo -e "${BLUE}  2. nvidia_peermem or nv_peer_mem kernel module${NC}"
echo -e "${BLUE}  3. CUDA Toolkit${NC}"
echo -e "${BLUE}  4. RDMA-capable NIC (e.g., Mellanox ConnectX-4+)${NC}"
echo -e "${BLUE}  5. GPU and NIC under the same PCIe topology (recommended)${NC}"
echo ""
