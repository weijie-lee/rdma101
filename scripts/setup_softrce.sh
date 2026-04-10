#!/bin/bash
#
# SoftRoCE 一键环境配置脚本
#
# 功能：在没有真实 RDMA 网卡的机器上，通过 SoftRoCE (软件 RoCE) 配置
#       完整的 RDMA 开发和学习环境。
#
# 使用方法：
#   chmod +x scripts/setup_softrce.sh
#   sudo ./scripts/setup_softrce.sh
#
# 前置条件：
#   - Ubuntu 20.04 / 22.04 / 24.04
#   - 至少一个正常工作的网络接口（非 lo）
#   - root 权限或 sudo
#

set -e

# ========== 颜色定义 ==========
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
echo "   RDMA 101 — SoftRoCE 一键环境配置"
echo "============================================="
echo ""

# ========== 步骤 1: 检查权限 ==========
echo ">>> 步骤 1/7: 检查权限"
if [ "$(id -u)" -ne 0 ]; then
    fail "需要 root 权限运行此脚本"
    echo "  请使用: sudo $0"
    exit 1
fi
ok "root 权限确认"

# ========== 步骤 2: 安装依赖包 ==========
echo ""
echo ">>> 步骤 2/7: 安装 RDMA 开发依赖"

# 检查是否已安装
PKGS="libibverbs-dev librdmacm-dev rdma-core ibverbs-utils perftest build-essential"
MISSING=""
for pkg in $PKGS; do
    if ! dpkg -l "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done

if [ -n "$MISSING" ]; then
    info "安装缺少的包:$MISSING"
    apt-get update -qq
    apt-get install -y -qq $MISSING >/dev/null 2>&1
    ok "依赖包安装完成"
else
    ok "所有依赖包已安装"
fi

# ========== 步骤 3: 加载内核模块 ==========
echo ""
echo ">>> 步骤 3/7: 加载 SoftRoCE 内核模块"

if lsmod | grep -q rdma_rxe; then
    ok "rdma_rxe 模块已加载"
else
    modprobe rdma_rxe
    if lsmod | grep -q rdma_rxe; then
        ok "rdma_rxe 模块加载成功"
    else
        fail "rdma_rxe 模块加载失败"
        echo "  可能原因: 内核版本不支持，请检查 dmesg"
        exit 1
    fi
fi

# 确保 ib_uverbs 也已加载
if ! lsmod | grep -q ib_uverbs; then
    modprobe ib_uverbs 2>/dev/null || true
fi

# ========== 步骤 4: 创建 SoftRoCE 设备 ==========
echo ""
echo ">>> 步骤 4/7: 创建 SoftRoCE 设备"

# 检查是否已有 SoftRoCE 设备
EXISTING_RXE=$(rdma link show 2>/dev/null | grep "rxe" | head -1 || true)
if [ -n "$EXISTING_RXE" ]; then
    ok "SoftRoCE 设备已存在: $EXISTING_RXE"
else
    # 自动选择一个非 lo 的网络接口
    NETDEV=$(ip -o link show up | grep -v "lo:" | head -1 | awk -F': ' '{print $2}' | tr -d ' ')
    if [ -z "$NETDEV" ]; then
        fail "没有找到可用的网络接口"
        echo "  请确保至少有一个 UP 状态的非 lo 接口"
        exit 1
    fi

    info "使用网络接口: $NETDEV"
    rdma link add rxe0 type rxe netdev "$NETDEV"
    ok "SoftRoCE 设备 rxe0 已创建 (绑定到 $NETDEV)"
fi

# ========== 步骤 5: 配置系统参数 ==========
echo ""
echo ">>> 步骤 5/7: 配置系统参数"

# 设置 memlock 限制
# 检查当前限制
CURRENT_MEMLOCK=$(ulimit -l 2>/dev/null || echo "0")
if [ "$CURRENT_MEMLOCK" = "unlimited" ] || [ "$CURRENT_MEMLOCK" -ge 65536 ] 2>/dev/null; then
    ok "内存锁定限制已足够: $CURRENT_MEMLOCK"
else
    # 写入 limits.conf
    if ! grep -q "memlock" /etc/security/limits.conf 2>/dev/null; then
        echo "* soft memlock unlimited" >> /etc/security/limits.conf
        echo "* hard memlock unlimited" >> /etc/security/limits.conf
        ok "已设置 /etc/security/limits.conf (需重新登录生效)"
    else
        ok "limits.conf 中已有 memlock 配置"
    fi
    warn "当前 shell 的 memlock 可能需要重新登录才能生效"
    warn "临时解决: 在运行程序前执行 ulimit -l unlimited"
fi

# ========== 步骤 6: 验证环境 ==========
echo ""
echo ">>> 步骤 6/7: 验证 RDMA 环境"

# 验证设备
DEVICE_COUNT=$(ibv_devices 2>/dev/null | grep -c "rxe\|mlx\|hfi\|qedr" || echo "0")
if [ "$DEVICE_COUNT" -gt 0 ]; then
    ok "检测到 $DEVICE_COUNT 个 RDMA 设备"
else
    fail "没有检测到 RDMA 设备"
    exit 1
fi

# 打印设备信息
echo ""
info "设备详情:"
ibv_devinfo 2>/dev/null | head -20
echo ""

# 检查端口状态
PORT_STATE=$(ibv_devinfo 2>/dev/null | grep "state:" | head -1 | awk '{print $2}')
if [ "$PORT_STATE" = "PORT_ACTIVE" ]; then
    ok "端口状态: ACTIVE"
else
    warn "端口状态: $PORT_STATE (非 ACTIVE)"
fi

# 检查 link_layer
LINK_LAYER=$(ibv_devinfo 2>/dev/null | grep "link_layer:" | head -1 | awk '{print $2}')
if [ "$LINK_LAYER" = "Ethernet" ]; then
    ok "链路层: Ethernet (RoCE 模式)"
    info "注意: SoftRoCE 下 LID=0 是正常的，使用 GID 进行寻址"
else
    info "链路层: $LINK_LAYER"
fi

# ========== 步骤 7: 编译和冒烟测试 ==========
echo ""
echo ">>> 步骤 7/7: 编译项目并运行冒烟测试"

cd "$PROJECT_DIR"

# 编译
info "编译项目 (make all)..."
if make all >/dev/null 2>&1; then
    ok "项目编译成功"
else
    warn "部分程序编译失败（可能缺少 librdmacm-dev），但核心程序可用"
fi

# 冒烟测试: 运行 hello_rdma
if [ -f "ch09-quickref/hello_rdma" ]; then
    info "运行冒烟测试: hello_rdma"
    ulimit -l unlimited 2>/dev/null || true
    if timeout 10 ./ch09-quickref/hello_rdma >/dev/null 2>&1; then
        ok "冒烟测试通过: hello_rdma 正常运行"
    else
        warn "hello_rdma 运行异常 (可能需要 ulimit -l unlimited)"
    fi
fi

# ========== 完成 ==========
echo ""
echo "============================================="
echo -e "  ${GREEN}SoftRoCE 环境配置完成!${NC}"
echo "============================================="
echo ""
echo "  SoftRoCE 核心概念:"
echo "    - LID = 0 是正常的 (RoCE 不使用 LID)"
echo "    - 使用 GID 进行寻址 (基于网卡 IP 地址生成)"
echo "    - 性能远低于真实硬件 (仅用于学习和开发)"
echo "    - 所有 Verbs API 行为与真实硬件一致"
echo ""
echo "  快速开始:"
echo "    1. 运行单进程示例:  ./ch09-quickref/hello_rdma"
echo "    2. 运行双进程示例:  (终端1) ./ch06-connection/01-manual-connect/manual_connect -s"
echo "                        (终端2) ./ch06-connection/01-manual-connect/manual_connect -c 127.0.0.1"
echo "    3. 运行全部测试:    sudo ./scripts/run_all_tests.sh"
echo ""
echo "  如果程序报 'Cannot allocate memory'，请执行:"
echo "    ulimit -l unlimited"
echo ""
