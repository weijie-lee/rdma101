#!/bin/bash
# =============================================================================
# NCCL 环境检查脚本
#
# 功能: 检查 NCCL 安装情况、环境变量配置，并提供常用命令示例
#
# NCCL (NVIDIA Collective Communications Library) 是 AI 分布式训练
# 的核心通信库，底层可使用 RDMA (IB Verbs) 进行节点间高速通信。
#
# 用法: bash nccl_env_check.sh
# =============================================================================

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ========== 辅助函数 ==========
print_ok()   { echo -e "  [${GREEN} OK ${NC}] $1"; }
print_warn() { echo -e "  [${YELLOW}WARN${NC}] $1"; }
print_fail() { echo -e "  [${RED}FAIL${NC}] $1"; }
print_info() { echo -e "         ${BLUE}↳ $1${NC}"; }

echo ""
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}    NCCL 环境检查${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""

# --- 检查 1: NCCL 库安装 ---
echo -e "${CYAN}[检查 1] NCCL 库安装${NC}"
NCCL_LIB=""
# 方式 1: ldconfig 查找
NCCL_LIB=$(ldconfig -p 2>/dev/null | grep "libnccl.so" | head -1 | awk '{print $NF}')
if [ -z "$NCCL_LIB" ]; then
    # 方式 2: 常见路径查找
    for dir in /usr/lib /usr/lib64 /usr/local/lib /usr/local/cuda/lib64; do
        if [ -f "$dir/libnccl.so" ] || [ -f "$dir/libnccl.so.2" ]; then
            NCCL_LIB="$dir/libnccl.so"
            break
        fi
    done
fi

if [ -n "$NCCL_LIB" ]; then
    print_ok "NCCL 库已安装: $NCCL_LIB"
else
    print_fail "NCCL 库未找到"
    print_info "安装: apt install libnccl2 libnccl-dev 或从 NVIDIA 官网下载"
fi

# --- 检查 2: NCCL 头文件 ---
echo ""
echo -e "${CYAN}[检查 2] NCCL 头文件${NC}"
NCCL_HEADER=""
for dir in /usr/include /usr/local/include /usr/local/cuda/include; do
    if [ -f "$dir/nccl.h" ]; then
        NCCL_HEADER="$dir/nccl.h"
        break
    fi
done

if [ -n "$NCCL_HEADER" ]; then
    print_ok "nccl.h 已找到: $NCCL_HEADER"
    # 提取 NCCL 版本号
    NCCL_MAJOR=$(grep "NCCL_MAJOR" "$NCCL_HEADER" 2>/dev/null | head -1 | awk '{print $3}')
    NCCL_MINOR=$(grep "NCCL_MINOR" "$NCCL_HEADER" 2>/dev/null | head -1 | awk '{print $3}')
    NCCL_PATCH=$(grep "NCCL_PATCH" "$NCCL_HEADER" 2>/dev/null | head -1 | awk '{print $3}')
    if [ -n "$NCCL_MAJOR" ]; then
        print_info "NCCL 版本: ${NCCL_MAJOR}.${NCCL_MINOR}.${NCCL_PATCH}"
    fi
else
    print_warn "nccl.h 未找到"
    print_info "安装: apt install libnccl-dev"
fi

# --- 检查 3: nccl-tests ---
echo ""
echo -e "${CYAN}[检查 3] nccl-tests (性能测试工具)${NC}"
if command -v all_reduce_perf &>/dev/null; then
    print_ok "all_reduce_perf 已安装: $(which all_reduce_perf)"
elif [ -f "/usr/local/bin/all_reduce_perf" ]; then
    print_ok "all_reduce_perf: /usr/local/bin/all_reduce_perf"
else
    print_warn "nccl-tests 未安装"
    print_info "安装方法:"
    print_info "  git clone https://github.com/NVIDIA/nccl-tests.git"
    print_info "  cd nccl-tests && make MPI=1 MPI_HOME=/usr/local/mpi"
fi

# --- 检查 4: 关键环境变量 ---
echo ""
echo -e "${CYAN}[检查 4] NCCL 关键环境变量${NC}"
echo ""

# 定义环境变量及其说明
declare -A ENV_DESC
ENV_DESC=(
    ["NCCL_IB_DISABLE"]="是否禁用 InfiniBand (0=启用 RDMA, 1=禁用回退 TCP)"
    ["NCCL_IB_HCA"]="指定使用的 HCA 设备名 (如 mlx5_0, mlx5_1)"
    ["NCCL_IB_GID_INDEX"]="RoCE GID 索引 (RoCE v2 通常用 3)"
    ["NCCL_NET_GDR_LEVEL"]="GPUDirect RDMA 级别 (0=禁用, 5=跨节点)"
    ["NCCL_DEBUG"]="调试输出级别 (WARN/INFO/TRACE)"
    ["NCCL_DEBUG_SUBSYS"]="调试子系统过滤 (NET/INIT/COLL/ALL)"
    ["NCCL_SOCKET_IFNAME"]="TCP 控制面使用的网络接口 (如 eth0)"
)

# 按顺序检查各环境变量
for var in NCCL_IB_DISABLE NCCL_IB_HCA NCCL_IB_GID_INDEX \
           NCCL_NET_GDR_LEVEL NCCL_DEBUG NCCL_DEBUG_SUBSYS \
           NCCL_SOCKET_IFNAME; do
    VALUE="${!var}"    # 间接引用获取变量值
    DESC="${ENV_DESC[$var]}"
    if [ -n "$VALUE" ]; then
        echo -e "  ${GREEN}$var${NC} = ${YELLOW}$VALUE${NC}"
    else
        echo -e "  ${BLUE}$var${NC} = (未设置)"
    fi
    echo -e "    ${BLUE}# $DESC${NC}"
done

# --- 常用命令示例 ---
echo ""
echo -e "${CYAN}=============================================${NC}"
echo -e "${CYAN}    常用 NCCL 命令示例${NC}"
echo -e "${CYAN}=============================================${NC}"
echo ""

echo -e "${YELLOW}# 1. 开启 NCCL 调试输出 (查看 RDMA 通信细节)${NC}"
echo "NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=NET python train.py"
echo ""

echo -e "${YELLOW}# 2. PyTorch 分布式初始化 + NCCL 调试${NC}"
echo 'NCCL_DEBUG=INFO python -c "'
echo '  import torch'
echo '  import torch.distributed as dist'
echo '  dist.init_process_group(backend=\"nccl\")'
echo '  print(f\"Rank {dist.get_rank()} initialized\")'
echo '"'
echo ""

echo -e "${YELLOW}# 3. nccl-tests 性能测试 (单机多卡)${NC}"
echo "all_reduce_perf -b 1M -e 1G -f 2 -g 4"
echo "  # -b: 起始消息大小 (1MB)"
echo "  # -e: 结束消息大小 (1GB)"
echo "  # -f: 大小递增倍数"
echo "  # -g: 每节点 GPU 数"
echo ""

echo -e "${YELLOW}# 4. nccl-tests 多机测试 (通过 MPI 启动)${NC}"
echo "mpirun -np 8 --host node1:4,node2:4 \\"
echo "  -x NCCL_IB_HCA=mlx5_0 \\"
echo "  -x NCCL_NET_GDR_LEVEL=5 \\"
echo "  all_reduce_perf -b 1M -e 1G -f 2 -g 1"
echo ""

echo -e "${YELLOW}# 5. 强制使用 RDMA (排除 TCP 干扰)${NC}"
echo "NCCL_IB_DISABLE=0 NCCL_SOCKET_IFNAME=eth0 NCCL_IB_HCA=mlx5_0 python train.py"
echo ""

echo -e "${YELLOW}# 6. 强制使用 TCP (RDMA 环境有问题时的回退方案)${NC}"
echo "NCCL_IB_DISABLE=1 NCCL_SOCKET_IFNAME=eth0 python train.py"
echo ""

echo -e "${BLUE}提示: NCCL 会自动检测并选择最优传输方式。${NC}"
echo -e "${BLUE}      只有在自动检测不正确时才需要手动设置环境变量。${NC}"
echo ""
