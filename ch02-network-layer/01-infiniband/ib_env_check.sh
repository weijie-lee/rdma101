#!/bin/bash
#
# IB (InfiniBand) Environment Check Script
#
# Features:
#   - Check if IB-related commands exist (ibstat, ibv_devinfo, sminfo)
#   - List all RDMA devices
#   - Display detailed device information
#   - Check Subnet Manager status
#   - Display port state, LID, speed
#   - Display GID table contents
#
# Usage: chmod +x ib_env_check.sh && ./ib_env_check.sh
#
set -e

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'  # No Color (reset to default)

# ========== Helper Functions ==========

# Print pass message
pass() {
    echo -e "  [${GREEN}PASS${NC}] $1"
}

# Print fail message
fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
}

# Print warning message
warn() {
    echo -e "  [${YELLOW}WARN${NC}] $1"
}

# Print info message
info() {
    echo -e "  [${BLUE}INFO${NC}] $1"
}

# Print separator line
separator() {
    echo -e "\n${CYAN}========== $1 ==========${NC}\n"
}

# ========== 1. Check if commands exist ==========
separator "1. Check IB Tool Commands"

# Check ibstat command
if command -v ibstat &>/dev/null; then
    pass "ibstat command exists: $(which ibstat)"
else
    warn "ibstat command not found (infiniband-diags package not installed)"
fi

# Check ibv_devinfo command
if command -v ibv_devinfo &>/dev/null; then
    pass "ibv_devinfo command exists: $(which ibv_devinfo)"
else
    fail "ibv_devinfo command not found (please install libibverbs-utils or rdma-core)"
fi

# Check sminfo command
if command -v sminfo &>/dev/null; then
    pass "sminfo command exists: $(which sminfo)"
else
    warn "sminfo command not found (infiniband-diags package not installed, only needed for IB networks)"
fi

# Check ibv_devices command
if command -v ibv_devices &>/dev/null; then
    pass "ibv_devices command exists: $(which ibv_devices)"
else
    fail "ibv_devices command not found"
fi

# Check rdma command (iproute2)
if command -v rdma &>/dev/null; then
    pass "rdma command exists: $(which rdma)"
else
    warn "rdma command not found (please install iproute2)"
fi

# ========== 2. List all RDMA devices ==========
separator "2. RDMA Device List (ibv_devices)"

if command -v ibv_devices &>/dev/null; then
    output=$(ibv_devices 2>&1) || true
    if echo "$output" | grep -q "device"; then
        pass "RDMA devices found:"
        echo "$output" | while IFS= read -r line; do
            echo -e "       $line"
        done
    else
        fail "No RDMA devices found"
        echo -e "       ${YELLOW}Hint: Please verify RDMA drivers are loaded (modprobe rdma_rxe / mlx5_ib)${NC}"
    fi
else
    fail "Unable to execute ibv_devices"
fi

# ========== 3. Device detailed information ==========
separator "3. Device Detailed Information (ibv_devinfo)"

if command -v ibv_devinfo &>/dev/null; then
    output=$(ibv_devinfo 2>&1) || true
    if [ -n "$output" ]; then
        pass "Device detailed information:"
        echo "$output" | while IFS= read -r line; do
            # Highlight key fields
            if echo "$line" | grep -q "transport:"; then
                echo -e "       ${BOLD}$line${NC}"
            elif echo "$line" | grep -q "state:"; then
                if echo "$line" | grep -q "PORT_ACTIVE"; then
                    echo -e "       ${GREEN}$line${NC}"
                else
                    echo -e "       ${RED}$line${NC}"
                fi
            elif echo "$line" | grep -q "link_layer:"; then
                echo -e "       ${CYAN}$line${NC}"
            else
                echo -e "       $line"
            fi
        done
    else
        fail "ibv_devinfo produced no output"
    fi
else
    fail "Unable to execute ibv_devinfo"
fi

# ========== 4. Subnet Manager status check ==========
separator "4. Subnet Manager (SM) Status"

sm_found=false

# Method 1: Use sminfo
if command -v sminfo &>/dev/null; then
    info "Attempting to query SM status using sminfo..."
    output=$(sminfo 2>&1) || true
    if echo "$output" | grep -q "SM"; then
        pass "Subnet Manager information:"
        echo -e "       $output"
        sm_found=true
    else
        warn "sminfo query failed (may not be an IB network): $output"
    fi
fi

# Method 2: Use ibstat
if ! $sm_found && command -v ibstat &>/dev/null; then
    info "Attempting to query using ibstat..."
    output=$(ibstat 2>&1) || true
    if [ -n "$output" ]; then
        pass "ibstat output:"
        echo "$output" | while IFS= read -r line; do
            echo -e "       $line"
        done
    else
        warn "ibstat produced no output"
    fi
fi

if ! $sm_found; then
    info "SM is only needed for InfiniBand networks; RoCE/iWARP do not require SM"
fi

# ========== 5. Port state, LID, speed ==========
separator "5. Port Status Details"

# Traverse all devices and ports under /sys/class/infiniband/
if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        dev_name=$(basename "$dev_dir")
        echo -e "  ${BOLD}Device: $dev_name${NC}"

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")

            # Read port status
            state=$(cat "$port_dir/state" 2>/dev/null || echo "unknown")
            lid=$(cat "$port_dir/lid" 2>/dev/null || echo "N/A")
            sm_lid=$(cat "$port_dir/sm_lid" 2>/dev/null || echo "N/A")
            rate=$(cat "$port_dir/rate" 2>/dev/null || echo "N/A")
            link_layer=$(cat "$port_dir/link_layer" 2>/dev/null || echo "N/A")
            phys_state=$(cat "$port_dir/phys_state" 2>/dev/null || echo "N/A")

            echo -e "    Port ${port_num}:"

            # State check
            if echo "$state" | grep -q "ACTIVE"; then
                echo -e "      State:       ${GREEN}${state}${NC}"
            else
                echo -e "      State:       ${RED}${state}${NC}"
            fi

            echo -e "      Phys State:  ${phys_state}"
            echo -e "      LID:         ${lid}"
            echo -e "      SM LID:      ${sm_lid}"
            echo -e "      Speed:       ${rate}"
            echo -e "      Link Layer:  ${CYAN}${link_layer}${NC}"
        done
        echo ""
    done
else
    fail "/sys/class/infiniband/ directory does not exist, RDMA driver not loaded"
fi

# ========== 6. GID table contents ==========
separator "6. GID Table Contents"

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        dev_name=$(basename "$dev_dir")

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")
            gid_dir="$port_dir/gids"

            [ -d "$gid_dir" ] || continue

            echo -e "  ${BOLD}${dev_name} / Port ${port_num} GID Table:${NC}"

            gid_count=0
            for gid_file in "$gid_dir"/*; do
                [ -f "$gid_file" ] || continue
                gid_index=$(basename "$gid_file")
                gid_value=$(cat "$gid_file" 2>/dev/null || echo "")

                # Skip all-zero GIDs
                if [ -n "$gid_value" ] && [ "$gid_value" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
                    echo -e "    GID[${gid_index}] = ${GREEN}${gid_value}${NC}"
                    gid_count=$((gid_count + 1))
                fi
            done

            if [ "$gid_count" -eq 0 ]; then
                warn "Port ${port_num} has no valid GID entries"
            else
                info "Port ${port_num} has ${gid_count} valid GID(s)"
            fi
            echo ""
        done
    done
else
    fail "Unable to read GID table"
fi

# ========== Summary ==========
separator "Check Complete"
info "This script checked the basic status of the InfiniBand environment"
info "If you are using a RoCE environment, please run ../02-roce/roce_env_check.sh"
info "If you are using an iWARP environment, please run ../03-iwarp/iwarp_env_check.sh"
