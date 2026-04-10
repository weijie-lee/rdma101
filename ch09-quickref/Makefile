# ch09-quickref Makefile
#
# 构建目标:
#   hello_rdma       - 最小完整 RDMA Send/Recv Loopback 示例
#   error_cheatsheet - WC 错误码速查工具
#
# 用法:
#   make              # 编译所有目标
#   make hello_rdma   # 只编译 hello_rdma
#   make clean        # 清理编译产物

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# 公共工具库路径
COMMON_DIR = ../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

# 链接 libibverbs
LDFLAGS = -libverbs

# ========== 构建目标 ==========

all: hello_rdma error_cheatsheet

# hello_rdma 依赖公共工具库
hello_rdma: hello_rdma.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

# error_cheatsheet 不依赖公共库，但需要 libibverbs 的头文件
error_cheatsheet: error_cheatsheet.c
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(LDFLAGS)

# 如果公共工具库不存在，先编译它
$(COMMON_LIB):
	$(MAKE) -C $(COMMON_DIR)

# ========== 清理 ==========

clean:
	rm -f hello_rdma error_cheatsheet

.PHONY: all clean
