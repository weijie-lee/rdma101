#!/bin/bash
#
# RoCE (RDMA over Converged Ethernet) 环境检查脚本
#
# 功能：
#   - 显示 GID 表内容 (sysfs)
#   - 检查 PFC (Priority Flow Control) 配置
#   - 检查 ECN (Explicit Congestion Notification) 配置
#   - 显示 RoCE 特定设置 (GID 类型、DSCP 等)
#   - 检查 SoftRoCE (rxe) 模块状态
#
# 用法: chmod +x roce_env_check.sh && ./roce_env_check.sh
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

# ========== 1. 检查 RDMA 设备和链路层类型 ==========
separator "1. RoCE 设备检测"

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
                pass "发现 RoCE 设备: ${dev_name} 端口 ${port_num} (link_layer=Ethernet)"
                roce_devices+=("${dev_name}:${port_num}")
            elif [ "$link_layer" = "InfiniBand" ]; then
                info "设备 ${dev_name} 端口 ${port_num} 是 InfiniBand (非 RoCE)"
            fi
        done
    done

    if [ ${#roce_devices[@]} -eq 0 ]; then
        fail "未发现 RoCE 设备"
        info "如需创建 SoftRoCE 设备，请执行:"
        echo -e "       ${YELLOW}sudo rdma link add rxe_0 type rxe netdev eth0${NC}"
    fi
else
    fail "/sys/class/infiniband/ 不存在"
fi

# ========== 2. GID 表内容 ==========
separator "2. GID 表内容"

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")

        for port_dir in "$dev_dir"/ports/*/; do
            [ -d "$port_dir" ] || continue
            port_num=$(basename "$port_dir")
            link_layer=$(cat "$port_dir/link_layer" 2>/dev/null || echo "")

            # 仅显示 Ethernet (RoCE) 端口的 GID
            [ "$link_layer" = "Ethernet" ] || continue

            echo -e "  ${BOLD}${dev_name} / 端口 ${port_num} GID 表:${NC}"

            gid_dir="$port_dir/gids"
            gid_type_dir="$port_dir/gid_attrs/types"
            gid_count=0

            for gid_file in "$gid_dir"/*; do
                [ -f "$gid_file" ] || continue
                gid_index=$(basename "$gid_file")
                gid_value=$(cat "$gid_file" 2>/dev/null || echo "")

                # 跳过全零 GID
                if [ -n "$gid_value" ] && [ "$gid_value" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
                    # 尝试读取 GID 类型 (RoCE v1 / RoCE v2)
                    gid_type="N/A"
                    if [ -f "$gid_type_dir/$gid_index" ]; then
                        gid_type=$(cat "$gid_type_dir/$gid_index" 2>/dev/null || echo "N/A")
                    fi

                    # 根据 GID 类型着色
                    if echo "$gid_type" | grep -qi "v2\|rocev2"; then
                        echo -e "    GID[${gid_index}] = ${GREEN}${gid_value}${NC}  类型: ${GREEN}${gid_type}${NC}  ← 推荐用于 RoCE v2"
                    else
                        echo -e "    GID[${gid_index}] = ${YELLOW}${gid_value}${NC}  类型: ${YELLOW}${gid_type}${NC}"
                    fi
                    gid_count=$((gid_count + 1))
                fi
            done

            if [ "$gid_count" -eq 0 ]; then
                warn "无有效 GID (端口可能未配置 IP 地址)"
            else
                info "共 ${gid_count} 个有效 GID"
            fi
            echo ""
        done
    done
fi

# ========== 3. PFC (Priority Flow Control) 配置 ==========
separator "3. PFC (优先级流控) 检查"

# 方法 1: 使用 mlnx_qos (Mellanox 专用)
if command -v mlnx_qos &>/dev/null; then
    pass "mlnx_qos 工具可用"
    info "运行 'mlnx_qos -i <netdev>' 可查看 PFC 配置"
    # 尝试获取第一个 RDMA 网卡对应的网络接口
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")
        netdev_file="$dev_dir/device/net"
        if [ -d "$netdev_file" ]; then
            for netdev in "$netdev_file"/*/; do
                netdev_name=$(basename "$netdev")
                info "设备 ${dev_name} 对应网卡: ${netdev_name}"
                # 不自动运行 mlnx_qos，避免权限问题
                echo -e "       ${YELLOW}运行: sudo mlnx_qos -i ${netdev_name}${NC}"
            done
        fi
    done
else
    info "mlnx_qos 不可用 (非 Mellanox 设备或未安装 mlnx-tools)"
fi

# 方法 2: 使用 lldptool
if command -v lldptool &>/dev/null; then
    pass "lldptool 工具可用"
    info "运行 'lldptool -t -i <netdev> -V PFC' 可查看 PFC 状态"
else
    info "lldptool 不可用 (lldpad 包未安装)"
fi

# 方法 3: 检查 sysfs 中的 DCB (Data Center Bridging) 信息
info "检查 DCB/PFC sysfs 信息..."
pfc_found=false
for net_dir in /sys/class/net/*/; do
    [ -d "$net_dir" ] || continue
    netdev_name=$(basename "$net_dir")
    dcb_dir="$net_dir/ieee8021/dcb"
    if [ -d "$dcb_dir" ]; then
        pass "网卡 ${netdev_name} 支持 DCB"
        pfc_found=true
    fi
done
if ! $pfc_found; then
    info "未检测到 DCB 配置 (SoftRoCE 环境无需 PFC)"
fi

# ========== 4. ECN (Explicit Congestion Notification) ==========
separator "4. ECN (显式拥塞通知) 检查"

# 检查 TCP ECN 设置
if [ -f /proc/sys/net/ipv4/tcp_ecn ]; then
    ecn_value=$(cat /proc/sys/net/ipv4/tcp_ecn 2>/dev/null)
    case "$ecn_value" in
        0) warn "tcp_ecn = 0 (ECN 已禁用)" ;;
        1) pass "tcp_ecn = 1 (ECN 已启用，发起连接时请求)" ;;
        2) pass "tcp_ecn = 2 (ECN 已启用，仅作为服务端响应)" ;;
        *) info "tcp_ecn = $ecn_value" ;;
    esac
else
    warn "无法读取 tcp_ecn 设置"
fi

# 检查 RoCE ECN 相关参数 (Mellanox 设备)
for dev_dir in /sys/class/infiniband/*/; do
    [ -d "$dev_dir" ] || continue
    dev_name=$(basename "$dev_dir")
    ecn_dir="$dev_dir/ecn"
    if [ -d "$ecn_dir" ]; then
        pass "设备 ${dev_name} 支持 ECN 配置"
        for ecn_file in "$ecn_dir"/*; do
            [ -f "$ecn_file" ] || continue
            ecn_param=$(basename "$ecn_file")
            ecn_val=$(cat "$ecn_file" 2>/dev/null || echo "N/A")
            echo -e "    ${ecn_param} = ${ecn_val}"
        done
    fi
done

# ========== 5. RoCE 特定设置 ==========
separator "5. RoCE 相关内核模块"

# 检查 SoftRoCE (rxe) 模块
if lsmod 2>/dev/null | grep -q "rdma_rxe"; then
    pass "rdma_rxe 模块已加载 (SoftRoCE)"
elif lsmod 2>/dev/null | grep -q "rxe"; then
    pass "rxe 相关模块已加载"
else
    info "rdma_rxe 模块未加载 (如需 SoftRoCE: sudo modprobe rdma_rxe)"
fi

# 检查 Mellanox 驱动
if lsmod 2>/dev/null | grep -q "mlx5_ib"; then
    pass "mlx5_ib 模块已加载 (Mellanox ConnectX)"
fi

if lsmod 2>/dev/null | grep -q "mlx4_ib"; then
    pass "mlx4_ib 模块已加载 (Mellanox ConnectX-3)"
fi

# 检查 RoCE 模式设置 (Mellanox 设备)
for dev_dir in /sys/class/infiniband/*/; do
    [ -d "$dev_dir" ] || continue
    dev_name=$(basename "$dev_dir")

    for port_dir in "$dev_dir"/ports/*/; do
        [ -d "$port_dir" ] || continue
        port_num=$(basename "$port_dir")

        # 检查 RoCE 版本 (如果支持)
        roce_type_file="$port_dir/gid_attrs/types/0"
        if [ -f "$roce_type_file" ]; then
            default_type=$(cat "$roce_type_file" 2>/dev/null || echo "N/A")
            info "设备 ${dev_name} 端口 ${port_num} 默认 GID[0] 类型: ${default_type}"
        fi
    done
done

# ========== 6. 网络接口状态 ==========
separator "6. 关联网络接口"

if [ -d /sys/class/infiniband ]; then
    for dev_dir in /sys/class/infiniband/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name=$(basename "$dev_dir")

        # 尝试找到关联的网络接口
        netdev_path="$dev_dir/device/net"
        if [ -d "$netdev_path" ]; then
            for netdev in "$netdev_path"/*/; do
                [ -d "$netdev" ] || continue
                netdev_name=$(basename "$netdev")
                echo -e "  ${BOLD}${dev_name} → ${netdev_name}${NC}"

                # 显示 IP 地址
                if command -v ip &>/dev/null; then
                    ip_info=$(ip addr show "$netdev_name" 2>/dev/null | grep "inet " || true)
                    if [ -n "$ip_info" ]; then
                        echo -e "    IP: ${GREEN}$(echo "$ip_info" | awk '{print $2}')${NC}"
                    else
                        warn "  ${netdev_name} 无 IPv4 地址 (GID 表可能不完整)"
                    fi
                fi

                # 显示 MTU
                mtu=$(cat "/sys/class/net/${netdev_name}/mtu" 2>/dev/null || echo "N/A")
                echo -e "    MTU: ${mtu}"

                # 显示链路状态
                operstate=$(cat "/sys/class/net/${netdev_name}/operstate" 2>/dev/null || echo "N/A")
                if [ "$operstate" = "up" ]; then
                    echo -e "    状态: ${GREEN}${operstate}${NC}"
                else
                    echo -e "    状态: ${RED}${operstate}${NC}"
                fi
            done
        else
            info "设备 ${dev_name} 无关联网络接口 (可能是虚拟设备)"
        fi
    done
fi

# ========== 总结 ==========
separator "检查完成"
info "RoCE 环境检查结束"
info "RoCE v2 推荐:"
echo -e "    1. 选择 RoCE v2 类型的 GID 索引 (通常为 1 或 3)"
echo -e "    2. 确保网络接口有 IP 地址"
echo -e "    3. 生产环境建议配置 PFC + ECN"
info "运行 C 程序 roce_gid_query 可获得更详细的 GID 分析"
