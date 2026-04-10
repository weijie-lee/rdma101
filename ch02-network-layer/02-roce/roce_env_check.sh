#!/bin/bash
#
# RoCE (RDMA over Converged Ethernet) Environment Check Script
#
# Features:
#   - Display GID table contents (sysfs)
#   - Check PFC (Priority Flow Control) configuration
#   - Check ECN (Explicit Congestion Notification) configuration
#   - Display RoCE-specific settings (GID type, DSCP, etc.)
#   - Check SoftRoCE (rxe) module status
#
# Usage: chmod +x roce_env_check.sh && ./roce_env_check.sh
#
set -e

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'  # No Color

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

# ========== 1. Check RDMA devices and link layer type ==========
separator "1. RoCE Device Detection"

roce_devices=()

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")
            link_layer=$(cat "$port_dir/link_layer" 2>/dev/null || echo "")

            if [ "$link_layer" = "Ethernet" ]; then
                pass "RoCE device found: ${dev_name} port ${port_num} (link_layer=Ethernet)"
                roce_devices+=("${dev_name}:${port_num}")
            elif [ "$link_layer" = "InfiniBand" ]; then
                info "Device ${dev_name} port ${port_num} is InfiniBand (not RoCE)"
            fi
        done
    done

    if [ ${#roce_devices[@]} -eq 0 ]; then
        fail "No RoCE devices found"
        info "To create a SoftRoCE device, run:"
        echo -e "       ${YELLOW}sudo rdma link add rxe_0 type rxe netdev eth0${NC}"
    fi
else
    fail "/sys/class/infiniband/ does not exist"
fi

# ========== 2. GID table contents ==========
separator "2. GID Table Contents"

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")
            link_layer=$(cat "$port_dir/link_layer" 2>/dev/null || echo "")

            # Only show GIDs for Ethernet (RoCE) ports
            [ "$link_layer" = "Ethernet" ] || continue

            echo -e "  ${BOLD}${dev_name} / Port ${port_num} GID Table:${NC}"

            gid_dir="$port_dir/gids"
            gid_type_dir="$port_dir/gid_attrs/types"
            gid_count=0

            for gid_file in "$gid_dir"/*; do
                [ -f "$gid_file" ] || continue
                gid_index=$(basename "$gid_file")
                gid_value=$(cat "$gid_file" 2>/dev/null || echo "")

                # Skip all-zero GIDs
                if [ -n "$gid_value" ] && [ "$gid_value" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
                    # Try to read GID type (RoCE v1 / RoCE v2)
                    gid_type="N/A"
                    if [ -f "$gid_type_dir/$gid_index" ]; then
                        gid_type=$(cat "$gid_type_dir/$gid_index" 2>/dev/null || echo "N/A")
                    fi

                    # Color based on GID type
                    if echo "$gid_type" | grep -qi "v2\|rocev2"; then
                        echo -e "    GID[${gid_index}] = ${GREEN}${gid_value}${NC}  Type: ${GREEN}${gid_type}${NC}  <- Recommended for RoCE v2"
                    else
                        echo -e "    GID[${gid_index}] = ${YELLOW}${gid_value}${NC}  Type: ${YELLOW}${gid_type}${NC}"
                    fi
                    gid_count=$((gid_count + 1))
                fi
            done

            if [ "$gid_count" -eq 0 ]; then
                warn "No valid GIDs (port may not have an IP address configured)"
            else
                info "Total ${gid_count} valid GID(s)"
            fi
            echo ""
        done
    done
fi

# ========== 3. PFC (Priority Flow Control) configuration ==========
separator "3. PFC (Priority Flow Control) Check"

# Method 1: Use mlnx_qos (Mellanox specific)
if command -v mlnx_qos &>/dev/null; then
    pass "mlnx_qos tool available"
    info "Run 'mlnx_qos -i <netdev>' to view PFC configuration"
    # Try to get the network interface corresponding to the first RDMA NIC
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")
        netdev_file="$dev_dir/device/net"
        if [ -d "$netdev_file" ]; then
            for netdev in "$netdev_file"/*/; do
                netdev_name=$(basename "$netdev")
                info "Device ${dev_name} associated NIC: ${netdev_name}"
                # Don't run mlnx_qos automatically to avoid permission issues
                echo -e "       ${YELLOW}Run: sudo mlnx_qos -i ${netdev_name}${NC}"
            done
        fi
    done
else
    info "mlnx_qos not available (not a Mellanox device or mlnx-tools not installed)"
fi

# Method 2: Use lldptool
if command -v lldptool &>/dev/null; then
    pass "lldptool tool available"
    info "Run 'lldptool -t -i <netdev> -V PFC' to view PFC status"
else
    info "lldptool not available (lldpad package not installed)"
fi

# Method 3: Check DCB (Data Center Bridging) info in sysfs
info "Checking DCB/PFC sysfs information..."
pfc_found=false
for net_dir in /sys/class/net/*/; do
    [ -d "$net_dir" ] || continue
    netdev_name=$(basename "$net_dir")
    dcb_dir="$net_dir/ieee8021/dcb"
    if [ -d "$dcb_dir" ]; then
        pass "NIC ${netdev_name} supports DCB"
        pfc_found=true
    fi
done
if ! $pfc_found; then
    info "No DCB configuration detected (SoftRoCE environment does not require PFC)"
fi

# ========== 4. ECN (Explicit Congestion Notification) ==========
separator "4. ECN (Explicit Congestion Notification) Check"

# Check TCP ECN setting
if [ -f /proc/sys/net/ipv4/tcp_ecn ]; then
    ecn_value=$(cat /proc/sys/net/ipv4/tcp_ecn 2>/dev/null)
    case "$ecn_value" in
        0) warn "tcp_ecn = 0 (ECN disabled)" ;;
        1) pass "tcp_ecn = 1 (ECN enabled, requested when initiating connections)" ;;
        2) pass "tcp_ecn = 2 (ECN enabled, only responds as server)" ;;
        *) info "tcp_ecn = $ecn_value" ;;
    esac
else
    warn "Unable to read tcp_ecn setting"
fi

# Check RoCE ECN related parameters (Mellanox devices)
for dev_dir in /sys/class/infiniband/*/; do
    [ -d "$dev_dir" ] || continue
    dev_name=$(basename "$dev_dir")
    ecn_dir="$dev_dir/ecn"
    if [ -d "$ecn_dir" ]; then
        pass "Device ${dev_name} supports ECN configuration"
        for ecn_file in "$ecn_dir"/*; do
            [ -f "$ecn_file" ] || continue
            ecn_param=$(basename "$ecn_file")
            ecn_val=$(cat "$ecn_file" 2>/dev/null || echo "N/A")
            echo -e "    ${ecn_param} = ${ecn_val}"
        done
    fi
done

# ========== 5. RoCE specific settings ==========
separator "5. RoCE Related Kernel Modules"

# Check SoftRoCE (rxe) module
if lsmod 2>/dev/null | grep -q "rdma_rxe"; then
    pass "rdma_rxe module loaded (SoftRoCE)"
elif lsmod 2>/dev/null | grep -q "rxe"; then
    pass "rxe related module loaded"
else
    info "rdma_rxe module not loaded (for SoftRoCE: sudo modprobe rdma_rxe)"
fi

# Check Mellanox driver
if lsmod 2>/dev/null | grep -q "mlx5_ib"; then
    pass "mlx5_ib module loaded (Mellanox ConnectX)"
fi

if lsmod 2>/dev/null | grep -q "mlx4_ib"; then
    pass "mlx4_ib module loaded (Mellanox ConnectX-3)"
fi

# Check RoCE mode setting (Mellanox devices)
for dev_dir in /sys/class/infiniband/*/; do
    [ -d "$dev_dir" ] || continue
    dev_name=$(basename "$dev_dir")

    for port_dir in "$dev_dir"/ports/*/; do
        [ -d "$port_dir" ] || continue
        port_num=$(basename "$port_dir")

        # Check RoCE version (if supported)
        roce_type_file="$port_dir/gid_attrs/types/0"
        if [ -f "$roce_type_file" ]; then
            default_type=$(cat "$roce_type_file" 2>/dev/null || echo "N/A")
            info "Device ${dev_name} port ${port_num} default GID[0] type: ${default_type}"
        fi
    done
done

# ========== 6. Network interface status ==========
separator "6. Associated Network Interfaces"

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")

        # Try to find associated network interface
        netdev_path="$dev_dir/device/net"
        if [ -d "$netdev_path" ]; then
            for netdev in "$netdev_path"/*/; do
                [ -d "$netdev" ] || continue
                netdev_name=$(basename "$netdev")
                echo -e "  ${BOLD}${dev_name} -> ${netdev_name}${NC}"

                # Display IP address
                if command -v ip &>/dev/null; then
                    ip_info=$(ip addr show "$netdev_name" 2>/dev/null | grep "inet " || true)
                    if [ -n "$ip_info" ]; then
                        echo -e "    IP: ${GREEN}$(echo "$ip_info" | awk '{print $2}')${NC}"
                    else
                        warn "  ${netdev_name} has no IPv4 address (GID table may be incomplete)"
                    fi
                fi

                # Display MTU
                mtu=$(cat "/sys/class/net/${netdev_name}/mtu" 2>/dev/null || echo "N/A")
                echo -e "    MTU: ${mtu}"

                # Display link status
                operstate=$(cat "/sys/class/net/${netdev_name}/operstate" 2>/dev/null || echo "N/A")
                if [ "$operstate" = "up" ]; then
                    echo -e "    Status: ${GREEN}${operstate}${NC}"
                else
                    echo -e "    Status: ${RED}${operstate}${NC}"
                fi
            done
        else
            info "Device ${dev_name} has no associated network interface (may be a virtual device)"
        fi
    done
fi

# ========== Summary ==========
separator "Check Complete"
info "RoCE environment check finished"
info "RoCE v2 recommendations:"
echo -e "    1. Choose the GID index of RoCE v2 type (usually 1 or 3)"
echo -e "    2. Ensure the network interface has an IP address"
echo -e "    3. For production environments, configure PFC + ECN"
info "Run the C program roce_gid_query for more detailed GID analysis"
