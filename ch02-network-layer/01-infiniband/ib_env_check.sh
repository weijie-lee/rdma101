#!/bin/bash
#
# IB (InfiniBand) 环境检查脚本
#
# 功能：
#   - 检查 IB 相关命令是否存在 (ibstat, ibv_devinfo, sminfo)
#   - 列出所有 RDMA 设备
#   - 显示设备详细信息
#   - 检查 Subnet Manager 状态
#   - 显示端口状态、LID、速率
#   - 显示 GID 表内容
#
# 用法: chmod +x ib_env_check.sh && ./ib_env_check.sh
#
set -e

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'  # 无颜色 (恢复默认)

# ========== 辅助函数 ==========

# 打印通过信息
pass() {
    echo -e "  [${GREEN}PASS${NC}] $1"
}

# 打印失败信息
fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
}

# 打印警告信息
warn() {
    echo -e "  [${YELLOW}WARN${NC}] $1"
}

# 打印信息
info() {
    echo -e "  [${BLUE}INFO${NC}] $1"
}

# 打印分隔线
separator() {
    echo -e "\n${CYAN}========== $1 ==========${NC}\n"
}

# ========== 1. 检查命令是否存在 ==========
separator "1. 检查 IB 工具命令"

# 检查 ibstat 命令
if command -v ibstat &>/dev/null; then
    pass "ibstat 命令存在: $(which ibstat)"
else
    warn "ibstat 命令不存在 (infiniband-diags 包未安装)"
fi

# 检查 ibv_devinfo 命令
if command -v ibv_devinfo &>/dev/null; then
    pass "ibv_devinfo 命令存在: $(which ibv_devinfo)"
else
    fail "ibv_devinfo 命令不存在 (请安装 libibverbs-utils 或 rdma-core)"
fi

# 检查 sminfo 命令
if command -v sminfo &>/dev/null; then
    pass "sminfo 命令存在: $(which sminfo)"
else
    warn "sminfo 命令不存在 (infiniband-diags 包未安装，仅 IB 网络需要)"
fi

# 检查 ibv_devices 命令
if command -v ibv_devices &>/dev/null; then
    pass "ibv_devices 命令存在: $(which ibv_devices)"
else
    fail "ibv_devices 命令不存在"
fi

# 检查 rdma 命令 (iproute2)
if command -v rdma &>/dev/null; then
    pass "rdma 命令存在: $(which rdma)"
else
    warn "rdma 命令不存在 (请安装 iproute2)"
fi

# ========== 2. 列出所有 RDMA 设备 ==========
separator "2. RDMA 设备列表 (ibv_devices)"

if command -v ibv_devices &>/dev/null; then
    output=$(ibv_devices 2>&1) || true
    if echo "$output" | grep -q "device"; then
        pass "发现 RDMA 设备:"
        echo "$output" | while IFS= read -r line; do
            echo -e "       $line"
        done
    else
        fail "未发现任何 RDMA 设备"
        echo -e "       ${YELLOW}提示: 请确认 RDMA 驱动已加载 (modprobe rdma_rxe / mlx5_ib)${NC}"
    fi
else
    fail "无法执行 ibv_devices"
fi

# ========== 3. 设备详细信息 ==========
separator "3. 设备详细信息 (ibv_devinfo)"

if command -v ibv_devinfo &>/dev/null; then
    output=$(ibv_devinfo 2>&1) || true
    if [ -n "$output" ]; then
        pass "设备详细信息:"
        echo "$output" | while IFS= read -r line; do
            # 高亮关键字段
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
        fail "ibv_devinfo 无输出"
    fi
else
    fail "无法执行 ibv_devinfo"
fi

# ========== 4. Subnet Manager 状态检查 ==========
separator "4. Subnet Manager (SM) 状态"

sm_found=false

# 方法 1: 使用 sminfo
if command -v sminfo &>/dev/null; then
    info "尝试使用 sminfo 查询 SM 状态..."
    output=$(sminfo 2>&1) || true
    if echo "$output" | grep -q "SM"; then
        pass "Subnet Manager 信息:"
        echo -e "       $output"
        sm_found=true
    else
        warn "sminfo 查询失败 (可能不是 IB 网络): $output"
    fi
fi

# 方法 2: 使用 ibstat
if ! $sm_found && command -v ibstat &>/dev/null; then
    info "尝试使用 ibstat 查询..."
    output=$(ibstat 2>&1) || true
    if [ -n "$output" ]; then
        pass "ibstat 输出:"
        echo "$output" | while IFS= read -r line; do
            echo -e "       $line"
        done
    else
        warn "ibstat 无输出"
    fi
fi

if ! $sm_found; then
    info "SM 仅在 InfiniBand 网络中需要，RoCE/iWARP 不需要 SM"
fi

# ========== 5. 端口状态、LID、速率 ==========
separator "5. 端口状态详情"

# 遍历 /sys/class/infiniband/ 下所有设备和端口
if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        dev_name=$(basename "$dev_dir")
        echo -e "  ${BOLD}设备: $dev_name${NC}"

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")

            # 读取端口状态
            state=$(cat "$port_dir/state" 2>/dev/null || echo "unknown")
            lid=$(cat "$port_dir/lid" 2>/dev/null || echo "N/A")
            sm_lid=$(cat "$port_dir/sm_lid" 2>/dev/null || echo "N/A")
            rate=$(cat "$port_dir/rate" 2>/dev/null || echo "N/A")
            link_layer=$(cat "$port_dir/link_layer" 2>/dev/null || echo "N/A")
            phys_state=$(cat "$port_dir/phys_state" 2>/dev/null || echo "N/A")

            echo -e "    端口 ${port_num}:"

            # 状态判断
            if echo "$state" | grep -q "ACTIVE"; then
                echo -e "      状态:      ${GREEN}${state}${NC}"
            else
                echo -e "      状态:      ${RED}${state}${NC}"
            fi

            echo -e "      物理状态:  ${phys_state}"
            echo -e "      LID:       ${lid}"
            echo -e "      SM LID:    ${sm_lid}"
            echo -e "      速率:      ${rate}"
            echo -e "      链路层:    ${CYAN}${link_layer}${NC}"
        done
        echo ""
    done
else
    fail "/sys/class/infiniband/ 目录不存在，未加载 RDMA 驱动"
fi

# ========== 6. GID 表内容 ==========
separator "6. GID 表内容"

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        dev_name=$(basename "$dev_dir")

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")
            gid_dir="$port_dir/gids"

            [ -d "$gid_dir" ] || continue

            echo -e "  ${BOLD}${dev_name} / 端口 ${port_num} GID 表:${NC}"

            gid_count=0
            for gid_file in "$gid_dir"/*; do
                [ -f "$gid_file" ] || continue
                gid_index=$(basename "$gid_file")
                gid_value=$(cat "$gid_file" 2>/dev/null || echo "")

                # 跳过全零 GID
                if [ -n "$gid_value" ] && [ "$gid_value" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
                    echo -e "    GID[${gid_index}] = ${GREEN}${gid_value}${NC}"
                    gid_count=$((gid_count + 1))
                fi
            done

            if [ "$gid_count" -eq 0 ]; then
                warn "端口 ${port_num} 无有效 GID 条目"
            else
                info "端口 ${port_num} 共 ${gid_count} 个有效 GID"
            fi
            echo ""
        done
    done
else
    fail "无法读取 GID 表"
fi

# ========== 总结 ==========
separator "检查完成"
info "本脚本检查了 InfiniBand 环境的基本状态"
info "如果你使用的是 RoCE 环境，请运行 ../02-roce/roce_env_check.sh"
info "如果你使用的是 iWARP 环境，请运行 ../03-iwarp/iwarp_env_check.sh"
