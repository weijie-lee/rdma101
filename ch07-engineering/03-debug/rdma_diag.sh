#!/bin/bash
#
# RDMA One-Click Environment Diagnostic Script
#
# Features:
#   - Check RDMA kernel module loading status
#   - Check RDMA device and port info
#   - Check rdma tool output
#   - Check SoftRoCE (RXE) configuration
#   - Check device files, memory lock limits
#   - Check RDMA-related dmesg logs
#   - Check perftest tool availability
#   - Generate diagnostic summary report
#
# Usage: ./rdma_diag.sh
#

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== Counters ==========
PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

# ========== Helper Functions ==========

# Print check results
pass_msg() {
    echo -e "  ${GREEN}[OK]${NC}   $*"
    PASS_COUNT=$((PASS_COUNT + 1))
}

warn_msg() {
    echo -e "  ${YELLOW}[WARN]${NC} $*"
    WARN_COUNT=$((WARN_COUNT + 1))
}

fail_msg() {
    echo -e "  ${RED}[FAIL]${NC} $*"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

info_msg() {
    echo -e "  ${BLUE}[Info]${NC} $*"
}

# Print section title
section() {
    echo ""
    echo -e "${BOLD}${CYAN}━━━ $* ━━━${NC}"
}

# Check if a kernel module is loaded
check_module() {
    local mod=$1
    local desc=$2
    if lsmod 2>/dev/null | grep -qw "$mod"; then
        pass_msg "$mod ($desc) - loaded"
    else
        warn_msg "$mod ($desc) - not loaded"
    fi
}

# Check if a command exists
check_command() {
    local cmd=$1
    local desc=$2
    if command -v "$cmd" &>/dev/null; then
        pass_msg "$cmd ($desc) - installed"
        return 0
    else
        warn_msg "$cmd ($desc) - not installed"
        return 1
    fi
}

# ========== Check Items ==========

# (a) Check kernel modules
check_kernel_modules() {
    section "Checking RDMA Kernel Modules"

    check_module "ib_core"    "RDMA core module"
    check_module "ib_uverbs"  "User-space Verbs interface"
    check_module "rdma_cm"    "RDMA Connection Manager"
    check_module "rdma_rxe"   "SoftRoCE (software RoCE emulation)"
    check_module "mlx5_ib"    "Mellanox ConnectX-5/6 driver"
    check_module "mlx4_ib"    "Mellanox ConnectX-3/4 driver"
    check_module "irdma"      "Intel RDMA driver"

    # Extra: check rdma_ucm
    check_module "rdma_ucm"   "RDMA CM user-space interface"
}

# (b) Check ibv_devices / ibv_devinfo
check_verbs_devices() {
    section "Checking RDMA Devices (ibv_devices / ibv_devinfo)"

    if ! command -v ibv_devices &>/dev/null; then
        fail_msg "ibv_devices command not found (please install libibverbs-utils or rdma-core)"
        return
    fi

    local dev_output
    dev_output=$(ibv_devices 2>&1)
    local dev_count
    dev_count=$(echo "$dev_output" | awk 'NR>2 && NF>0' | wc -l)

    if [ "$dev_count" -gt 0 ]; then
        pass_msg "Detected $dev_count RDMA device(s)"
        echo "$dev_output" | awk 'NR>2 && NF>0 {print "         " $0}'
    else
        fail_msg "No RDMA devices detected"
        info_msg "Tip: To use SoftRoCE, run: rdma link add rxe0 type rxe netdev <interface_name>"
    fi

    # Print ibv_devinfo summary
    if [ "$dev_count" -gt 0 ] && command -v ibv_devinfo &>/dev/null; then
        echo ""
        info_msg "Device details (ibv_devinfo):"
        ibv_devinfo 2>/dev/null | head -30 | sed 's/^/         /'
    fi
}

# (c) Check rdma link / rdma dev
check_rdma_tool() {
    section "Checking rdma Tool Output"

    if ! command -v rdma &>/dev/null; then
        warn_msg "rdma command not found (please install iproute2-rdma or iproute2)"
        return
    fi

    # rdma dev
    info_msg "rdma dev output:"
    local rdma_dev
    rdma_dev=$(rdma dev 2>&1)
    if [ -n "$rdma_dev" ]; then
        echo "$rdma_dev" | sed 's/^/         /'
        pass_msg "rdma dev command works"
    else
        warn_msg "rdma dev has no output"
    fi

    # rdma link
    echo ""
    info_msg "rdma link output:"
    local rdma_link
    rdma_link=$(rdma link 2>&1)
    if [ -n "$rdma_link" ]; then
        echo "$rdma_link" | sed 's/^/         /'
        pass_msg "rdma link command works"
    else
        warn_msg "rdma link has no output"
    fi
}

# (d) Check port state
check_port_state() {
    section "Checking Port State"

    if ! command -v ibv_devinfo &>/dev/null; then
        fail_msg "ibv_devinfo not available, cannot check port state"
        return
    fi

    # Parse port state from ibv_devinfo
    local port_info
    port_info=$(ibv_devinfo 2>/dev/null | grep -E "(hca_id|port:|state:)")

    if [ -z "$port_info" ]; then
        warn_msg "Unable to get port info"
        return
    fi

    # Check line by line
    local current_dev=""
    while IFS= read -r line; do
        if echo "$line" | grep -q "hca_id:"; then
            current_dev=$(echo "$line" | awk '{print $2}')
        fi
        if echo "$line" | grep -q "state:"; then
            local state
            state=$(echo "$line" | awk '{print $2}')
            if [ "$state" = "PORT_ACTIVE" ]; then
                pass_msg "$current_dev port state: ${GREEN}ACTIVE${NC}"
            elif [ "$state" = "PORT_DOWN" ]; then
                fail_msg "$current_dev port state: ${RED}DOWN${NC}"
                info_msg "  Port DOWN possible causes: cable not connected / switch port disabled / driver issue"
            else
                warn_msg "$current_dev port state: $state"
            fi
        fi
    done <<< "$port_info"
}

# (e) Check LID / GID table
check_lid_gid() {
    section "Checking LID / GID Info"

    if ! command -v ibv_devinfo &>/dev/null; then
        fail_msg "ibv_devinfo not available"
        return
    fi

    # LID
    local lid_info
    lid_info=$(ibv_devinfo 2>/dev/null | grep "lid:")
    if [ -n "$lid_info" ]; then
        local lid_val
        lid_val=$(echo "$lid_info" | head -1 | awk '{print $2}')
        if [ "$lid_val" = "0" ] || [ "$lid_val" = "0x0000" ]; then
            info_msg "LID = 0 (this is normal for RoCE mode, RoCE uses GID addressing)"
        else
            pass_msg "LID = $lid_val (IB mode, SM has assigned LID)"
        fi
    fi

    # GID[0]
    local gid_info
    gid_info=$(ibv_devinfo -v 2>/dev/null | grep "GID\[" | head -5)
    if [ -n "$gid_info" ]; then
        pass_msg "GID table (first 5 entries):"
        echo "$gid_info" | sed 's/^/         /'
    else
        warn_msg "Unable to get GID table info"
    fi
}

# (f) Check SoftRoCE configuration
check_softrce() {
    section "Checking SoftRoCE (RXE) Configuration"

    # Check rdma_rxe module
    if lsmod 2>/dev/null | grep -qw "rdma_rxe"; then
        pass_msg "rdma_rxe module loaded"
    else
        info_msg "rdma_rxe module not loaded (for SoftRoCE: modprobe rdma_rxe)"
    fi

    # Check for rxe devices
    if command -v rdma &>/dev/null; then
        local rxe_devs
        rxe_devs=$(rdma link 2>/dev/null | grep -i "rxe")
        if [ -n "$rxe_devs" ]; then
            pass_msg "SoftRoCE device(s) detected:"
            echo "$rxe_devs" | sed 's/^/         /'
        else
            info_msg "No SoftRoCE devices detected"
            info_msg "To create: rdma link add rxe0 type rxe netdev <interface_name>"
        fi
    fi
}

# (g) Check device files
check_device_files() {
    section "Checking /dev/infiniband/ Device Files"

    if [ -d /dev/infiniband ]; then
        local files
        files=$(ls /dev/infiniband/ 2>/dev/null)
        if [ -n "$files" ]; then
            pass_msg "/dev/infiniband/ directory exists"
            for f in $files; do
                local perms
                perms=$(ls -la "/dev/infiniband/$f" 2>/dev/null | awk '{print $1}')
                info_msg "  $f ($perms)"
            done

            # Check uverbs devices
            if ls /dev/infiniband/uverbs* &>/dev/null; then
                pass_msg "uverbs device files exist (user-space Verbs available)"
            else
                fail_msg "No uverbs device files found"
            fi
        else
            fail_msg "/dev/infiniband/ directory is empty"
        fi
    else
        fail_msg "/dev/infiniband/ directory does not exist"
        info_msg "Tip: you may need to load ib_uverbs module: modprobe ib_uverbs"
    fi
}

# (h) Check memory lock limit
check_memlock() {
    section "Checking Memory Lock Limit (ulimit -l)"

    local memlock
    memlock=$(ulimit -l 2>/dev/null)

    if [ "$memlock" = "unlimited" ]; then
        pass_msg "memlock = unlimited (best configuration)"
    elif [ -n "$memlock" ] && [ "$memlock" -ge 65536 ] 2>/dev/null; then
        pass_msg "memlock = ${memlock} KB (sufficient for most RDMA programs)"
    elif [ -n "$memlock" ]; then
        warn_msg "memlock = ${memlock} KB (may be insufficient)"
        info_msg "RDMA needs to pin memory, recommend setting unlimited:"
        info_msg "  ulimit -l unlimited"
        info_msg "  Or edit /etc/security/limits.conf and add:"
        info_msg "  * soft memlock unlimited"
        info_msg "  * hard memlock unlimited"
    else
        warn_msg "Unable to get memlock limit"
    fi
}

# (i) Check RDMA messages in dmesg
check_dmesg() {
    section "Checking dmesg RDMA-Related Logs"

    local dmesg_rdma
    dmesg_rdma=$(dmesg 2>/dev/null | grep -iE "(rdma|infiniband|ib_|rxe|mlx[45]|roce)" | tail -15)

    if [ -n "$dmesg_rdma" ]; then
        info_msg "Recent RDMA-related kernel logs:"
        echo "$dmesg_rdma" | sed 's/^/         /'

        # Check for errors
        if echo "$dmesg_rdma" | grep -qiE "(error|fail|warn)"; then
            warn_msg "RDMA-related error/warning messages found in dmesg"
        else
            pass_msg "No RDMA error messages in dmesg"
        fi
    else
        info_msg "No RDMA-related logs found in dmesg (may need root privileges)"
    fi
}

# (j) Check perftest tools
check_perftest() {
    section "Checking perftest Tools"

    local tools=(ib_send_lat ib_send_bw ib_write_lat ib_write_bw ib_read_lat ib_read_bw)
    local installed=0
    local total=${#tools[@]}

    for tool in "${tools[@]}"; do
        if command -v "$tool" &>/dev/null; then
            installed=$((installed + 1))
        fi
    done

    if [ $installed -eq $total ]; then
        pass_msg "perftest tools fully installed ($installed/$total)"
    elif [ $installed -gt 0 ]; then
        warn_msg "perftest tools partially installed ($installed/$total)"
    else
        warn_msg "perftest tools not installed (recommended: apt install perftest)"
    fi
}

# ========== Summary Report ==========
print_summary() {
    echo ""
    echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║      RDMA Environment Diagnostic Summary     ║${NC}"
    echo -e "${BOLD}${CYAN}╚═══════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  ${GREEN}Passed: $PASS_COUNT${NC}"
    echo -e "  ${YELLOW}Warnings: $WARN_COUNT${NC}"
    echo -e "  ${RED}Failed: $FAIL_COUNT${NC}"
    echo ""

    local total=$((PASS_COUNT + WARN_COUNT + FAIL_COUNT))
    if [ $FAIL_COUNT -eq 0 ] && [ $WARN_COUNT -eq 0 ]; then
        echo -e "  ${GREEN}${BOLD}Diagnostic result: RDMA environment is fully functional!${NC}"
    elif [ $FAIL_COUNT -eq 0 ]; then
        echo -e "  ${YELLOW}${BOLD}Diagnostic result: RDMA is basically usable, but $WARN_COUNT warning(s) need attention${NC}"
    else
        echo -e "  ${RED}${BOLD}Diagnostic result: $FAIL_COUNT issue(s) need to be fixed${NC}"
        echo ""
        echo -e "  ${BOLD}Common fix steps:${NC}"
        echo "  1. Load kernel modules: modprobe ib_core ib_uverbs"
        echo "  2. Create SoftRoCE: rdma link add rxe0 type rxe netdev eth0"
        echo "  3. Set memory limit: ulimit -l unlimited"
        echo "  4. Install packages: apt install libibverbs-dev perftest"
    fi
    echo ""
}

# ========== Main Flow ==========
main() {
    echo ""
    echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║   RDMA One-Click Environment Diagnostic Tool  ║${NC}"
    echo -e "${BOLD}${CYAN}║   Check modules/devices/ports/GID/quotas      ║${NC}"
    echo -e "${BOLD}${CYAN}╚═══════════════════════════════════════╝${NC}"

    check_kernel_modules
    check_verbs_devices
    check_rdma_tool
    check_port_state
    check_lid_gid
    check_softrce
    check_device_files
    check_memlock
    check_dmesg
    check_perftest

    print_summary
}

main "$@"
