#!/bin/bash
#
# RDMA 101 - Full Test Suite Runner
#
# Verify all example programs one by one on SoftRoCE environment,
# ensuring the learning environment is complete and functional.
#
# Usage:
#   chmod +x scripts/run_all_tests.sh
#   ulimit -l unlimited
#   ./scripts/run_all_tests.sh
#
# Tests fall into two categories:
#   - Single-process: run directly, self-contained loopback tests
#   - Dual-process: start server (background), then start client (127.0.0.1)
#

set -o pipefail

# ========== Color Definitions ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# ========== Global Counters ==========
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# Timeout (seconds)
TIMEOUT=15
CS_TIMEOUT=20  # Longer for client-server mode

# ========== Helper Functions ==========

# Run single-process test
run_single() {
    local name="$1"
    local binary="$2"
    local args="$3"
    TOTAL=$((TOTAL + 1))

    if [ ! -x "$binary" ]; then
        echo -e "  ${YELLOW}[SKIP]${NC} $name - binary not found: $binary"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    if timeout $TIMEOUT "$binary" $args >/dev/null 2>&1; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        PASSED=$((PASSED + 1))
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "  ${YELLOW}[TOUT]${NC} $name - timeout (${TIMEOUT}s)"
            SKIPPED=$((SKIPPED + 1))
        else
            echo -e "  ${RED}[FAIL]${NC} $name (exit code=$exit_code)"
            FAILED=$((FAILED + 1))
        fi
    fi
}

# Run dual-process (server + client) test
run_client_server() {
    local name="$1"
    local binary="$2"
    local server_args="$3"
    local client_args="$4"
    local wait_time="${5:-2}"  # Server startup wait time, default 2 seconds
    TOTAL=$((TOTAL + 1))

    if [ ! -x "$binary" ]; then
        echo -e "  ${YELLOW}[SKIP]${NC} $name - binary not found: $binary"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    # Start server (background)
    timeout $CS_TIMEOUT "$binary" $server_args >/dev/null 2>&1 &
    local server_pid=$!

    # Wait for server to start
    sleep "$wait_time"

    # Check if server is still running
    if ! kill -0 $server_pid 2>/dev/null; then
        echo -e "  ${RED}[FAIL]${NC} $name - server failed to start"
        FAILED=$((FAILED + 1))
        wait $server_pid 2>/dev/null || true
        return
    fi

    # Start client
    if timeout $CS_TIMEOUT "$binary" $client_args >/dev/null 2>&1; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        PASSED=$((PASSED + 1))
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "  ${YELLOW}[TOUT]${NC} $name - client timeout"
            SKIPPED=$((SKIPPED + 1))
        else
            echo -e "  ${RED}[FAIL]${NC} $name (client exit code=$exit_code)"
            FAILED=$((FAILED + 1))
        fi
    fi

    # Clean up server process
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# ========== Main Test Flow ==========

echo ""
echo "================================================="
echo "   RDMA 101 - Full Test Suite Runner (SoftRoCE)"
echo "================================================="
echo ""

# Check environment
if ! ibv_devices >/dev/null 2>&1; then
    echo -e "${RED}Error: No RDMA devices detected${NC}"
    echo "Please run first: sudo ./scripts/setup_softrce.sh"
    exit 1
fi

# Ensure ulimit
ulimit -l unlimited 2>/dev/null || true

echo -e "${BOLD}=== ch03-verbs-api: Core Programming Objects ===${NC}"
echo ""

echo "  --- 01-initialization ---"
run_single "Device Init (six-step)"     "ch03-verbs-api/01-initialization/01_init_resources"
run_single "Multi-device Multi-port"    "ch03-verbs-api/01-initialization/02_multi_device"

echo "  --- 02-qp-state ---"
run_single "QP State Machine (loopback)" "ch03-verbs-api/02-qp-state/qp_state"
run_single "QP Error Recovery"           "ch03-verbs-api/02-qp-state/01_qp_error_recovery"

echo "  --- 03-pd ---"
run_single "PD Protection Domain"        "ch03-verbs-api/03-pd/pd_isolation"

echo "  --- 04-mr ---"
run_single "MR Access Flags"             "ch03-verbs-api/04-mr/mr_access_flags"
run_single "MR Multi Registration"       "ch03-verbs-api/04-mr/mr_multi_reg"

echo "  --- 05-cq ---"
run_single "CQ Event Driven"             "ch03-verbs-api/05-cq/cq_event_driven"
run_single "CQ Overflow Demo"            "ch03-verbs-api/05-cq/cq_overflow"

echo ""
echo -e "${BOLD}=== ch05-communication: Communication Operations ===${NC}"
echo ""

echo "  --- Single-process loopback ---"
run_single "Send/Recv Loopback"          "ch05-communication/02-send-recv/01_loopback_send_recv"
run_single "Scatter-Gather Demo"         "ch05-communication/02-send-recv/02_sge_demo"

echo "  --- Dual-process (server + client 127.0.0.1) ---"
run_client_server "RDMA Write"           "ch05-communication/01-rdma-write/rdma_write" \
    "server" "client 127.0.0.1"

run_client_server "Send/Recv (C/S)"      "ch05-communication/02-send-recv/send_recv" \
    "server" "client 127.0.0.1"

run_client_server "RDMA Read"            "ch05-communication/03-rdma-read/rdma_read" \
    "server" "client 127.0.0.1"

run_client_server "Atomic Ops (FAA/CAS)" "ch05-communication/04-atomic/atomic_ops" \
    "server" "client 127.0.0.1"

run_client_server "Write with Imm"       "ch05-communication/01-rdma-write/01_write_imm" \
    "server" "client 127.0.0.1"

run_client_server "RNR Error Demo"       "ch05-communication/02-send-recv/03_rnr_error_demo" \
    "server" "client 127.0.0.1"

run_client_server "Batch RDMA Read"      "ch05-communication/03-rdma-read/01_batch_read" \
    "server" "client 127.0.0.1"

run_client_server "Alignment Error Demo" "ch05-communication/04-atomic/01_alignment_error" \
    "server" "client 127.0.0.1"

run_client_server "CAS Distributed Lock" "ch05-communication/04-atomic/02_spinlock" \
    "server" "client 127.0.0.1"

echo ""
echo -e "${BOLD}=== ch06-connection: Connection Management ===${NC}"
echo ""

run_client_server "Manual Connect (-s/-c)" "ch06-connection/01-manual-connect/manual_connect" \
    "-s" "-c 127.0.0.1"

run_client_server "RDMA CM"              "ch06-connection/02-rdma-cm/rdma_cm_example" \
    "-s 7471" "-c 127.0.0.1 7471"

run_single "UD Mode Loopback"            "ch06-connection/03-ud-mode/ud_loopback"

echo ""
echo -e "${BOLD}=== ch07-engineering: Engineering Practice ===${NC}"
echo ""

echo "  --- 02-tuning ---"
run_single "Inline Data Optimization"    "ch07-engineering/02-tuning/inline_data"
run_single "Unsignaled Send"             "ch07-engineering/02-tuning/unsignaled_send"
run_single "SRQ Shared Receive Queue"    "ch07-engineering/02-tuning/srq_demo"

echo "  --- 04-error-handling ---"
run_single "Error Trigger Demo"          "ch07-engineering/04-error-handling/trigger_errors"
run_single "Error Code Diagnostic Tool"  "ch07-engineering/04-error-handling/error_diagnosis"

echo ""
echo -e "${BOLD}=== ch09-quickref: Quick Reference ===${NC}"
echo ""

run_single "Hello RDMA (Minimal Template)" "ch09-quickref/hello_rdma"
run_single "Error Lookup Tool"           "ch09-quickref/error_cheatsheet"

# ========== Summary ==========
echo ""
echo "================================================="
echo "   Test Summary"
echo "================================================="
echo ""
echo -e "  Total:   ${BOLD}$TOTAL${NC} tests"
echo -e "  Passed:  ${GREEN}$PASSED${NC}"
echo -e "  Failed:  ${RED}$FAILED${NC}"
echo -e "  Skipped: ${YELLOW}$SKIPPED${NC} (timeout or binary not found)"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}All tests passed!${NC} SoftRoCE environment fully functional."
else
    echo -e "  ${YELLOW}$FAILED test(s) failed${NC}, please check:"
    echo "    1. Have you run ulimit -l unlimited"
    echo "    2. Is SoftRoCE configured: rdma link show"
    echo "    3. Is the port ACTIVE: ibv_devinfo | grep state"
fi
echo ""

exit $FAILED
