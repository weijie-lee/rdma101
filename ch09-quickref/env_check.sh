#!/bin/bash
# =============================================================================
# RDMA Environment One-Click Check Script
#
# Function: 10 checks to comprehensively verify RDMA dev/runtime environment
#
# Usage: bash env_check.sh
#
# Check items:
#   1. RDMA kernel modules loaded
#   2. ibv_devices can see devices
#   3. At least one port in ACTIVE state
#   4. LID assigned (IB) or GID valid (RoCE)
#   5. SM running (IB mode only)
#   6. /dev/infiniband/ device files exist
#   7. ulimit -l locked memory limit sufficient
#   8. libibverbs installed
#   9. rdma-core version
#   10. perftest tools available
# =============================================================================

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ========== Counters ==========
PASS=0
FAIL=0
TOTAL=10

# ========== Helper Functions ==========

print_pass() {
    echo -e "  [${GREEN}PASS${NC}] $1"
    PASS=$((PASS + 1))
}

print_fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
    FAIL=$((FAIL + 1))
}

print_fix() {
    echo -e "         ${YELLOW}Fix: $1${NC}"
}

print_info() {
    echo -e "         ${BLUE}↳ $1${NC}"
}

# ========== Header ==========
echo ""
echo -e "${CYAN}================================================${NC}"
echo -e "${CYAN}      RDMA Environment One-Click Check (10 Items)${NC}"
echo -e "${CYAN}================================================${NC}"
echo ""

# ========== Check 1: RDMA Kernel Modules ==========
echo -e "${CYAN}[1/10] RDMA Kernel Modules${NC}"

# Check critical RDMA kernel modules
MODULES="ib_core ib_uverbs rdma_ucm"
LOADED=0
MISSING=""

for mod in $MODULES; do
    if lsmod 2>/dev/null | grep -q "^$mod "; then
        LOADED=$((LOADED + 1))
    else
        MISSING="$MISSING $mod"
    fi
done

if [ $LOADED -ge 2 ]; then
    print_pass "RDMA kernel modules loaded ($LOADED core modules)"
    # Show loaded RDMA-related modules
    lsmod 2>/dev/null | grep -E "^(ib_|rdma_|mlx|rxe)" | while IFS= read -r line; do
        print_info "$(echo "$line" | awk '{print $1}')"
    done
else
    print_fail "RDMA kernel modules incomplete (missing:$MISSING)"
    print_fix "modprobe ib_core ib_uverbs rdma_ucm"
    print_fix "If using SoftRoCE: modprobe rdma_rxe"
fi

echo ""

# ========== Check 2: ibv_devices ==========
echo -e "${CYAN}[2/10] RDMA Device List${NC}"

if ! command -v ibv_devices &>/dev/null; then
    print_fail "ibv_devices command not installed"
    print_fix "apt install ibverbs-utils or yum install libibverbs-utils"
else
    DEVICE_OUTPUT=$(ibv_devices 2>/dev/null)
    DEVICE_COUNT=$(echo "$DEVICE_OUTPUT" | grep -c "^\s")

    if [ "$DEVICE_COUNT" -gt 0 ]; then
        print_pass "Found $DEVICE_COUNT RDMA device(s)"
        echo "$DEVICE_OUTPUT" | grep "^\s" | while IFS= read -r line; do
            print_info "$(echo "$line" | xargs)"
        done
    else
        print_fail "No RDMA devices found"
        print_fix "Physical NIC: Check if Mellanox driver is installed"
        print_fix "SoftRoCE: rdma link add rxe_0 type rxe netdev eth0"
    fi
fi

echo ""

# ========== Check 3: Port Status ==========
echo -e "${CYAN}[3/10] Port Status (at least one ACTIVE)${NC}"

if command -v ibv_devinfo &>/dev/null; then
    ACTIVE_COUNT=$(ibv_devinfo 2>/dev/null | grep -c "PORT_ACTIVE")

    if [ "$ACTIVE_COUNT" -gt 0 ]; then
        print_pass "$ACTIVE_COUNT port(s) in ACTIVE state"
    else
        print_fail "No ACTIVE ports"
        print_fix "Check cable connections and switch configuration"
        print_fix "SoftRoCE: Ensure underlying interface (eth0) is UP"
    fi
else
    print_fail "ibv_devinfo not installed, cannot check port status"
    print_fix "apt install ibverbs-utils"
fi

echo ""

# ========== Check 4: LID (IB) or GID (RoCE) ==========
echo -e "${CYAN}[4/10] Addressing Information (LID / GID)${NC}"

if command -v ibv_devinfo &>/dev/null; then
    # Check link_layer to determine mode
    LINK_LAYER=$(ibv_devinfo 2>/dev/null | grep "link_layer" | head -1 | awk '{print $2}')
    LID=$(ibv_devinfo 2>/dev/null | grep "sm_lid" | head -1 | awk '{print $2}')

    if [ "$LINK_LAYER" = "Ethernet" ]; then
        # RoCE mode: check GID
        # Get first device name
        DEV_NAME=$(ibv_devinfo -l 2>/dev/null | grep "^\s" | head -1 | xargs)
        GID_PATH="/sys/class/infiniband/$DEV_NAME/ports/1/gids/0"
        if [ -f "$GID_PATH" ]; then
            GID_VAL=$(cat "$GID_PATH" 2>/dev/null)
            if [ -n "$GID_VAL" ] && [ "$GID_VAL" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
                print_pass "RoCE mode - GID valid: $GID_VAL"
            else
                print_fail "RoCE mode - GID invalid (all zeros)"
                print_fix "Check network interface IP configuration; GID is usually generated from IPv4/IPv6 address"
            fi
        else
            print_fail "Cannot read GID table"
            print_fix "Check /sys/class/infiniband/ directory"
        fi
    else
        # IB mode: check LID
        PORT_LID=$(ibv_devinfo 2>/dev/null | grep "^\s*lid:" | head -1 | awk '{print $2}')
        if [ -n "$PORT_LID" ] && [ "$PORT_LID" != "0" ] && [ "$PORT_LID" != "0x0000" ]; then
            print_pass "IB mode - LID assigned: $PORT_LID"
        else
            print_fail "IB mode - LID not assigned (is 0)"
            print_fix "Check if Subnet Manager (opensm) is running"
        fi
    fi
else
    print_fail "Cannot check addressing information"
fi

echo ""

# ========== Check 5: Subnet Manager (IB only) ==========
echo -e "${CYAN}[5/10] Subnet Manager (IB mode only)${NC}"

LINK_LAYER=$(ibv_devinfo 2>/dev/null | grep "link_layer" | head -1 | awk '{print $2}')

if [ "$LINK_LAYER" = "Ethernet" ]; then
    print_pass "RoCE mode, Subnet Manager not required"
else
    # IB mode: check SM
    SM_LID=$(ibv_devinfo 2>/dev/null | grep "sm_lid" | head -1 | awk '{print $2}')
    if [ -n "$SM_LID" ] && [ "$SM_LID" != "0" ] && [ "$SM_LID" != "0x0000" ]; then
        print_pass "Subnet Manager running (SM LID: $SM_LID)"
    else
        # Check opensm process
        if pgrep -x "opensm" &>/dev/null; then
            print_pass "opensm process is running"
        else
            print_fail "Subnet Manager not running"
            print_fix "Start: systemctl start opensm"
            print_fix "Or: opensm -B (run in background)"
        fi
    fi
fi

echo ""

# ========== Check 6: /dev/infiniband/ ==========
echo -e "${CYAN}[6/10] /dev/infiniband/ Device Files${NC}"

if [ -d "/dev/infiniband" ]; then
    UVERBS_COUNT=$(ls /dev/infiniband/uverbs* 2>/dev/null | wc -l)
    if [ "$UVERBS_COUNT" -gt 0 ]; then
        print_pass "/dev/infiniband/ exists ($UVERBS_COUNT uverbs device(s))"
        ls /dev/infiniband/ 2>/dev/null | while IFS= read -r f; do
            print_info "$f"
        done
    else
        print_fail "/dev/infiniband/ exists but no uverbs devices"
        print_fix "Check ib_uverbs module: modprobe ib_uverbs"
    fi
else
    print_fail "/dev/infiniband/ directory does not exist"
    print_fix "RDMA kernel modules not loaded correctly"
    print_fix "modprobe ib_core ib_uverbs"
fi

echo ""

# ========== Check 7: ulimit -l ==========
echo -e "${CYAN}[7/10] Locked Memory Limit (ulimit -l)${NC}"

MEMLOCK=$(ulimit -l 2>/dev/null)

if [ "$MEMLOCK" = "unlimited" ]; then
    print_pass "Locked memory limit: unlimited (optimal)"
elif [ -n "$MEMLOCK" ] && [ "$MEMLOCK" -ge 65536 ] 2>/dev/null; then
    print_pass "Locked memory limit: ${MEMLOCK} KB (>= 64MB, sufficient)"
else
    print_fail "Locked memory limit insufficient: ${MEMLOCK} KB (need >= 65536 KB)"
    print_fix "Temporary: ulimit -l unlimited"
    print_fix "Permanent: Edit /etc/security/limits.conf and add:"
    print_fix "  * soft memlock unlimited"
    print_fix "  * hard memlock unlimited"
fi

echo ""

# ========== Check 8: libibverbs ==========
echo -e "${CYAN}[8/10] libibverbs Library${NC}"

LIBIBVERBS=$(ldconfig -p 2>/dev/null | grep "libibverbs.so" | head -1)

if [ -n "$LIBIBVERBS" ]; then
    print_pass "libibverbs installed"
    print_info "$LIBIBVERBS"
    # Check development headers
    if [ -f "/usr/include/infiniband/verbs.h" ]; then
        print_info "Headers: /usr/include/infiniband/verbs.h ✓"
    else
        print_info "Warning: development headers not installed (need libibverbs-dev)"
    fi
else
    print_fail "libibverbs not installed"
    print_fix "apt install libibverbs-dev or yum install libibverbs-devel"
fi

echo ""

# ========== Check 9: rdma-core version ==========
echo -e "${CYAN}[9/10] rdma-core Version${NC}"

RDMA_CORE_VER=""
# Debian/Ubuntu
RDMA_CORE_VER=$(dpkg -l 2>/dev/null | grep "rdma-core" | head -1 | awk '{print $3}')
# CentOS/RHEL
if [ -z "$RDMA_CORE_VER" ]; then
    RDMA_CORE_VER=$(rpm -q rdma-core 2>/dev/null | grep -v "not installed")
fi

if [ -n "$RDMA_CORE_VER" ]; then
    print_pass "rdma-core version: $RDMA_CORE_VER"
else
    print_fail "rdma-core not installed or version cannot be determined"
    print_fix "apt install rdma-core or yum install rdma-core"
fi

echo ""

# ========== Check 10: perftest ==========
echo -e "${CYAN}[10/10] perftest Tools (Performance Testing)${NC}"

PERFTEST_TOOLS="ib_write_bw ib_write_lat ib_read_bw ib_send_bw"
FOUND=0

for tool in $PERFTEST_TOOLS; do
    if command -v "$tool" &>/dev/null; then
        FOUND=$((FOUND + 1))
    fi
done

if [ "$FOUND" -ge 3 ]; then
    print_pass "perftest tools installed ($FOUND/4 tools available)"
elif [ "$FOUND" -gt 0 ]; then
    print_pass "perftest partially installed ($FOUND/4 tools available)"
else
    print_fail "perftest tools not installed"
    print_fix "apt install perftest or yum install perftest"
fi

echo ""

# ========== Summary ==========
echo -e "${CYAN}================================================${NC}"
echo -e "${CYAN}      Check Results Summary${NC}"
echo -e "${CYAN}================================================${NC}"
echo ""
echo -e "  ${GREEN}Passed: $PASS${NC}  /  ${RED}Failed: $FAIL${NC}  /  Total: $TOTAL"
echo ""

if [ "$PASS" -eq "$TOTAL" ]; then
    echo -e "  ${GREEN}★ All passed! RDMA environment is fully ready ★${NC}"
elif [ "$PASS" -ge 7 ]; then
    echo -e "  ${YELLOW}△ Basically usable, but $FAIL item(s) need to be fixed${NC}"
elif [ "$PASS" -ge 4 ]; then
    echo -e "  ${YELLOW}△ Environment incomplete, please fix the FAIL items above${NC}"
else
    echo -e "  ${RED}✗ RDMA environment not ready, please follow the fix suggestions to resolve each item${NC}"
fi

echo ""
echo -e "${BLUE}Tip: Re-run this script after fixing to verify${NC}"
echo -e "${BLUE}     bash env_check.sh${NC}"
echo ""
