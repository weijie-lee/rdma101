#!/bin/bash
#
# RDMA 一键环境诊断脚本
#
# 功能：
#   - 检查 RDMA 内核模块加载状态
#   - 检查 RDMA 设备和端口信息
#   - 检查 rdma 工具输出
#   - 检查 SoftRoCE (RXE) 配置
#   - 检查设备文件、内存锁限制
#   - 检查 dmesg 中的 RDMA 相关日志
#   - 检查 perftest 工具可用性
#   - 生成诊断汇总报告
#
# 用法: ./rdma_diag.sh
#

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== 计数器 ==========
PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

# ========== 辅助函数 ==========

# 打印检查结果
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
    echo -e "  ${BLUE}[信息]${NC} $*"
}

# 打印章节标题
section() {
    echo ""
    echo -e "${BOLD}${CYAN}━━━ $* ━━━${NC}"
}

# 检查内核模块是否加载
check_module() {
    local mod=$1
    local desc=$2
    if lsmod 2>/dev/null | grep -qw "$mod"; then
        pass_msg "$mod ($desc) - 已加载"
    else
        warn_msg "$mod ($desc) - 未加载"
    fi
}

# 检查命令是否存在
check_command() {
    local cmd=$1
    local desc=$2
    if command -v "$cmd" &>/dev/null; then
        pass_msg "$cmd ($desc) - 已安装"
        return 0
    else
        warn_msg "$cmd ($desc) - 未安装"
        return 1
    fi
}

# ========== 检查项 ==========

# (a) 检查内核模块
check_kernel_modules() {
    section "检查 RDMA 内核模块"

    check_module "ib_core"    "RDMA 核心模块"
    check_module "ib_uverbs"  "用户态 Verbs 接口"
    check_module "rdma_cm"    "RDMA 连接管理"
    check_module "rdma_rxe"   "SoftRoCE (软件模拟 RoCE)"
    check_module "mlx5_ib"    "Mellanox ConnectX-5/6 驱动"
    check_module "mlx4_ib"    "Mellanox ConnectX-3/4 驱动"
    check_module "irdma"      "Intel RDMA 驱动"

    # 额外: 检查 rdma_ucm
    check_module "rdma_ucm"   "RDMA CM 用户态接口"
}

# (b) 检查 ibv_devices / ibv_devinfo
check_verbs_devices() {
    section "检查 RDMA 设备 (ibv_devices / ibv_devinfo)"

    if ! command -v ibv_devices &>/dev/null; then
        fail_msg "ibv_devices 命令不存在 (请安装 libibverbs-utils 或 rdma-core)"
        return
    fi

    local dev_output
    dev_output=$(ibv_devices 2>&1)
    local dev_count
    dev_count=$(echo "$dev_output" | awk 'NR>2 && NF>0' | wc -l)

    if [ "$dev_count" -gt 0 ]; then
        pass_msg "检测到 $dev_count 个 RDMA 设备"
        echo "$dev_output" | awk 'NR>2 && NF>0 {print "         " $0}'
    else
        fail_msg "未检测到任何 RDMA 设备"
        info_msg "提示: 如需使用 SoftRoCE，请运行: rdma link add rxe0 type rxe netdev <网卡名>"
    fi

    # 打印 ibv_devinfo 摘要
    if [ "$dev_count" -gt 0 ] && command -v ibv_devinfo &>/dev/null; then
        echo ""
        info_msg "设备详情 (ibv_devinfo):"
        ibv_devinfo 2>/dev/null | head -30 | sed 's/^/         /'
    fi
}

# (c) 检查 rdma link / rdma dev
check_rdma_tool() {
    section "检查 rdma 工具输出"

    if ! command -v rdma &>/dev/null; then
        warn_msg "rdma 命令不存在 (请安装 iproute2-rdma 或 iproute2)"
        return
    fi

    # rdma dev
    info_msg "rdma dev 输出:"
    local rdma_dev
    rdma_dev=$(rdma dev 2>&1)
    if [ -n "$rdma_dev" ]; then
        echo "$rdma_dev" | sed 's/^/         /'
        pass_msg "rdma dev 命令正常"
    else
        warn_msg "rdma dev 无输出"
    fi

    # rdma link
    echo ""
    info_msg "rdma link 输出:"
    local rdma_link
    rdma_link=$(rdma link 2>&1)
    if [ -n "$rdma_link" ]; then
        echo "$rdma_link" | sed 's/^/         /'
        pass_msg "rdma link 命令正常"
    else
        warn_msg "rdma link 无输出"
    fi
}

# (d) 检查端口状态
check_port_state() {
    section "检查端口状态"

    if ! command -v ibv_devinfo &>/dev/null; then
        fail_msg "ibv_devinfo 不可用，无法检查端口状态"
        return
    fi

    # 解析 ibv_devinfo 中的端口状态
    local port_info
    port_info=$(ibv_devinfo 2>/dev/null | grep -E "(hca_id|port:|state:)")

    if [ -z "$port_info" ]; then
        warn_msg "无法获取端口信息"
        return
    fi

    # 逐行检查
    local current_dev=""
    while IFS= read -r line; do
        if echo "$line" | grep -q "hca_id:"; then
            current_dev=$(echo "$line" | awk '{print $2}')
        fi
        if echo "$line" | grep -q "state:"; then
            local state
            state=$(echo "$line" | awk '{print $2}')
            if [ "$state" = "PORT_ACTIVE" ]; then
                pass_msg "$current_dev 端口状态: ${GREEN}ACTIVE${NC}"
            elif [ "$state" = "PORT_DOWN" ]; then
                fail_msg "$current_dev 端口状态: ${RED}DOWN${NC}"
                info_msg "  端口 DOWN 可能原因: 网线未连接 / 交换机端口关闭 / 驱动问题"
            else
                warn_msg "$current_dev 端口状态: $state"
            fi
        fi
    done <<< "$port_info"
}

# (e) 检查 LID / GID 表
check_lid_gid() {
    section "检查 LID / GID 信息"

    if ! command -v ibv_devinfo &>/dev/null; then
        fail_msg "ibv_devinfo 不可用"
        return
    fi

    # LID
    local lid_info
    lid_info=$(ibv_devinfo 2>/dev/null | grep "lid:")
    if [ -n "$lid_info" ]; then
        local lid_val
        lid_val=$(echo "$lid_info" | head -1 | awk '{print $2}')
        if [ "$lid_val" = "0" ] || [ "$lid_val" = "0x0000" ]; then
            info_msg "LID = 0 (这是 RoCE 模式的正常情况, RoCE 使用 GID 寻址)"
        else
            pass_msg "LID = $lid_val (IB 模式, SM 已分配 LID)"
        fi
    fi

    # GID[0]
    local gid_info
    gid_info=$(ibv_devinfo -v 2>/dev/null | grep "GID\[" | head -5)
    if [ -n "$gid_info" ]; then
        pass_msg "GID 表 (前 5 条):"
        echo "$gid_info" | sed 's/^/         /'
    else
        warn_msg "未能获取 GID 表信息"
    fi
}

# (f) 检查 SoftRoCE 配置
check_softrce() {
    section "检查 SoftRoCE (RXE) 配置"

    # 检查 rdma_rxe 模块
    if lsmod 2>/dev/null | grep -qw "rdma_rxe"; then
        pass_msg "rdma_rxe 模块已加载"
    else
        info_msg "rdma_rxe 模块未加载 (如需 SoftRoCE: modprobe rdma_rxe)"
    fi

    # 检查是否有 rxe 设备
    if command -v rdma &>/dev/null; then
        local rxe_devs
        rxe_devs=$(rdma link 2>/dev/null | grep -i "rxe")
        if [ -n "$rxe_devs" ]; then
            pass_msg "检测到 SoftRoCE 设备:"
            echo "$rxe_devs" | sed 's/^/         /'
        else
            info_msg "未检测到 SoftRoCE 设备"
            info_msg "创建方法: rdma link add rxe0 type rxe netdev <网卡名>"
        fi
    fi
}

# (g) 检查设备文件
check_device_files() {
    section "检查 /dev/infiniband/ 设备文件"

    if [ -d /dev/infiniband ]; then
        local files
        files=$(ls /dev/infiniband/ 2>/dev/null)
        if [ -n "$files" ]; then
            pass_msg "/dev/infiniband/ 目录存在"
            for f in $files; do
                local perms
                perms=$(ls -la "/dev/infiniband/$f" 2>/dev/null | awk '{print $1}')
                info_msg "  $f ($perms)"
            done

            # 检查 uverbs 设备
            if ls /dev/infiniband/uverbs* &>/dev/null; then
                pass_msg "uverbs 设备文件存在 (用户态 Verbs 可用)"
            else
                fail_msg "未找到 uverbs 设备文件"
            fi
        else
            fail_msg "/dev/infiniband/ 目录为空"
        fi
    else
        fail_msg "/dev/infiniband/ 目录不存在"
        info_msg "提示: 可能需要加载 ib_uverbs 模块: modprobe ib_uverbs"
    fi
}

# (h) 检查内存锁限制
check_memlock() {
    section "检查内存锁限制 (ulimit -l)"

    local memlock
    memlock=$(ulimit -l 2>/dev/null)

    if [ "$memlock" = "unlimited" ]; then
        pass_msg "memlock = unlimited (最佳配置)"
    elif [ -n "$memlock" ] && [ "$memlock" -ge 65536 ] 2>/dev/null; then
        pass_msg "memlock = ${memlock} KB (足够大多数 RDMA 程序)"
    elif [ -n "$memlock" ]; then
        warn_msg "memlock = ${memlock} KB (可能不足)"
        info_msg "RDMA 需要 pin 内存, 建议设置 unlimited:"
        info_msg "  ulimit -l unlimited"
        info_msg "  或编辑 /etc/security/limits.conf 添加:"
        info_msg "  * soft memlock unlimited"
        info_msg "  * hard memlock unlimited"
    else
        warn_msg "无法获取 memlock 限制"
    fi
}

# (i) 检查 dmesg 中的 RDMA 消息
check_dmesg() {
    section "检查 dmesg RDMA 相关日志"

    local dmesg_rdma
    dmesg_rdma=$(dmesg 2>/dev/null | grep -iE "(rdma|infiniband|ib_|rxe|mlx[45]|roce)" | tail -15)

    if [ -n "$dmesg_rdma" ]; then
        info_msg "最近的 RDMA 相关内核日志:"
        echo "$dmesg_rdma" | sed 's/^/         /'

        # 检查是否有错误
        if echo "$dmesg_rdma" | grep -qiE "(error|fail|warn)"; then
            warn_msg "dmesg 中存在 RDMA 相关的错误/警告消息"
        else
            pass_msg "dmesg 中无 RDMA 错误消息"
        fi
    else
        info_msg "dmesg 中未找到 RDMA 相关日志 (可能需要 root 权限)"
    fi
}

# (j) 检查 perftest 工具
check_perftest() {
    section "检查 perftest 工具"

    local tools=(ib_send_lat ib_send_bw ib_write_lat ib_write_bw ib_read_lat ib_read_bw)
    local installed=0
    local total=${#tools[@]}

    for tool in "${tools[@]}"; do
        if command -v "$tool" &>/dev/null; then
            installed=$((installed + 1))
        fi
    done

    if [ $installed -eq $total ]; then
        pass_msg "perftest 工具已完整安装 ($installed/$total)"
    elif [ $installed -gt 0 ]; then
        warn_msg "perftest 工具部分安装 ($installed/$total)"
    else
        warn_msg "perftest 工具未安装 (建议: apt install perftest)"
    fi
}

# ========== 汇总报告 ==========
print_summary() {
    echo ""
    echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║          RDMA 环境诊断汇总           ║${NC}"
    echo -e "${BOLD}${CYAN}╚═══════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  ${GREEN}通过: $PASS_COUNT${NC}"
    echo -e "  ${YELLOW}警告: $WARN_COUNT${NC}"
    echo -e "  ${RED}失败: $FAIL_COUNT${NC}"
    echo ""

    local total=$((PASS_COUNT + WARN_COUNT + FAIL_COUNT))
    if [ $FAIL_COUNT -eq 0 ] && [ $WARN_COUNT -eq 0 ]; then
        echo -e "  ${GREEN}${BOLD}诊断结论: RDMA 环境完全正常!${NC}"
    elif [ $FAIL_COUNT -eq 0 ]; then
        echo -e "  ${YELLOW}${BOLD}诊断结论: RDMA 基本可用，但有 $WARN_COUNT 个警告需关注${NC}"
    else
        echo -e "  ${RED}${BOLD}诊断结论: 存在 $FAIL_COUNT 个问题需要修复${NC}"
        echo ""
        echo -e "  ${BOLD}常见修复步骤:${NC}"
        echo "  1. 加载内核模块: modprobe ib_core ib_uverbs"
        echo "  2. 创建 SoftRoCE: rdma link add rxe0 type rxe netdev eth0"
        echo "  3. 设置内存限制: ulimit -l unlimited"
        echo "  4. 安装工具包:   apt install libibverbs-dev perftest"
    fi
    echo ""
}

# ========== 主流程 ==========
main() {
    echo ""
    echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║     RDMA 环境一键诊断工具             ║${NC}"
    echo -e "${BOLD}${CYAN}║     检查模块/设备/端口/GID/配额       ║${NC}"
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
