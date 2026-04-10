#!/bin/bash
#
# iWARP Environment Check Script
#
# Features:
#   - Check if iWARP devices exist
#   - Check iWARP drivers (iw_cxgb4, i40iw, irdma)
#   - Use rdma link / ibv_devinfo to display iWARP information
#   - Display iWARP-specific node type (RNIC)
#
# Usage: chmod +x iwarp_env_check.sh && ./iwarp_env_check.sh
#
set -e

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== Helper Functions ==========

pass() {
    echo -e "  [${GREEN}PASS${NC}] $1"
}

fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
}

warn() {
    echo -e "  [${YELLOW}WARN${NC}] $1"
}

info() {
    echo -e "  [${BLUE}INFO${NC}] $1"
}

separator() {
    echo -e "\n${CYAN}========== $1 ==========${NC}\n"
}

# ========== 1. Check iWARP driver modules ==========
separator "1. iWARP Driver Module Check"

iwarp_driver_found=false

# Chelsio T5/T6 driver
if lsmod 2>/dev/null | grep -q "iw_cxgb4"; then
    pass "iw_cxgb4 module loaded (Chelsio T5/T6 iWARP)"
    iwarp_driver_found=true
else
    info "iw_cxgb4 module not loaded (Chelsio iWARP driver)"
fi

# Intel X722 driver (legacy)
if lsmod 2>/dev/null | grep -q "i40iw"; then
    pass "i40iw module loaded (Intel X722 iWARP)"
    iwarp_driver_found=true
else
    info "i40iw module not loaded (Intel X722 driver)"
fi

# Intel irdma driver (new, supports both RoCE and iWARP)
if lsmod 2>/dev/null | grep -q "irdma"; then
    pass "irdma module loaded (Intel E810/next-gen RDMA)"
    iwarp_driver_found=true
else
    info "irdma module not loaded (Intel next-gen RDMA driver)"
fi

if ! $iwarp_driver_found; then
    warn "No iWARP driver modules detected"
    info "iWARP requires specific hardware support (Chelsio T5/T6 or Intel X722/E810)"
    info "Note: SoftRoCE (rxe) is not iWARP, it is a software implementation of RoCE"
fi

# ========== 2. Check iWARP devices (via node_type) ==========
separator "2. iWARP Device Detection"

iwarp_found=false

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")
        node_type_file="$dev_dir/node_type"

        if [ -f "$node_type_file" ]; then
            node_type=$(cat "$node_type_file" 2>/dev/null || echo "")
            echo -e "  Device ${BOLD}${dev_name}${NC}: node_type = ${node_type}"

            # iWARP device node_type contains "RNIC"
            if echo "$node_type" | grep -qi "RNIC"; then
                pass "${dev_name} is an iWARP device (node_type=RNIC)"
                iwarp_found=true
            else
                info "${dev_name} is not iWARP (node_type=${node_type})"
            fi
        fi
    done

    if ! $iwarp_found; then
        info "No iWARP devices found (IBV_NODE_RNIC)"
        info "Most cloud environments use RoCE; iWARP devices are less common"
    fi
else
    fail "/sys/class/infiniband/ does not exist"
fi

# ========== 3. Display info using rdma link ==========
separator "3. RDMA Link Information"

if command -v rdma &>/dev/null; then
    output=$(rdma link 2>&1) || true
    if [ -n "$output" ]; then
        pass "rdma link output:"
        echo "$output" | while IFS= read -r line; do
            echo -e "    $line"
        done
    else
        warn "rdma link produced no output"
    fi
else
    warn "rdma command not available (please install iproute2)"
fi

# ========== 4. ibv_devinfo detailed information ==========
separator "4. Device Detailed Information (ibv_devinfo)"

if command -v ibv_devinfo &>/dev/null; then
    output=$(ibv_devinfo 2>&1) || true
    if [ -n "$output" ]; then
        echo "$output" | while IFS= read -r line; do
            # Highlight transport and node_type fields
            if echo "$line" | grep -qi "transport\|node_type"; then
                echo -e "    ${BOLD}${line}${NC}"
            else
                echo -e "    $line"
            fi
        done
    fi
else
    warn "ibv_devinfo command not available"
fi

# ========== Summary ==========
separator "Check Complete"

if $iwarp_found; then
    pass "iWARP device found, iWARP programming is available"
    info "iWARP recommends using RDMA CM (rdma_cma.h) for connection establishment"
else
    info "No iWARP devices found"
    info "Key differences between iWARP and IB/RoCE:"
    echo -e "    - node_type: ${YELLOW}IBV_NODE_RNIC${NC} (not IBV_NODE_CA)"
    echo -e "    - Underlying transport: ${YELLOW}TCP${NC} (not IB native or UDP)"
    echo -e "    - Connection method: Recommend using ${YELLOW}RDMA CM${NC} (because underlying is TCP stream)"
    echo -e "    - Lossless network: ${GREEN}Not required${NC} (TCP has built-in retransmission)"
fi
