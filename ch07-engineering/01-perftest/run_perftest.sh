#!/bin/bash
#
# RDMA perftest 自动化基准测试脚本
#
# 功能：
#   - 检测 perftest 工具是否安装
#   - 自动运行延迟和带宽测试 (loopback 模式)
#   - 测试多种消息大小，生成带宽曲线
#   - 格式化输出测试结果
#
# 用法：
#   ./run_perftest.sh [设备名]
#   ./run_perftest.sh              # 使用默认设备
#   ./run_perftest.sh rxe0         # 指定设备
#
# 说明：
#   本脚本使用 loopback 模式 (服务端在后台运行，客户端连接本机)。
#   如需跨机器测试，请手动在两台机器上分别运行 server 和 client。
#

set -e

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'  # 无颜色

# ========== 全局变量 ==========
DEVICE=""                    # RDMA 设备名
PORT=18515                   # loopback 测试用端口基址
DURATION=5                   # 每个测试运行秒数
RESULT_DIR="/tmp/perftest_results_$$"  # 临时结果目录
PASS_COUNT=0                 # 通过的测试数
FAIL_COUNT=0                 # 失败的测试数

# ========== 辅助函数 ==========

# 打印带颜色的信息
info()  { echo -e "${BLUE}[信息]${NC} $*"; }
ok()    { echo -e "${GREEN}[成功]${NC} $*"; }
warn()  { echo -e "${YELLOW}[警告]${NC} $*"; }
fail()  { echo -e "${RED}[失败]${NC} $*"; }

# 打印分隔线
separator() {
    echo -e "${CYAN}$(printf '=%.0s' {1..70})${NC}"
}

# 打印章节标题
section() {
    echo ""
    separator
    echo -e "${BOLD}${CYAN}  $*${NC}"
    separator
}

# 清理后台进程和临时文件
cleanup() {
    # 杀掉所有后台 perftest 进程
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$RESULT_DIR"
}
trap cleanup EXIT

# ========== 环境检查 ==========

# 检查单个 perftest 工具是否存在
check_tool() {
    local tool=$1
    if command -v "$tool" &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} $tool"
        return 0
    else
        echo -e "  ${RED}✗${NC} $tool (未安装)"
        return 1
    fi
}

# 检查所有 perftest 工具
check_perftest_tools() {
    section "检查 perftest 工具"

    local all_ok=0
    local tools=(ib_send_lat ib_send_bw ib_write_lat ib_write_bw ib_read_lat ib_read_bw)

    for tool in "${tools[@]}"; do
        if ! check_tool "$tool"; then
            all_ok=1
        fi
    done

    if [ $all_ok -ne 0 ]; then
        echo ""
        warn "部分 perftest 工具缺失，请安装："
        echo "  sudo apt-get install perftest"
        echo "  # 或"
        echo "  sudo yum install perftest"
        exit 1
    fi

    ok "所有 perftest 工具已就绪"
}

# 检查 RDMA 设备
check_device() {
    section "检查 RDMA 设备"

    if [ -n "$DEVICE" ]; then
        # 用户指定了设备，验证是否存在
        if ! ibv_devinfo -d "$DEVICE" &>/dev/null; then
            fail "设备 '$DEVICE' 不存在"
            echo "  可用设备："
            ibv_devices 2>/dev/null | tail -n +2 || echo "  (无设备)"
            exit 1
        fi
        ok "使用指定设备: $DEVICE"
    else
        # 自动检测第一个设备
        DEVICE=$(ibv_devices 2>/dev/null | awk 'NR>2 && NF>0 {print $1; exit}')
        if [ -z "$DEVICE" ]; then
            fail "未检测到 RDMA 设备"
            echo "  请确认已加载 RDMA 驱动 (如 SoftRoCE: rdma link add rxe0 type rxe netdev eth0)"
            exit 1
        fi
        ok "自动检测到设备: $DEVICE"
    fi

    # 打印设备基本信息
    echo ""
    info "设备信息："
    ibv_devinfo -d "$DEVICE" 2>/dev/null | head -15 | sed 's/^/  /'
}

# ========== 测试函数 ==========

# 运行延迟测试 (loopback)
#   参数: $1=工具名 $2=消息大小 $3=测试描述
run_latency_test() {
    local tool=$1
    local size=$2
    local desc=$3
    local port=$((PORT++))
    local server_out="$RESULT_DIR/${tool}_${size}_server.txt"
    local client_out="$RESULT_DIR/${tool}_${size}_client.txt"

    info "测试: $desc ($tool, 消息大小=${size}B)"

    # 启动服务端 (后台)
    $tool -d "$DEVICE" -s "$size" -p "$port" -n 1000 > "$server_out" 2>&1 &
    local server_pid=$!
    sleep 1  # 等待服务端就绪

    # 启动客户端
    if $tool -d "$DEVICE" -s "$size" -p "$port" -n 1000 localhost > "$client_out" 2>&1; then
        wait $server_pid 2>/dev/null || true

        # 解析延迟结果 (从客户端输出中提取)
        local avg_lat=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $2}')
        local p99_lat=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $5}')

        if [ -n "$avg_lat" ]; then
            ok "  平均延迟: ${avg_lat} μs"
            [ -n "$p99_lat" ] && echo -e "  ${BLUE}  P99 延迟: ${p99_lat} μs${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            # 尝试从输出中提取其他格式
            echo -e "  ${YELLOW}输出:${NC}"
            tail -5 "$client_out" | sed 's/^/    /'
            PASS_COUNT=$((PASS_COUNT + 1))
        fi
    else
        wait $server_pid 2>/dev/null || true
        fail "  测试失败"
        tail -3 "$client_out" 2>/dev/null | sed 's/^/    /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# 运行带宽测试 (loopback)
#   参数: $1=工具名 $2=消息大小 $3=测试描述
run_bandwidth_test() {
    local tool=$1
    local size=$2
    local desc=$3
    local port=$((PORT++))
    local server_out="$RESULT_DIR/${tool}_${size}_server.txt"
    local client_out="$RESULT_DIR/${tool}_${size}_client.txt"

    info "测试: $desc ($tool, 消息大小=${size}B)"

    # 启动服务端 (后台)
    $tool -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" > "$server_out" 2>&1 &
    local server_pid=$!
    sleep 1

    # 启动客户端
    if $tool -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" localhost > "$client_out" 2>&1; then
        wait $server_pid 2>/dev/null || true

        # 解析带宽结果
        local bw_gbps=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF-1)}')
        local bw_mbps=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF)}')
        local msg_rate=$(grep -E '^\s+[0-9]' "$client_out" | tail -1 | awk '{print $(NF-2)}')

        if [ -n "$bw_gbps" ]; then
            ok "  带宽: ${bw_gbps} Gb/s  (${bw_mbps} MB/s)"
            [ -n "$msg_rate" ] && echo -e "  ${BLUE}  消息速率: ${msg_rate} Mpps${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            echo -e "  ${YELLOW}输出:${NC}"
            tail -5 "$client_out" | sed 's/^/    /'
            PASS_COUNT=$((PASS_COUNT + 1))
        fi
    else
        wait $server_pid 2>/dev/null || true
        fail "  测试失败"
        tail -3 "$client_out" 2>/dev/null | sed 's/^/    /'
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# ========== 主测试套件 ==========

# 测试 1: 基础延迟测试
test_basic_latency() {
    section "测试 1: 基础延迟测试"
    echo ""
    echo "  测量 Send 操作的半程延迟 (half-round-trip)"
    echo ""

    run_latency_test "ib_send_lat" 64 "Send 延迟 (64B 小消息)"
    echo ""
    run_latency_test "ib_write_lat" 64 "RDMA Write 延迟 (64B)"
}

# 测试 2: 基础带宽测试
test_basic_bandwidth() {
    section "测试 2: 基础带宽测试"
    echo ""
    echo "  测量大消息下的最大带宽"
    echo ""

    run_bandwidth_test "ib_write_bw" 1048576 "RDMA Write 带宽 (1MB 大消息)"
    echo ""
    run_bandwidth_test "ib_read_bw" 1048576 "RDMA Read 带宽 (1MB 大消息)"
    echo ""
    run_bandwidth_test "ib_send_bw" 1048576 "Send 带宽 (1MB 大消息)"
}

# 测试 3: 带宽曲线 (多种消息大小)
test_bandwidth_curve() {
    section "测试 3: 带宽 vs 消息大小 曲线 (RDMA Write)"
    echo ""
    echo "  测试不同消息大小下的带宽变化趋势"
    echo ""

    # 表头
    printf "  ${BOLD}%-12s %-15s %-15s${NC}\n" "消息大小" "带宽(Gb/s)" "消息速率(Mpps)"
    printf "  %-12s %-15s %-15s\n" "--------" "----------" "-------------"

    local sizes=(64 256 1024 4096 65536 1048576)
    local size_names=("64 B" "256 B" "1 KB" "4 KB" "64 KB" "1 MB")

    for i in "${!sizes[@]}"; do
        local size=${sizes[$i]}
        local name=${size_names[$i]}
        local port=$((PORT++))
        local server_out="$RESULT_DIR/curve_server_${size}.txt"
        local client_out="$RESULT_DIR/curve_client_${size}.txt"

        # 启动服务端
        ib_write_bw -d "$DEVICE" -s "$size" -p "$port" -D "$DURATION" > "$server_out" 2>&1 &
        local server_pid=$!
        sleep 1

        # 启动客户端
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

# ========== 结果汇总 ==========
print_summary() {
    section "测试汇总"
    echo ""
    echo -e "  设备:     ${BOLD}$DEVICE${NC}"
    echo -e "  通过:     ${GREEN}$PASS_COUNT${NC}"
    echo -e "  失败:     ${RED}$FAIL_COUNT${NC}"
    echo ""

    # 性能指标说明
    echo -e "${BOLD}  指标说明:${NC}"
    echo "  ┌─────────────────────────────────────────────────────┐"
    echo "  │ BW (Gb/s)   带宽，千兆位/秒                        │"
    echo "  │             1 Gb/s = 125 MB/s = 128000 KB/s        │"
    echo "  │ BW (MB/s)   带宽，兆字节/秒                        │"
    echo "  │ Latency(μs) 半程延迟，微秒                         │"
    echo "  │             典型值: IB=1-2μs, RoCE=3-5μs, SoftRoCE>10μs│"
    echo "  │ MsgRate     每秒消息数 (百万条/秒)                  │"
    echo "  │             小消息场景下更关注消息速率而非带宽      │"
    echo "  └─────────────────────────────────────────────────────┘"
    echo ""

    if [ $FAIL_COUNT -eq 0 ]; then
        ok "所有测试通过!"
    else
        warn "有 $FAIL_COUNT 个测试失败，请检查设备状态和日志"
    fi
}

# ========== 主流程 ==========

main() {
    echo ""
    echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║    RDMA perftest 自动化基准测试          ║${NC}"
    echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════╝${NC}"
    echo ""

    # 解析参数
    if [ -n "$1" ]; then
        DEVICE="$1"
    fi

    # 创建临时结果目录
    mkdir -p "$RESULT_DIR"

    # 环境检查
    check_perftest_tools
    check_device

    # 运行测试
    test_basic_latency
    test_basic_bandwidth
    test_bandwidth_curve

    # 汇总
    print_summary
}

main "$@"
