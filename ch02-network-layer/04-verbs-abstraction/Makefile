# Makefile for ch02-network-layer/04-verbs-abstraction
#
# 编译 Verbs 统一抽象层演示程序
# 依赖: common/librdma_utils.a (请先 make -C ../../common)

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11

# 公共工具库路径
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

# 目标文件
TARGETS = verbs_any_transport

all: check_common $(TARGETS)

# 检查公共库是否已编译
check_common:
	@if [ ! -f $(COMMON_DIR)/librdma_utils.a ]; then \
		echo "正在编译公共工具库..."; \
		$(MAKE) -C $(COMMON_DIR); \
	fi

verbs_any_transport: verbs_any_transport.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean check_common
