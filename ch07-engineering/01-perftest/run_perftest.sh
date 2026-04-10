#!/bin/bash
#
# RDMA perftest Automated Benchmark Script
#
# Features:
#   - Detects if perftest tools are installed
#   - Automatically runs latency and bandwidth tests (loopback mode)
#   - Tests multiple message sizes, generates bandwidth curve
#   - Formatted output of test results
#
# Usage:
#   ./run_perftest.sh [device_name]
#   ./run_perftest.sh              # Use default device
#   ./run_perftest.sh rxe0         # Specify device
#
# Notes:
#   This script uses loopback mode (server runs in background, client connects to localhost).
#   For cross-machine testing, manually run server and client on two separate machines.
#

set -e

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'  # No color

# ========== Global Variables ==========
DEVICE=""                    # RDMA device name
PORT=18515                   # Base port for loopback tests
DURATION=5                   # Duration of each test in seconds
RESULT_DIR="/tmp/perftest_results_$$"  # Temporary results directory
PASS_COUNT=0                 # Number of passed tests
FAIL_COUNT=0                 # Number of failed tests

# ========== Helper Functions ==========

# Print colored messages
info()  { echo -e "${BLUE}[Info]${NC} $*"; }
ok()    { echo -e "${GREEN}[Pass]${NC} $*"; }
warn()  { echo -e "${YELLOW}[Warn]${NC} $*"; }
fail()  { echo -e "${RED}[Fail]${NC} $*"; }

# Print separator line
separator() {
    echo -e "${CYAN}$(printf '=%.0s' {1..70})${NC}"
}

# Print section title
section() {
    echo ""
    separator
    echo -e "${BOLD}${CYAN}  $*${NC}"
    separator
}

# Cleanup background processes and temp files
cleanup() {
    # Kill all background perftest processes
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$RESULT_DIR"
}
trap cleanup EXIT

# ========== Environment Checks ==========

# Check if a single perftest tool exists
check_tool() {
    local tool=$1
    if command -v "$tool" &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} $tool"
        return 0
    else
        echo -e "  ${RED}✗${NC} $tool (not installed)"
        return 1
    fi
}

# Check all perftest tools
check_perftest_tools() {
    section "Checking perftest Tools"

    local all_ok=0
    local tools=(ib_send_lat ib_send_bw ib_write_lat ib_write_bw ib_read_lat ib_read_bw)

    for tool in "${tools[@]}"; do
        if ! check_tool "$tool"; then
            all_ok=1
        fi
    done

    if [ $all_ok -ne 0 ]; then
        echo ""
        warn "Some perftest tools are missing, please install:"
        echo "  sudo apt-get install perftest"
        echo "  # or"
        echo "  sudo yum install perftest"
        exit 1
    fi

    ok "All perftest tools are ready"
}

# Check RDMA device
check_device() {
    section "Checking RDMA Device"

    if [ -n "$DEVICE" ]; then
        # User specified a device, verify it exists
        if ! ibv_devinfo -d "$DEVICE" &>/dev/null; then
            fail "Device '$DEVICE' does not exist"
            echo "  Available devices:"
            ibv_devices 2>/dev/null | tail -n +2 || echo "  (none)"
            exit 1
        fi
        ok "Using specified device: $DEVICE"
    else
        # Auto-detect the first device
        DEVICE=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF>0 {print $1; exit}')
        if [ -z "$DEVICE" ]; then
            fail "No RDMA device detected"
            echo "  Please verify RDMA driver is loaded (e.g., SoftRoCE: rdma link add rxe0 type rxe netdev eth0)"
            exit 1
        fi
        ok "Auto-detected device: $DEVICE"
    fi

    # Print basic device info
    echo ""
    info "Device info:"
    ibv_devinfo -d "$DEVICE" 2>/dev/null | head -15 | sed 's/^/  /'
}

# ========== Test Functions ==========

# Run latency test (loopback)
#   Args: $1=tool_name $2=message_size $3=test_description
run_latency_test() {
    local tool=$1
    local size=$2
    local desc=$3
    local port=$((PORT++))
    local server_out="$RESULT_DIR/${tool}_${size}_server.txt"
    local client_out="$RESULT_DIR/${tool}_${size}_client.txt"

    info "Test: $desc ($tool, msg_size=${size}B)"

    # Start server (background)
    $tool -d "$DEVICE" -s "$size" -p "$port" -n 1000 > "$server_out" 2>&1 &
    local server_pid=$!
    sleep 1  # Wait for server to be ready

    # Start client
    if $tool -d "$DEVICE" -s "$size" -p "$port" -n 1000 localhost > "$client_out" 2>&1; then
        wait $server_pid 2>/dev/null || true

        # Parse latency results (extract from client output)
        local avg_lat=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $2}')
        local p99_lat=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $5}')

        if [ -n "$avg_lat" ]; then
            ok "  Average latency: ${avg_lat} us"
            [ -n "$p99_lat" ] && echo -e "  ${BLUE}  P99 latency: ${p99_lat} us${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            # Try to extract other format from output
            echo -e "  ${YELLOW}Output:${NC}"
            tail -5 "$client_out" | sed 's/^/    /'
            PASS_COUNT=$((PASS_COUNT + 1))
        fi
    else
        wait $server_pid 2>/dev/null || true
        fail "  Test failed"
        tail -3 "$client_out" 2>/dev/null | sed 's/^/    /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# Run bandwidth test (loopback)
#   Args: $1=tool_name $2=message_size $3=test_description
run_bandwidth_test() {
    local tool=$1
    local size=$2
    local desc=$3
    local port=$((PORT++))
    local server_out="$RESULT_DIR/${tool}_${size}_server.txt"
    local client_out="$RESULT_DIR/${tool}_${size}_client.txt"

    info "Test: $desc ($tool, msg_size=${size}B)"

    # Start server (background)
    $tool -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" > "$server_out" 2>&1 &
    local server_pid=$!
    sleep 1

    # Start client
    if $tool -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" localhost > "$client_out" 2>&1; then
        wait $server_pid 2>/dev/null || true

        # Parse bandwidth results
        local bw_gbps=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF-1)}')
        local bw_mbps=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF)}')
        local msg_rate=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF-2)}')

        if [ -n "$bw_gbps" ]; then
            ok "  Bandwidth: ${bw_gbps} Gb/s  (${bw_mbps} MB/s)"
            [ -n "$msg_rate" ] && echo -e "  ${BLUE}  Message rate: ${msg_rate} Mpps${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            echo -e "  ${YELLOW}Output:${NC}"
            tail -5 "$client_out" | sed 's/^/    /'
            PASS_COUNT=$((PASS_COUNT + 1))
        fi
    else
        wait $server_pid 2>/dev/null || true
        fail "  Test failed"
        tail -3 "$client_out" 2>/dev/null | sed 's/^/    /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# ========== Main Test Suite ==========

# Test 1: Basic latency test
test_basic_latency() {
    section "Test 1: Basic Latency Test"
    echo ""
    echo "  Measuring Send operation half-round-trip latency"
    echo ""

    run_latency_test "ib_send_lat" 64 "Send latency (64B small message)"
    echo ""
    run_latency_test "ib_write_lat" 64 "RDMA Write latency (64B)"
}

# Test 2: Basic bandwidth test
test_basic_bandwidth() {
    section "Test 2: Basic Bandwidth Test"
    echo ""
    echo "  Measuring maximum bandwidth with large messages"
    echo ""

    run_bandwidth_test "ib_write_bw" 1048576 "RDMA Write bandwidth (1MB large message)"
    echo ""
    run_bandwidth_test "ib_read_bw" 1048576 "RDMA Read bandwidth (1MB large message)"
    echo ""
    run_bandwidth_test "ib_send_bw" 1048576 "Send bandwidth (1MB large message)"
}

# Test 3: Bandwidth curve (multiple message sizes)
test_bandwidth_curve() {
    section "Test 3: Bandwidth vs Message Size Curve (RDMA Write)"
    echo ""
    echo "  Testing bandwidth variation across different message sizes"
    echo ""

    # Table header
    printf "  ${BOLD}%-12s %-15s %-15s${NC}\n" "Msg Size" "BW (Gb/s)" "MsgRate (Mpps)"
    printf "  %-12s %-15s %-15s\n" "--------" "----------" "-------------"

    local sizes=(64 256 1024 4096 65536 1048576)
    local size_names=("64 B" "256 B" "1 KB" "4 KB" "64 KB" "1 MB")

    for i in "${!sizes[@]}"; do
        local size=${sizes[$i]}
        local name=${size_names[$i]}
        local port=$((PORT++))
        local server_out="$RESULT_DIR/curve_server_${size}.txt"
        local client_out="$RESULT_DIR/curve_client_${size}.txt"

        # Start server
        ib_write_bw -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" > "$server_out" 2>&1 &
        local server_pid=$!
        sleep 1

        # Start client
        if ib_write_bw -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" localhost > "$client_out" 2>&1; then
            wait $server_pid 2>/dev/null || true

            local bw=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF-1)}')
            local rate=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF-2)}')

            printf "  ${GREEN}%-12s${NC} %-15s %-15s\n" "$name" "${bw:-N/A}" "${rate:-N/A}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            wait $server_pid 2>/dev/null || true
            printf "  ${RED}%-12s${NC} %-15s %-15s\n" "$name" "FAIL" "FAIL"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    done
}

# ========== Results Summary ==========
print_summary() {
    section "Test Summary"
    echo ""
    echo -e "  Device:     ${BOLD}$DEVICE${NC}"
    echo -e "  Passed:     ${GREEN}$PASS_COUNT${NC}"
    echo -e "  Failed:     ${RED}$FAIL_COUNT${NC}"
    echo ""

    # Metrics explanation
    echo -e "${BOLD}  Metrics Explained:${NC}"
    echo "  ┌─────────────────────────────────────────────────────┐"
    echo "  │ BW (Gb/s)   Bandwidth, gigabits per second          │"
    echo "  │             1 Gb/s = 125 MB/s = 128000 KB/s        │"
    echo "  │ BW (MB/s)   Bandwidth, megabytes per second         │"
    echo "  │ Latency(us) Half-round-trip latency, microseconds   │"
    echo "  │             Typical: IB=1-2us, RoCE=3-5us, SoftRoCE>10us│"
    echo "  │ MsgRate     Messages per second (millions/sec)      │"
    echo "  │             More relevant than BW for small messages │"
    echo "  └─────────────────────────────────────────────────────┘"
    echo ""

    if [ $FAIL_COUNT -eq 0 ]; then
        ok "All tests passed!"
    else
        warn "$FAIL_COUNT test(s) failed, please check device status and logs"
    fi
}

# ========== Main Flow ==========

main() {
    echo ""
    echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║    RDMA perftest Automated Benchmark     ║${NC}"
    echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════╝${NC}"
    echo ""

    # Parse arguments
    if [ -n "$1" ]; then
        DEVICE="$1"
    fi

    # Create temporary results directory
    mkdir -p "$RESULT_DIR"

    # Environment checks
    check_perftest_tools
    check_device

    # Run tests
    test_basic_latency
    test_basic_bandwidth
    test_bandwidth_curve

    # Summary
    print_summary
}

main "$@"
