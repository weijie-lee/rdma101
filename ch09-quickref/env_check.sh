#!/bin/bash
# =============================================================================
# RDMA 环境一键检查脚本
#
# 功能: 10 项检查，全面验证 RDMA 开发/运行环境是否就绪
#
# 用法: bash env_check.sh
#
# 检查项:
#   1. RDMA 内核模块是否加载
#   2. ibv_devices 能否看到设备
#   3. 至少一个端口处于 ACTIVE 状态
#   4. LID 已分配 (IB) 或 GID 有效 (RoCE)
#   5. SM 运行中 (仅 IB 模式)
#   6. /dev/infiniband/ 设备文件存在
#   7. ulimit -l 内存锁定限制足够
#   8. libibverbs 已安装
#   9. rdma-core 版本
#   10. perftest 工具可用
# =============================================================================

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ========== 计数器 ==========
PASS=0
FAIL=0
TOTAL=10

# ========== 辅助函数 ==========

print_pass() {
    echo -e "  [${GREEN}PASS${NC}] $1"
    PASS=$((PASS + 1))
}

print_fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
    FAIL=$((FAIL + 1))
}

print_fix() {
    echo -e "         ${YELLOW}修复: $1${NC}"
}

print_info() {
    echo -e "         ${BLUE}↳ $1${NC}"
}

# ========== 标题 ==========
echo ""
echo -e "${CYAN}================================================${NC}"
echo -e "${CYAN}      RDMA 环境一键检查 (10 项)${NC}"
echo -e "${CYAN}================================================${NC}"
echo ""

# ========== 检查 1: RDMA 内核模块 ==========
echo -e "${CYAN}[1/10] RDMA 内核模块${NC}"

# 检查关键的 RDMA 内核模块
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
    print_pass "RDMA 内核模块已加载 ($LOADED 个核心模块)"
    # 显示已加载的 RDMA 相关模块
    lsmod 2>/dev/null | grep -E "^(ib_|rdma_|mlx|rxe)" | while IFS= read -r line; do
        print_info "$(echo "$line" | awk '{print $1}')"
    done
else
    print_fail "RDMA 内核模块不完整 (缺少:$MISSING)"
    print_fix "modprobe ib_core ib_uverbs rdma_ucm"
    print_fix "如果使用 SoftRoCE: modprobe rdma_rxe"
fi

echo ""

# ========== 检查 2: ibv_devices ==========
echo -e "${CYAN}[2/10] RDMA 设备列表${NC}"

if ! command -v ibv_devices &>/dev/null; then
    print_fail "ibv_devices 命令未安装"
    print_fix "apt install ibverbs-utils 或 yum install libibverbs-utils"
else
    DEVICE_OUTPUT=$(ibv_devices 2>/dev/null)
    DEVICE_COUNT=$(echo "$DEVICE_OUTPUT" | grep -c "^\s")

    if [ "$DEVICE_COUNT" -gt 0 ]; then
        print_pass "找到 $DEVICE_COUNT 个 RDMA 设备"
        echo "$DEVICE_OUTPUT" | grep "^\s" | while IFS= read -r line; do
            print_info "$(echo "$line" | xargs)"
        done
    else
        print_fail "没有找到 RDMA 设备"
        print_fix "物理网卡: 检查 Mellanox 驱动是否安装"
        print_fix "SoftRoCE: rdma link add rxe_0 type rxe netdev eth0"
    fi
fi

echo ""

# ========== 检查 3: 端口状态 ==========
echo -e "${CYAN}[3/10] 端口状态 (至少一个 ACTIVE)${NC}"

if command -v ibv_devinfo &>/dev/null; then
    ACTIVE_COUNT=$(ibv_devinfo 2>/dev/null | grep -c "PORT_ACTIVE")

    if [ "$ACTIVE_COUNT" -gt 0 ]; then
        print_pass "$ACTIVE_COUNT 个端口处于 ACTIVE 状态"
    else
        print_fail "没有 ACTIVE 端口"
        print_fix "检查网线连接和交换机配置"
        print_fix "SoftRoCE: 确保底层网口 (eth0) 是 UP 状态"
    fi
else
    print_fail "ibv_devinfo 未安装，无法检查端口状态"
    print_fix "apt install ibverbs-utils"
fi

echo ""

# ========== 检查 4: LID (IB) 或 GID (RoCE) ==========
echo -e "${CYAN}[4/10] 寻址信息 (LID / GID)${NC}"

if command -v ibv_devinfo &>/dev/null; then
    # 检查 link_layer 确定模式
    LINK_LAYER=$(ibv_devinfo 2>/dev/null | grep "link_layer" | head -1 | awk '{print $2}')
    LID=$(ibv_devinfo 2>/dev/null | grep "sm_lid" | head -1 | awk '{print $2}')

    if [ "$LINK_LAYER" = "Ethernet" ]; then
        # RoCE 模式: 检查 GID
        # 获取第一个设备名
        DEV_NAME=$(ibv_devinfo -l 2>/dev/null | grep "^\s" | head -1 | xargs)
        GID_PATH="/sys/class/infiniband/$DEV_NAME/ports/1/gids/0"
        if [ -f "$GID_PATH" ]; then
            GID_VAL=$(cat "$GID_PATH" 2>/dev/null)
            if [ -n "$GID_VAL" ] && [ "$GID_VAL" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
                print_pass "RoCE 模式 — GID 有效: $GID_VAL"
            else
                print_fail "RoCE 模式 — GID 无效 (全零)"
                print_fix "检查网络接口 IP 配置，GID 通常由 IPv4/IPv6 地址生成"
            fi
        else
            print_fail "无法读取 GID 表"
            print_fix "检查 /sys/class/infiniband/ 目录"
        fi
    else
        # IB 模式: 检查 LID
        PORT_LID=$(ibv_devinfo 2>/dev/null | grep "^\s*lid:" | head -1 | awk '{print $2}')
        if [ -n "$PORT_LID" ] && [ "$PORT_LID" != "0" ] && [ "$PORT_LID" != "0x0000" ]; then
            print_pass "IB 模式 — LID 已分配: $PORT_LID"
        else
            print_fail "IB 模式 — LID 未分配 (为 0)"
            print_fix "检查 Subnet Manager (opensm) 是否运行"
        fi
    fi
else
    print_fail "无法检查寻址信息"
fi

echo ""

# ========== 检查 5: Subnet Manager (仅 IB) ==========
echo -e "${CYAN}[5/10] Subnet Manager (仅 IB 模式需要)${NC}"

LINK_LAYER=$(ibv_devinfo 2>/dev/null | grep "link_layer" | head -1 | awk '{print $2}')

if [ "$LINK_LAYER" = "Ethernet" ]; then
    print_pass "RoCE 模式，不需要 Subnet Manager"
else
    # IB 模式: 检查 SM
    SM_LID=$(ibv_devinfo 2>/dev/null | grep "sm_lid" | head -1 | awk '{print $2}')
    if [ -n "$SM_LID" ] && [ "$SM_LID" != "0" ] && [ "$SM_LID" != "0x0000" ]; then
        print_pass "Subnet Manager 运行中 (SM LID: $SM_LID)"
    else
        # 检查 opensm 进程
        if pgrep -x "opensm" &>/dev/null; then
            print_pass "opensm 进程运行中"
        else
            print_fail "Subnet Manager 未运行"
            print_fix "启动: systemctl start opensm"
            print_fix "或: opensm -B (后台运行)"
        fi
    fi
fi

echo ""

# ========== 检查 6: /dev/infiniband/ ==========
echo -e "${CYAN}[6/10] /dev/infiniband/ 设备文件${NC}"

if [ -d "/dev/infiniband" ]; then
    UVERBS_COUNT=$(ls /dev/infiniband/uverbs* 2>/dev/null | wc -l)
    if [ "$UVERBS_COUNT" -gt 0 ]; then
        print_pass "/dev/infiniband/ 存在 ($UVERBS_COUNT 个 uverbs 设备)"
        ls /dev/infiniband/ 2>/dev/null | while IFS= read -r f; do
            print_info "$f"
        done
    else
        print_fail "/dev/infiniband/ 存在但无 uverbs 设备"
        print_fix "检查 ib_uverbs 模块: modprobe ib_uverbs"
    fi
else
    print_fail "/dev/infiniband/ 目录不存在"
    print_fix "RDMA 内核模块未正确加载"
    print_fix "modprobe ib_core ib_uverbs"
fi

echo ""

# ========== 检查 7: ulimit -l ==========
echo -e "${CYAN}[7/10] 锁定内存限制 (ulimit -l)${NC}"

MEMLOCK=$(ulimit -l 2>/dev/null)

if [ "$MEMLOCK" = "unlimited" ]; then
    print_pass "锁定内存限制: unlimited (最佳)"
elif [ -n "$MEMLOCK" ] && [ "$MEMLOCK" -ge 65536 ] 2>/dev/null; then
    print_pass "锁定内存限制: ${MEMLOCK} KB (>= 64MB，足够)"
else
    print_fail "锁定内存限制不足: ${MEMLOCK} KB (需要 >= 65536 KB)"
    print_fix "临时: ulimit -l unlimited"
    print_fix "永久: 编辑 /etc/security/limits.conf 添加:"
    print_fix "  * soft memlock unlimited"
    print_fix "  * hard memlock unlimited"
fi

echo ""

# ========== 检查 8: libibverbs ==========
echo -e "${CYAN}[8/10] libibverbs 库${NC}"

LIBIBVERBS=$(ldconfig -p 2>/dev/null | grep "libibverbs.so" | head -1)

if [ -n "$LIBIBVERBS" ]; then
    print_pass "libibverbs 已安装"
    print_info "$LIBIBVERBS"
    # 检查开发头文件
    if [ -f "/usr/include/infiniband/verbs.h" ]; then
        print_info "头文件: /usr/include/infiniband/verbs.h ✓"
    else
        print_info "警告: 开发头文件未安装 (需要 libibverbs-dev)"
    fi
else
    print_fail "libibverbs 未安装"
    print_fix "apt install libibverbs-dev 或 yum install libibverbs-devel"
fi

echo ""

# ========== 检查 9: rdma-core 版本 ==========
echo -e "${CYAN}[9/10] rdma-core 版本${NC}"

RDMA_CORE_VER=""
# Debian/Ubuntu
RDMA_CORE_VER=$(dpkg -l 2>/dev/null | grep "rdma-core" | head -1 | awk '{print $3}')
# CentOS/RHEL
if [ -z "$RDMA_CORE_VER" ]; then
    RDMA_CORE_VER=$(rpm -q rdma-core 2>/dev/null | grep -v "not installed")
fi

if [ -n "$RDMA_CORE_VER" ]; then
    print_pass "rdma-core 版本: $RDMA_CORE_VER"
else
    print_fail "rdma-core 未安装或无法确定版本"
    print_fix "apt install rdma-core 或 yum install rdma-core"
fi

echo ""

# ========== 检查 10: perftest ==========
echo -e "${CYAN}[10/10] perftest 工具 (性能测试)${NC}"

PERFTEST_TOOLS="ib_write_bw ib_write_lat ib_read_bw ib_send_bw"
FOUND=0

for tool in $PERFTEST_TOOLS; do
    if command -v "$tool" &>/dev/null; then
        FOUND=$((FOUND + 1))
    fi
done

if [ "$FOUND" -ge 3 ]; then
    print_pass "perftest 工具已安装 ($FOUND/4 个工具可用)"
elif [ "$FOUND" -gt 0 ]; then
    print_pass "perftest 部分安装 ($FOUND/4 个工具可用)"
else
    print_fail "perftest 工具未安装"
    print_fix "apt install perftest 或 yum install perftest"
fi

echo ""

# ========== 汇总 ==========
echo -e "${CYAN}================================================${NC}"
echo -e "${CYAN}      检查结果汇总${NC}"
echo -e "${CYAN}================================================${NC}"
echo ""
echo -e "  ${GREEN}通过: $PASS${NC}  /  ${RED}失败: $FAIL${NC}  /  总计: $TOTAL"
echo ""

if [ "$PASS" -eq "$TOTAL" ]; then
    echo -e "  ${GREEN}★ 全部通过! RDMA 环境完全就绪 ★${NC}"
elif [ "$PASS" -ge 7 ]; then
    echo -e "  ${YELLOW}△ 基本可用，但有 $FAIL 项需要修复${NC}"
elif [ "$PASS" -ge 4 ]; then
    echo -e "  ${YELLOW}△ 环境不完整，请修复上述 FAIL 项${NC}"
else
    echo -e "  ${RED}✗ RDMA 环境未就绪，请按照修复建议逐项解决${NC}"
fi

echo ""
echo -e "${BLUE}提示: 修复后重新运行此脚本验证${NC}"
echo -e "${BLUE}      bash env_check.sh${NC}"
echo ""
