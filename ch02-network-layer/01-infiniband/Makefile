# Makefile for ch02-network-layer/01-infiniband
#
# 编译 InfiniBand 端口查询示例程序
# 依赖: common/librdma_utils.a (请先 make -C ../../common)

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11

# 公共工具库路径
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

# 目标文件
TARGETS = ib_port_detail

all: check_common $(TARGETS)

# 检查公共库是否已编译
check_common:
	@if [ ! -f $(COMMON_DIR)/librdma_utils.a ]; then \
		echo "正在编译公共工具库..."; \
		$(MAKE) -C $(COMMON_DIR); \
	fi

ib_port_detail: ib_port_detail.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean check_common
