#!/bin/bash
#
# SoftRoCE One-Click Environment Setup Script
#
# Function: On machines without real RDMA NICs, configure a complete RDMA
#           development and learning environment using SoftRoCE (software RoCE).
#
# Usage:
#   chmod +x scripts/setup_softrce.sh
#   sudo ./scripts/setup_softrce.sh
#
# Prerequisites:
#   - Ubuntu 20.04 / 22.04 / 24.04
#   - At least one working network interface (non-lo)
#   - root privileges or sudo
#

set -e

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

ok()   { echo -e "  ${GREEN}[OK]${NC}   $1"; }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; }
warn() { echo -e "  ${YELLOW}[WARN]${NC} $1"; }
info() { echo -e "  ${BLUE}[INFO]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo ""
echo "============================================="
echo "   RDMA 101 - SoftRoCE One-Click Setup"
echo "============================================="
echo ""

# ========== Step 1: Check Permissions ==========
echo ">>> Step 1/7: Checking permissions"
if [ "$(id -u)" -ne 0 ]; then
    fail "Root privileges required to run this script"
    echo "  Please use: sudo $0"
    exit 1
fi
ok "Root privileges confirmed"

# ========== Step 2: Install Dependencies ==========
echo ""
echo ">>> Step 2/7: Installing RDMA development dependencies"

# Check if already installed
PKGS="libibverbs-dev librdmacm-dev rdma-core ibverbs-utils perftest build-essential"
MISSING=""
for pkg in $PKGS; do
    if ! dpkg -l "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done

if [ -n "$MISSING" ]; then
    info "Installing missing packages:$MISSING"
    apt-get update -qq
    apt-get install -y -qq $MISSING >/dev/null 2>&1
    ok "Dependency packages installed"
else
    ok "All dependency packages already installed"
fi

# ========== Step 3: Load Kernel Modules ==========
echo ""
echo ">>> Step 3/7: Loading SoftRoCE kernel modules"

if lsmod | grep -q rdma_rxe; then
    ok "rdma_rxe module already loaded"
else
    modprobe rdma_rxe
    if lsmod | grep -q rdma_rxe; then
        ok "rdma_rxe module loaded successfully"
    else
        fail "rdma_rxe module failed to load"
        echo "  Possible cause: kernel version not supported, check dmesg"
        exit 1
    fi
fi

# Ensure ib_uverbs is also loaded
if ! lsmod | grep -q ib_uverbs; then
    modprobe ib_uverbs 2>/dev/null || true
fi

# ========== Step 4: Create SoftRoCE Device ==========
echo ""
echo ">>> Step 4/7: Creating SoftRoCE device"

# Check if SoftRoCE device already exists
EXISTING_RXE=$(rdma link show 2>/dev/null | grep "rxe" | head -1 || true)
if [ -n "$EXISTING_RXE" ]; then
    ok "SoftRoCE device already exists: $EXISTING_RXE"
else
    # Auto-select a non-lo network interface
    NETDEV=$(ip -o link show up | grep -v "lo:" | head -1 | awk -F': ' '{print $2}' | tr -d ' ')
    if [ -z "$NETDEV" ]; then
        fail "No available network interface found"
        echo "  Please ensure at least one non-lo interface is UP"
        exit 1
    fi

    info "Using network interface: $NETDEV"
    rdma link add rxe0 type rxe netdev "$NETDEV"
    ok "SoftRoCE device rxe0 created (bound to $NETDEV)"
fi

# ========== Step 5: Configure System Parameters ==========
echo ""
echo ">>> Step 5/7: Configuring system parameters"

# Set memlock limit
# Check current limit
CURRENT_MEMLOCK=$(ulimit -l 2>/dev/null || echo "0")
if [ "$CURRENT_MEMLOCK" = "unlimited" ] || [ "$CURRENT_MEMLOCK" -ge 65536 ] 2>/dev/null; then
    ok "Locked memory limit is sufficient: $CURRENT_MEMLOCK"
else
    # Write to limits.conf
    if ! grep -q "memlock" /etc/security/limits.conf 2>/dev/null; then
        echo "* soft memlock unlimited" >> /etc/security/limits.conf
        echo "* hard memlock unlimited" >> /etc/security/limits.conf
        ok "Updated /etc/security/limits.conf (re-login required to take effect)"
    else
        ok "limits.conf already has memlock configuration"
    fi
    warn "Current shell memlock may require re-login to take effect"
    warn "Temporary fix: run ulimit -l unlimited before running programs"
fi

# ========== Step 6: Verify Environment ==========
echo ""
echo ">>> Step 6/7: Verifying RDMA environment"

# Verify devices
DEVICE_COUNT=$(ibv_devices 2>/dev/null | grep -c "rxe\|mlx\|hfi\|qedr" || echo "0")
if [ "$DEVICE_COUNT" -gt 0 ]; then
    ok "Detected $DEVICE_COUNT RDMA device(s)"
else
    fail "No RDMA devices detected"
    exit 1
fi

# Print device info
echo ""
info "Device details:"
ibv_devinfo 2>/dev/null | head -20
echo ""

# Check port status
PORT_STATE=$(ibv_devinfo 2>/dev/null | grep "state:" | head -1 | awk '{print $2}')
if [ "$PORT_STATE" = "PORT_ACTIVE" ]; then
    ok "Port status: ACTIVE"
else
    warn "Port status: $PORT_STATE (not ACTIVE)"
fi

# Check link_layer
LINK_LAYER=$(ibv_devinfo 2>/dev/null | grep "link_layer:" | head -1 | awk '{print $2}')
if [ "$LINK_LAYER" = "Ethernet" ]; then
    ok "Link layer: Ethernet (RoCE mode)"
    info "Note: LID=0 is normal under SoftRoCE; GID is used for addressing"
else
    info "Link layer: $LINK_LAYER"
fi

# ========== Step 7: Build and Smoke Test ==========
echo ""
echo ">>> Step 7/7: Building project and running smoke test"

cd "$PROJECT_DIR"

# Build
info "Building project (make all)..."
if make all >/dev/null 2>&1; then
    ok "Project built successfully"
else
    warn "Some programs failed to build (may be missing librdmacm-dev), but core programs are usable"
fi

# Smoke test: run hello_rdma
if [ -f "ch09-quickref/hello_rdma" ]; then
    info "Running smoke test: hello_rdma"
    ulimit -l unlimited 2>/dev/null || true
    if timeout 10 ./ch09-quickref/hello_rdma >/dev/null 2>&1; then
        ok "Smoke test passed: hello_rdma runs correctly"
    else
        warn "hello_rdma ran abnormally (may need ulimit -l unlimited)"
    fi
fi

# ========== Complete ==========
echo ""
echo "============================================="
echo -e "  ${GREEN}SoftRoCE environment setup complete!${NC}"
echo "============================================="
echo ""
echo "  SoftRoCE key concepts:"
echo "    - LID = 0 is normal (RoCE doesn't use LID)"
echo "    - Uses GID for addressing (generated from NIC IP address)"
echo "    - Performance is much lower than real hardware (for learning/development only)"
echo "    - All Verbs API behaviors are consistent with real hardware"
echo ""
echo "  Quick start:"
echo "    1. Run single-process example:  ./ch09-quickref/hello_rdma"
echo "    2. Run dual-process example:    (Terminal 1) ./ch06-connection/01-manual-connect/manual_connect -s"
echo "                                    (Terminal 2) ./ch06-connection/01-manual-connect/manual_connect -c 127.0.0.1"
echo "    3. Run all tests:               sudo ./scripts/run_all_tests.sh"
echo ""
echo "  If a program reports 'Cannot allocate memory', run:"
echo "    ulimit -l unlimited"
echo ""
