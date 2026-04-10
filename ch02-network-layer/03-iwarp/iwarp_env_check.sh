#!/bin/bash
#
# iWARP 环境检查脚本
#
# 功能：
#   - 检查是否存在 iWARP 设备
#   - 检查 iWARP 驱动 (iw_cxgb4, i40iw, irdma)
#   - 使用 rdma link / ibv_devinfo 显示 iWARP 信息
#   - 显示 iWARP 特定的节点类型 (RNIC)
#
# 用法: chmod +x iwarp_env_check.sh && ./iwarp_env_check.sh
#
set -e

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== 辅助函数 ==========

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

# ========== 1. 检查 iWARP 驱动模块 ==========
separator "1. iWARP 驱动模块检查"

iwarp_driver_found=false

# Chelsio T5/T6 驱动
if lsmod 2>/dev/null | grep -q "iw_cxgb4"; then
    pass "iw_cxgb4 模块已加载 (Chelsio T5/T6 iWARP)"
    iwarp_driver_found=true
else
    info "iw_cxgb4 模块未加载 (Chelsio iWARP 驱动)"
fi

# Intel X722 驱动 (旧版)
if lsmod 2>/dev/null | grep -q "i40iw"; then
    pass "i40iw 模块已加载 (Intel X722 iWARP)"
    iwarp_driver_found=true
else
    info "i40iw 模块未加载 (Intel X722 驱动)"
fi

# Intel irdma 驱动 (新版，同时支持 RoCE 和 iWARP)
if lsmod 2>/dev/null | grep -q "irdma"; then
    pass "irdma 模块已加载 (Intel E810/新一代 RDMA)"
    iwarp_driver_found=true
else
    info "irdma 模块未加载 (Intel 新一代 RDMA 驱动)"
fi

if ! $iwarp_driver_found; then
    warn "未检测到 iWARP 驱动模块"
    info "iWARP 需要特定硬件支持 (Chelsio T5/T6 或 Intel X722/E810)"
    info "注意: SoftRoCE (rxe) 不是 iWARP，它是 RoCE 的软件实现"
fi

# ========== 2. 检查 iWARP 设备 (通过 node_type) ==========
separator "2. iWARP 设备检测"

iwarp_found=false

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")
        node_type_file="$dev_dir/node_type"

        if [ -f "$node_type_file" ]; then
            node_type=$(cat "$node_type_file" 2>/dev/null || echo "")
            echo -e "  设备 ${BOLD}${dev_name}${NC}: node_type = ${node_type}"

            # iWARP 设备的 node_type 包含 "RNIC"
            if echo "$node_type" | grep -qi "RNIC"; then
                pass "${dev_name} 是 iWARP 设备 (node_type=RNIC)"
                iwarp_found=true
            else
                info "${dev_name} 不是 iWARP (node_type=${node_type})"
            fi
        fi
    done

    if ! $iwarp_found; then
        info "未发现 iWARP 设备 (IBV_NODE_RNIC)"
        info "大部分云环境使用 RoCE，iWARP 设备较少见"
    fi
else
    fail "/sys/class/infiniband/ 不存在"
fi

# ========== 3. 使用 rdma link 显示信息 ==========
separator "3. RDMA 链路信息"

if command -v rdma &>/dev/null; then
    output=$(rdma link 2>&1) || true
    if [ -n "$output" ]; then
        pass "rdma link 输出:"
        echo "$output" | while IFS= read -r line; do
            echo -e "    $line"
        done
    else
        warn "rdma link 无输出"
    fi
else
    warn "rdma 命令不可用 (请安装 iproute2)"
fi

# ========== 4. ibv_devinfo 详细信息 ==========
separator "4. 设备详细信息 (ibv_devinfo)"

if command -v ibv_devinfo &>/dev/null; then
    output=$(ibv_devinfo 2>&1) || true
    if [ -n "$output" ]; then
        echo "$output" | while IFS= read -r line; do
            # 高亮 transport 和 node_type 字段
            if echo "$line" | grep -qi "transport\|node_type"; then
                echo -e "    ${BOLD}${line}${NC}"
            else
                echo -e "    $line"
            fi
        done
    fi
else
    warn "ibv_devinfo 命令不可用"
fi

# ========== 总结 ==========
separator "检查完成"

if $iwarp_found; then
    pass "发现 iWARP 设备，可以使用 iWARP 编程"
    info "iWARP 建议使用 RDMA CM (rdma_cma.h) 建立连接"
else
    info "未发现 iWARP 设备"
    info "iWARP 与 IB/RoCE 的核心区别:"
    echo -e "    - node_type: ${YELLOW}IBV_NODE_RNIC${NC} (而非 IBV_NODE_CA)"
    echo -e "    - 底层传输: ${YELLOW}TCP${NC} (而非 IB 原生或 UDP)"
    echo -e "    - 建连方式: 推荐使用 ${YELLOW}RDMA CM${NC} (因为底层是 TCP 流)"
    echo -e "    - 无损网络: ${GREEN}不需要${NC} (TCP 自带重传)"
fi
