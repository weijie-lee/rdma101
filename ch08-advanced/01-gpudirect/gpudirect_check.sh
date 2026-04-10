#!/bin/bash
# =============================================================================
# GPUDirect RDMA 环境检查脚本
#
# 功能: 逐项检查 GPUDirect RDMA 所需的全部环境条件
#       包括 GPU 驱动、内核模块、CUDA 版本、RDMA 设备等
#
# 用法: bash gpudirect_check.sh
#
# 每项检查结果:
#   [OK]   - 条件满足
#   [WARN] - 条件部分满足，可能影响功能
#   [FAIL] - 条件不满足，GPUDirect 无法工作
# =============================================================================

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # 无颜色

# ========== 计数器 ==========
PASS=0      # 通过的检查数
WARN=0      # 警告的检查数
FAIL=0      # 失败的检查数
TOTAL=0     # 总检查数

# ========== 辅助函数 ==========

# 打印标题
print_header() {
    echo ""
    echo -e "${CYAN}=============================================${NC}"
    echo -e "${CYAN}    GPUDirect RDMA 环境检查${NC}"
    echo -e "${CYAN}=============================================${NC}"
    echo ""
}

# 打印检查结果: OK
print_ok() {
    echo -e "  [${GREEN} OK ${NC}] $1"
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
}

# 打印检查结果: WARN
print_warn() {
    echo -e "  [${YELLOW}WARN${NC}] $1"
    WARN=$((WARN + 1))
    TOTAL=$((TOTAL + 1))
}

# 打印检查结果: FAIL
print_fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
}

# 打印说明信息
print_info() {
    echo -e "         ${BLUE}↳ $1${NC}"
}

# ========== 检查项 ==========

print_header

# --- 检查 1: nvidia-smi 是否存在并可用 ---
echo -e "${CYAN}[检查 1] NVIDIA GPU 驱动${NC}"
if command -v nvidia-smi &>/dev/null; then
    # nvidia-smi 存在，尝试运行
    GPU_INFO=$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null)
    if [ $? -eq 0 ]; then
        print_ok "nvidia-smi 可用"
        # 逐行打印 GPU 信息
        echo "$GPU_INFO" | while IFS= read -r line; do
            print_info "GPU: $line"
        done
    else
        print_fail "nvidia-smi 存在但无法查询 GPU"
        print_info "可能原因: GPU 驱动未正确加载，运行 dmesg | grep -i nvidia 查看"
    fi
else
    print_fail "nvidia-smi 未找到"
    print_info "请安装 NVIDIA 驱动: https://www.nvidia.com/drivers"
fi

echo ""

# --- 检查 2: nvidia_peermem / nv_peer_mem 内核模块 ---
echo -e "${CYAN}[检查 2] PeerMem 内核模块 (GPU显存 ↔ RDMA NIC 直通)${NC}"
if lsmod 2>/dev/null | grep -q "nvidia_peermem"; then
    # CUDA 11.4+ 自带的新模块
    print_ok "nvidia_peermem 模块已加载 (CUDA 内置版本)"
    print_info "这是推荐的方式，CUDA 11.4+ 自动包含此模块"
elif lsmod 2>/dev/null | grep -q "nv_peer_mem"; then
    # Mellanox 提供的旧模块
    print_ok "nv_peer_mem 模块已加载 (Mellanox 独立版本)"
    print_info "建议升级到 CUDA 11.4+ 使用内置的 nvidia_peermem"
else
    print_fail "PeerMem 模块未加载"
    print_info "nvidia_peermem: modprobe nvidia_peermem (CUDA 11.4+)"
    print_info "nv_peer_mem:    需要单独安装 https://github.com/Mellanox/nv_peer_memory"
    print_info "没有此模块，ibv_reg_mr() 无法注册 GPU 显存"
fi

echo ""

# --- 检查 3: CUDA 版本 ---
echo -e "${CYAN}[检查 3] CUDA Toolkit${NC}"
if command -v nvcc &>/dev/null; then
    CUDA_VER=$(nvcc --version 2>/dev/null | grep "release" | awk '{print $6}' | tr -d ',')
    print_ok "CUDA Toolkit 已安装: $CUDA_VER"

    # 检查版本是否 >= 11.4 (nvidia_peermem 内置版本)
    MAJOR=$(echo "$CUDA_VER" | cut -d. -f1 | tr -d 'V')
    MINOR=$(echo "$CUDA_VER" | cut -d. -f2)
    if [ "$MAJOR" -gt 11 ] || ([ "$MAJOR" -eq 11 ] && [ "$MINOR" -ge 4 ]); then
        print_info "CUDA >= 11.4，支持内置 nvidia_peermem"
    else
        print_warn "CUDA < 11.4，需要单独安装 nv_peer_mem 模块"
    fi
else
    # nvcc 不在 PATH 中，检查常见安装路径
    if [ -d "/usr/local/cuda" ]; then
        print_warn "CUDA 目录存在 (/usr/local/cuda) 但 nvcc 不在 PATH 中"
        print_info "运行: export PATH=/usr/local/cuda/bin:\$PATH"
    else
        print_fail "CUDA Toolkit 未安装"
        print_info "GPUDirect RDMA 需要 CUDA，推荐 11.4+ 版本"
        print_info "安装: https://developer.nvidia.com/cuda-downloads"
    fi
fi

echo ""

# --- 检查 4: /sys/kernel/mm/memory_peers/ ---
echo -e "${CYAN}[检查 4] Memory Peers 内核接口${NC}"
if [ -d "/sys/kernel/mm/memory_peers/" ]; then
    print_ok "/sys/kernel/mm/memory_peers/ 存在"
    # 列出注册的 peer memory 客户端
    PEERS=$(ls /sys/kernel/mm/memory_peers/ 2>/dev/null)
    if [ -n "$PEERS" ]; then
        print_info "已注册的 peer memory 客户端: $PEERS"
    fi
else
    print_warn "/sys/kernel/mm/memory_peers/ 不存在"
    print_info "该目录在 PeerMem 模块加载后出现"
    print_info "某些内核版本可能使用不同的路径"
fi

echo ""

# --- 检查 5: RDMA 设备 ---
echo -e "${CYAN}[检查 5] RDMA 设备${NC}"
if command -v ibv_devinfo &>/dev/null; then
    # 检查是否有活跃的 RDMA 设备
    ACTIVE_PORTS=$(ibv_devinfo 2>/dev/null | grep -c "state:.*PORT_ACTIVE")
    DEVICE_COUNT=$(ibv_devinfo -l 2>/dev/null | grep -c "^\s")

    if [ "$ACTIVE_PORTS" -gt 0 ]; then
        print_ok "RDMA 设备可用，$ACTIVE_PORTS 个端口处于 ACTIVE 状态"
        # 显示设备名称和端口状态
        ibv_devinfo 2>/dev/null | grep -E "hca_id|port:|state:" | while IFS= read -r line; do
            print_info "$(echo "$line" | xargs)"
        done
    elif [ "$DEVICE_COUNT" -gt 0 ]; then
        print_warn "找到 RDMA 设备但没有 ACTIVE 端口"
        print_info "检查网络连接和端口配置"
    else
        print_fail "没有找到 RDMA 设备"
        print_info "GPUDirect RDMA 需要支持 RDMA 的网卡 (如 Mellanox ConnectX)"
    fi
else
    print_fail "ibv_devinfo 未安装"
    print_info "安装: apt install ibverbs-utils 或 yum install libibverbs-utils"
fi

echo ""

# --- 检查 6: PCIe 拓扑 (GPU 和 NIC 的亲和性) ---
echo -e "${CYAN}[检查 6] PCIe 拓扑 (GPU-NIC 亲和性)${NC}"
if command -v nvidia-smi &>/dev/null && command -v lspci &>/dev/null; then
    # 获取 GPU 的 PCIe 地址
    GPU_PCI=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader 2>/dev/null | head -1)
    if [ -n "$GPU_PCI" ]; then
        print_ok "GPU PCIe 地址: $GPU_PCI"
        # 获取 Mellanox NIC 的 PCIe 地址
        NIC_PCI=$(lspci 2>/dev/null | grep -i "mellanox\|infiniband" | head -1 | awk '{print $1}')
        if [ -n "$NIC_PCI" ]; then
            print_info "NIC PCIe 地址: $NIC_PCI"
            print_info "最佳性能: GPU 和 NIC 应在同一 PCIe switch / NUMA 节点下"
            print_info "查看详细拓扑: nvidia-smi topo -m"
        else
            print_warn "未找到 Mellanox/InfiniBand NIC"
        fi
    fi
else
    print_warn "无法检查 PCIe 拓扑 (需要 nvidia-smi 和 lspci)"
fi

echo ""

# ========== 汇总 ==========
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}    检查结果汇总${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""
echo -e "  ${GREEN}通过: $PASS${NC}  ${YELLOW}警告: $WARN${NC}  ${RED}失败: $FAIL${NC}  总计: $TOTAL"
echo ""

if [ "$FAIL" -eq 0 ] && [ "$WARN" -eq 0 ]; then
    echo -e "  ${GREEN}✓ GPUDirect RDMA 环境完全就绪！${NC}"
elif [ "$FAIL" -eq 0 ]; then
    echo -e "  ${YELLOW}△ GPUDirect RDMA 基本可用，但有警告需要关注${NC}"
else
    echo -e "  ${RED}✗ GPUDirect RDMA 环境未就绪，请解决上述 FAIL 项${NC}"
fi

echo ""
echo -e "${BLUE}提示: GPUDirect RDMA 需要以下全部条件:${NC}"
echo -e "${BLUE}  1. NVIDIA GPU + 驱动${NC}"
echo -e "${BLUE}  2. nvidia_peermem 或 nv_peer_mem 内核模块${NC}"
echo -e "${BLUE}  3. CUDA Toolkit${NC}"
echo -e "${BLUE}  4. 支持 RDMA 的网卡 (如 Mellanox ConnectX-4+)${NC}"
echo -e "${BLUE}  5. GPU 和 NIC 在同一 PCIe 拓扑下 (推荐)${NC}"
echo ""
