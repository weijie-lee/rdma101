#!/bin/bash
#
# strace RDMA 程序分析脚本
#
# 功能：
#   - 使用 strace 追踪 RDMA 程序的系统调用
#   - 自动标注 RDMA 相关的系统调用含义
#   - 区分哪些操作走内核态 vs 用户态
#
# 用法:
#   ./strace_verbs.sh <程序路径> [程序参数...]
#
# 示例:
#   ./strace_verbs.sh ./inline_data
#   ./strace_verbs.sh /usr/bin/ibv_devinfo
#
# 说明:
#   RDMA (libibverbs) 的设计是"内核旁路 (kernel bypass)"。
#   但资源创建/销毁仍需要走内核 (通过 ioctl)，
#   而数据传输 (post_send/post_recv/poll_cq) 是纯用户态操作。
#
#   本脚本追踪的系统调用:
#   - open/openat: 打开设备文件
#   - mmap/munmap: 映射 NIC 寄存器到用户空间
#   - ioctl:       内核态操作 (创建 QP/CQ/MR 等)
#   - close:       关闭文件描述符
#   - write:       可能的事件通知
#

# ========== 颜色定义 ==========
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ========== 参数检查 ==========
if [ $# -lt 1 ]; then
    echo -e "${BOLD}用法: $0 <程序路径> [程序参数...]${NC}"
    echo ""
    echo "示例:"
    echo "  $0 ./inline_data"
    echo "  $0 /usr/bin/ibv_devinfo"
    exit 1
fi

PROG="$1"
shift
PROG_ARGS="$@"

# 检查程序是否存在
if [ ! -f "$PROG" ] && ! command -v "$PROG" &>/dev/null; then
    echo -e "${RED}[错误]${NC} 程序 '$PROG' 不存在"
    exit 1
fi

# 检查 strace 是否安装
if ! command -v strace &>/dev/null; then
    echo -e "${RED}[错误]${NC} strace 未安装"
    echo "  安装: sudo apt-get install strace"
    exit 1
fi

# ========== 运行 strace ==========
echo -e "${BOLD}${CYAN}╔═══════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║   strace RDMA 程序系统调用分析        ║${NC}"
echo -e "${BOLD}${CYAN}╚═══════════════════════════════════════╝${NC}"
echo ""
echo -e "${BLUE}[信息]${NC} 目标程序: $PROG $PROG_ARGS"
echo -e "${BLUE}[信息]${NC} 追踪调用: open, openat, close, mmap, munmap, ioctl, write"
echo ""

# 运行 strace 并保存输出
STRACE_OUT=$(mktemp /tmp/strace_rdma_XXXXXX.log)
trap "rm -f $STRACE_OUT" EXIT

strace -e trace=open,openat,close,mmap,munmap,ioctl,write \
       -f -tt "$PROG" $PROG_ARGS 2>"$STRACE_OUT"

# ========== 分析和注释输出 ==========
echo ""
echo -e "${BOLD}${CYAN}━━━ RDMA 相关系统调用分析 ━━━${NC}"
echo ""

# 统计计数
UVERBS_OPEN=0
MMAP_COUNT=0
IOCTL_COUNT=0

while IFS= read -r line; do
    # 过滤和注释 RDMA 相关调用

    # /dev/infiniband/uverbs* → 打开 Verbs 设备文件
    if echo "$line" | grep -q '/dev/infiniband/uverbs'; then
        echo -e "${GREEN}[Verbs设备]${NC} $line"
        echo -e "    ${YELLOW}→ 打开 Verbs 设备文件 (用户态 Verbs 的入口点)${NC}"
        UVERBS_OPEN=$((UVERBS_OPEN + 1))

    # /dev/infiniband/rdma_cm → 打开 RDMA CM 设备
    elif echo "$line" | grep -q '/dev/infiniband/rdma_cm'; then
        echo -e "${GREEN}[RDMA CM]${NC} $line"
        echo -e "    ${YELLOW}→ 打开 RDMA CM 设备 (连接管理)${NC}"

    # mmap 调用 → 映射网卡寄存器/内存
    elif echo "$line" | grep -q "^.*mmap("; then
        echo -e "${BLUE}[内存映射]${NC} $line"
        echo -e "    ${YELLOW}→ 映射网卡寄存器/DoorBell/缓冲区到用户空间${NC}"
        echo -e "    ${YELLOW}  (这是 kernel bypass 的关键: 数据通路不再经过内核)${NC}"
        MMAP_COUNT=$((MMAP_COUNT + 1))

    # ioctl 调用 → 内核态操作
    elif echo "$line" | grep -q "^.*ioctl("; then
        echo -e "${RED}[内核操作]${NC} $line"
        echo -e "    ${YELLOW}→ 内核态操作 (资源创建: PD/MR/CQ/QP, 或状态修改)${NC}"
        IOCTL_COUNT=$((IOCTL_COUNT + 1))

    # /sys/class/infiniband → 查询设备 sysfs 信息
    elif echo "$line" | grep -q '/sys/class/infiniband'; then
        echo -e "${CYAN}[设备查询]${NC} $line"
        echo -e "    ${YELLOW}→ 通过 sysfs 查询设备/端口属性${NC}"
    fi

done < "$STRACE_OUT"

# ========== 统计汇总 ==========
echo ""
echo -e "${BOLD}${CYAN}━━━ 系统调用统计 ━━━${NC}"
echo ""
echo -e "  Verbs 设备打开次数:  ${GREEN}$UVERBS_OPEN${NC}"
echo -e "  mmap 映射次数:       ${BLUE}$MMAP_COUNT${NC}"
echo -e "  ioctl (内核操作):    ${RED}$IOCTL_COUNT${NC}"
echo ""

echo -e "${BOLD}━━━ RDMA 数据通路说明 ━━━${NC}"
echo ""
echo "  ┌─────────────────────────────────────────────────────────┐"
echo "  │ 内核态操作 (走 ioctl):                                  │"
echo "  │   - ibv_open_device()    打开设备                       │"
echo "  │   - ibv_alloc_pd()       创建保护域                     │"
echo "  │   - ibv_reg_mr()         注册内存区域                   │"
echo "  │   - ibv_create_cq()      创建完成队列                   │"
echo "  │   - ibv_create_qp()      创建队列对                     │"
echo "  │   - ibv_modify_qp()      修改 QP 状态                   │"
echo "  │   - ibv_destroy_*()      销毁资源                       │"
echo "  ├─────────────────────────────────────────────────────────┤"
echo "  │ 用户态操作 (纯 CPU, 不走内核):                          │"
echo "  │   - ibv_post_send()      发布发送请求 (写 DoorBell)     │"
echo "  │   - ibv_post_recv()      发布接收请求 (写 DoorBell)     │"
echo "  │   - ibv_poll_cq()        轮询完成队列 (读共享内存)      │"
echo "  │   这些操作不会出现在 strace 中!                         │"
echo "  └─────────────────────────────────────────────────────────┘"
echo ""
echo -e "${GREEN}提示: 真正的数据传输 (post_send/recv/poll_cq) 是纯用户态的，${NC}"
echo -e "${GREEN}      strace 无法追踪。这就是 RDMA 'kernel bypass' 的核心。${NC}"
echo ""
