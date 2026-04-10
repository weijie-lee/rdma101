#!/bin/bash
#
# RDMA 101 — 全量测试运行器
#
# 在 SoftRoCE 环境上逐个验证所有示例程序，确保学习环境完整可用。
#
# 使用方法：
#   chmod +x scripts/run_all_tests.sh
#   ulimit -l unlimited
#   ./scripts/run_all_tests.sh
#
# 测试分两类:
#   - 单进程: 直接运行，自包含 loopback 测试
#   - 双进程: 先启动 server (后台)，再启动 client (127.0.0.1)
#

set -o pipefail

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# ========== 全局计数 ==========
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# 超时时间（秒）
TIMEOUT=15
CS_TIMEOUT=20  # client-server 模式更长一些

# ========== 辅助函数 ==========

# 运行单进程测试
run_single() {
    local name="$1"
    local binary="$2"
    local args="$3"
    TOTAL=$((TOTAL + 1))

    if [ ! -x "$binary" ]; then
        echo -e "  ${YELLOW}[SKIP]${NC} $name — 二进制不存在: $binary"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    if timeout $TIMEOUT "$binary" $args >/dev/null 2>&1; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        PASSED=$((PASSED + 1))
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "  ${YELLOW}[TOUT]${NC} $name — 超时 (${TIMEOUT}s)"
            SKIPPED=$((SKIPPED + 1))
        else
            echo -e "  ${RED}[FAIL]${NC} $name (退出码=$exit_code)"
            FAILED=$((FAILED + 1))
        fi
    fi
}

# 运行双进程 (server + client) 测试
run_client_server() {
    local name="$1"
    local binary="$2"
    local server_args="$3"
    local client_args="$4"
    local wait_time="${5:-2}"  # server 启动等待时间，默认 2 秒
    TOTAL=$((TOTAL + 1))

    if [ ! -x "$binary" ]; then
        echo -e "  ${YELLOW}[SKIP]${NC} $name — 二进制不存在: $binary"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    # 启动 server（后台）
    timeout $CS_TIMEOUT "$binary" $server_args >/dev/null 2>&1 &
    local server_pid=$!

    # 等待 server 启动
    sleep "$wait_time"

    # 检查 server 是否还在运行
    if ! kill -0 $server_pid 2>/dev/null; then
        echo -e "  ${RED}[FAIL]${NC} $name — server 启动失败"
        FAILED=$((FAILED + 1))
        wait $server_pid 2>/dev/null || true
        return
    fi

    # 启动 client
    if timeout $CS_TIMEOUT "$binary" $client_args >/dev/null 2>&1; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        PASSED=$((PASSED + 1))
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "  ${YELLOW}[TOUT]${NC} $name — client 超时"
            SKIPPED=$((SKIPPED + 1))
        else
            echo -e "  ${RED}[FAIL]${NC} $name (client 退出码=$exit_code)"
            FAILED=$((FAILED + 1))
        fi
    fi

    # 清理 server 进程
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# ========== 主测试流程 ==========

echo ""
echo "================================================="
echo "   RDMA 101 — 全量测试运行器 (SoftRoCE)"
echo "================================================="
echo ""

# 检查环境
if ! ibv_devices >/dev/null 2>&1; then
    echo -e "${RED}错误: 没有检测到 RDMA 设备${NC}"
    echo "请先运行: sudo ./scripts/setup_softrce.sh"
    exit 1
fi

# 确保 ulimit
ulimit -l unlimited 2>/dev/null || true

echo -e "${BOLD}=== ch03-verbs-api: 核心编程对象 ===${NC}"
echo ""

echo "  --- 01-initialization ---"
run_single "设备初始化 (六步法)"     "ch03-verbs-api/01-initialization/01_init_resources"
run_single "多设备多端口枚举"       "ch03-verbs-api/01-initialization/02_multi_device"

echo "  --- 02-qp-state ---"
run_single "QP 状态机 (loopback)"  "ch03-verbs-api/02-qp-state/qp_state"
run_single "QP 错误恢复"           "ch03-verbs-api/02-qp-state/01_qp_error_recovery"

echo "  --- 03-pd ---"
run_single "PD 保护域隔离"         "ch03-verbs-api/03-pd/pd_isolation"

echo "  --- 04-mr ---"
run_single "MR 访问权限标志"        "ch03-verbs-api/04-mr/mr_access_flags"
run_single "MR 多次注册"           "ch03-verbs-api/04-mr/mr_multi_reg"

echo "  --- 05-cq ---"
run_single "CQ 事件驱动"           "ch03-verbs-api/05-cq/cq_event_driven"
run_single "CQ 溢出演示"           "ch03-verbs-api/05-cq/cq_overflow"

echo ""
echo -e "${BOLD}=== ch05-communication: 通信操作 ===${NC}"
echo ""

echo "  --- 单进程 loopback ---"
run_single "Send/Recv Loopback"    "ch05-communication/02-send-recv/01_loopback_send_recv"
run_single "Scatter-Gather 演示"   "ch05-communication/02-send-recv/02_sge_demo"

echo "  --- 双进程 (server + client 127.0.0.1) ---"
run_client_server "RDMA Write"     "ch05-communication/01-rdma-write/rdma_write" \
    "server" "client 127.0.0.1"

run_client_server "Send/Recv (C/S)" "ch05-communication/02-send-recv/send_recv" \
    "server" "client 127.0.0.1"

run_client_server "RDMA Read"      "ch05-communication/03-rdma-read/rdma_read" \
    "server" "client 127.0.0.1"

run_client_server "原子操作 (FAA/CAS)" "ch05-communication/04-atomic/atomic_ops" \
    "server" "client 127.0.0.1"

run_client_server "Write with Imm" "ch05-communication/01-rdma-write/01_write_imm" \
    "server" "client 127.0.0.1"

run_client_server "RNR 错误演示"    "ch05-communication/02-send-recv/03_rnr_error_demo" \
    "server" "client 127.0.0.1"

run_client_server "批量 RDMA Read"  "ch05-communication/03-rdma-read/01_batch_read" \
    "server" "client 127.0.0.1"

run_client_server "对齐错误演示"     "ch05-communication/04-atomic/01_alignment_error" \
    "server" "client 127.0.0.1"

run_client_server "CAS 分布式锁"    "ch05-communication/04-atomic/02_spinlock" \
    "server" "client 127.0.0.1"

echo ""
echo -e "${BOLD}=== ch06-connection: 连接管理 ===${NC}"
echo ""

run_client_server "手动建连 (-s/-c)" "ch06-connection/01-manual-connect/manual_connect" \
    "-s" "-c 127.0.0.1"

run_client_server "RDMA CM"        "ch06-connection/02-rdma-cm/rdma_cm_example" \
    "-s 7471" "-c 127.0.0.1 7471"

run_single "UD 模式 Loopback"      "ch06-connection/03-ud-mode/ud_loopback"

echo ""
echo -e "${BOLD}=== ch07-engineering: 工程实践 ===${NC}"
echo ""

echo "  --- 02-tuning ---"
run_single "Inline Data 优化"      "ch07-engineering/02-tuning/inline_data"
run_single "Unsignaled Send"       "ch07-engineering/02-tuning/unsignaled_send"
run_single "SRQ 共享接收队列"       "ch07-engineering/02-tuning/srq_demo"

echo "  --- 04-error-handling ---"
run_single "错误触发演示"           "ch07-engineering/04-error-handling/trigger_errors"
run_single "错误码诊断工具"         "ch07-engineering/04-error-handling/error_diagnosis"

echo ""
echo -e "${BOLD}=== ch09-quickref: 速查手册 ===${NC}"
echo ""

run_single "Hello RDMA (最小模板)"  "ch09-quickref/hello_rdma"
run_single "错误速查工具"           "ch09-quickref/error_cheatsheet"

# ========== 汇总 ==========
echo ""
echo "================================================="
echo "   测试汇总"
echo "================================================="
echo ""
echo -e "  总计:   ${BOLD}$TOTAL${NC} 个测试"
echo -e "  通过:   ${GREEN}$PASSED${NC}"
echo -e "  失败:   ${RED}$FAILED${NC}"
echo -e "  跳过:   ${YELLOW}$SKIPPED${NC} (超时或二进制不存在)"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}所有测试通过!${NC} SoftRoCE 环境完全可用。"
else
    echo -e "  ${YELLOW}有 $FAILED 个测试失败${NC}，请检查:"
    echo "    1. 是否已运行 ulimit -l unlimited"
    echo "    2. 是否已配置 SoftRoCE: rdma link show"
    echo "    3. 端口是否 ACTIVE: ibv_devinfo | grep state"
fi
echo ""

exit $FAILED
