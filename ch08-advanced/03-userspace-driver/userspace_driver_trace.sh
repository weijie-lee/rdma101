#!/bin/bash
# =============================================================================
# RDMA 用户态驱动追踪脚本
#
# 功能: 使用 strace 追踪 RDMA 程序的系统调用，
#       揭示用户态驱动 (libibverbs) 的工作方式
#
# 用法:
#   bash userspace_driver_trace.sh              # 默认追踪 ibv_devices
#   bash userspace_driver_trace.sh ibv_devinfo  # 追踪指定程序
#   bash userspace_driver_trace.sh ./my_rdma_program  # 追踪自定义程序
#
# 核心发现:
#   - ibv_open_device → open("/dev/infiniband/uverbs0")
#   - ibv_create_qp  → ioctl + mmap (映射 QP 到用户态)
#   - ibv_post_send  → 无系统调用! (纯用户态写 mmap 内存)
#   - ibv_poll_cq    → 无系统调用! (纯用户态读 mmap 内存)
# =============================================================================

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== 参数处理 ==========
# 默认追踪 ibv_devices
TARGET="${1:-ibv_devices}"

# 检查 strace 是否安装
if ! command -v strace &>/dev/null; then
    echo -e "${RED}[错误] strace 未安装${NC}"
    echo "  安装: apt install strace 或 yum install strace"
    exit 1
fi

# 检查目标程序是否存在
if ! command -v "$TARGET" &>/dev/null && [ ! -f "$TARGET" ]; then
    echo -e "${RED}[错误] 程序不存在: $TARGET${NC}"
    exit 1
fi

echo ""
echo -e "${CYAN}======================================================${NC}"
echo -e "${CYAN}  RDMA 用户态驱动追踪${NC}"
echo -e "${CYAN}  追踪目标: ${YELLOW}$TARGET${NC}"
echo -e "${CYAN}======================================================${NC}"
echo ""

# ========== 追踪 1: uverbs 设备文件打开 ==========
echo -e "${BOLD}[追踪 1] 设备文件打开 (open/openat /dev/infiniband/*)${NC}"
echo -e "${BLUE}# RDMA 程序第一步: 打开 uverbs 字符设备${NC}"
echo -e "${BLUE}# ibv_open_device() → 内核 open(\"/dev/infiniband/uverbs0\")${NC}"
echo -e "${BLUE}# uverbs0 是用户态 Verbs 的内核接口${NC}"
echo ""

strace -f -e trace=open,openat "$TARGET" 2>&1 | \
    grep -E "infiniband|uverbs|rdma" | head -20 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""
echo -e "  ${YELLOW}说明: /dev/infiniband/uverbs* 是 RDMA 用户态入口${NC}"
echo -e "  ${YELLOW}      每个 HCA 设备对应一个 uverbs 字符设备${NC}"
echo ""

# ========== 追踪 2: mmap 调用 ==========
echo -e "${BOLD}[追踪 2] mmap 调用 (映射 HCA 资源到用户态)${NC}"
echo -e "${BLUE}# ibv_create_qp/cq 后，内核通过 mmap 将以下内容映射到用户空间:${NC}"
echo -e "${BLUE}#   - Send Queue / Recv Queue 内存 → 用户态可直接写 WQE${NC}"
echo -e "${BLUE}#   - Completion Queue 内存 → 用户态可直接读 CQE${NC}"
echo -e "${BLUE}#   - Doorbell 寄存器 (MMIO) → 用户态直接通知 HCA${NC}"
echo ""

strace -f -e trace=mmap "$TARGET" 2>&1 | \
    grep -v "PROT_NONE\|No such\|ENOENT" | head -20 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""
echo -e "  ${YELLOW}说明: mmap 是用户态驱动的关键 — 建立共享内存映射后，${NC}"
echo -e "  ${YELLOW}      ibv_post_send/ibv_poll_cq 就不再需要系统调用${NC}"
echo ""

# ========== 追踪 3: ioctl 调用 ==========
echo -e "${BOLD}[追踪 3] ioctl 调用 (内核 Verbs 操作)${NC}"
echo -e "${BLUE}# 资源创建/修改通过 ioctl 完成 (控制路径，非数据路径):${NC}"
echo -e "${BLUE}#   ibv_alloc_pd   → ioctl(fd, IB_USER_VERBS_CMD_ALLOC_PD, ...)${NC}"
echo -e "${BLUE}#   ibv_reg_mr     → ioctl(fd, IB_USER_VERBS_CMD_REG_MR, ...)${NC}"
echo -e "${BLUE}#   ibv_create_cq  → ioctl(fd, IB_USER_VERBS_CMD_CREATE_CQ, ...)${NC}"
echo -e "${BLUE}#   ibv_create_qp  → ioctl(fd, IB_USER_VERBS_CMD_CREATE_QP, ...)${NC}"
echo -e "${BLUE}#   ibv_modify_qp  → ioctl(fd, IB_USER_VERBS_CMD_MODIFY_QP, ...)${NC}"
echo ""

strace -f -e trace=ioctl "$TARGET" 2>&1 | \
    grep -v "TCGETS\|TIOCGWINSZ" | head -20 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""
echo -e "  ${YELLOW}说明: ioctl 只在资源创建/销毁时调用 (控制路径)${NC}"
echo -e "  ${YELLOW}      数据路径 (post_send/poll_cq) 不使用 ioctl${NC}"
echo ""

# ========== 追踪 4: write 调用 (Doorbell) ==========
echo -e "${BOLD}[追踪 4] write 调用 (部分驱动使用 write 代替 MMIO doorbell)${NC}"
echo -e "${BLUE}# 某些旧版驱动通过 write() 系统调用触发 doorbell${NC}"
echo -e "${BLUE}# 现代 mlx5 驱动使用 mmap 的 MMIO doorbell (BlueFlame)，无 write()${NC}"
echo ""

strace -f -e trace=write "$TARGET" 2>&1 | \
    grep -v "EBADF\|pipe\|stdout\|stderr" | head -10 | \
    while IFS= read -r line; do
        echo -e "  ${GREEN}$line${NC}"
    done

echo ""

# ========== 综合追踪: 所有系统调用统计 ==========
echo -e "${BOLD}[综合] 系统调用统计${NC}"
echo -e "${BLUE}# 统计程序运行期间各系统调用的次数${NC}"
echo -e "${BLUE}# 注意: ibv_post_send 和 ibv_poll_cq 不会出现在这里！${NC}"
echo ""

strace -f -c "$TARGET" 2>&1 | tail -30 | \
    while IFS= read -r line; do
        echo -e "  $line"
    done

echo ""

# ========== 原理总结 ==========
echo -e "${CYAN}======================================================${NC}"
echo -e "${CYAN}  RDMA 用户态驱动原理总结${NC}"
echo -e "${CYAN}======================================================${NC}"
echo ""
echo -e "  ${BOLD}调用链:${NC}"
echo ""
echo -e "  应用程序"
echo -e "    │"
echo -e "    ▼"
echo -e "  libibverbs.so (通用 Verbs 接口)"
echo -e "    │"
echo -e "    ▼"
echo -e "  libmlx5.so / libhfi1.so (厂商驱动库, provider)"
echo -e "    │"
echo -e "    ├── 控制路径: open() + ioctl() + mmap() → 内核参与"
echo -e "    │"
echo -e "    └── 数据路径: 直接写 mmap 内存 → ${GREEN}纯用户态，零系统调用${NC}"
echo -e "          │"
echo -e "          ▼"
echo -e "        HCA 硬件 (通过 DMA 读取数据并发送)"
echo ""
echo -e "  ${YELLOW}关键洞察:${NC}"
echo -e "  ${YELLOW}  1. ibv_post_send() = 写 WQE 到 mmap 的 SQ + 写 doorbell 寄存器${NC}"
echo -e "  ${YELLOW}  2. ibv_poll_cq()   = 读 mmap 的 CQ 内存${NC}"
echo -e "  ${YELLOW}  3. 数据路径完全在用户态，延迟 ~1μs (vs TCP 的 ~10μs)${NC}"
echo -e "  ${YELLOW}  4. 这就是 RDMA 被称为 \"kernel bypass\" 的原因${NC}"
echo ""
