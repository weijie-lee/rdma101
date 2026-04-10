# Makefile for ch02-network-layer/03-iwarp
#
# 编译 iWARP 设备查询示例程序
# 依赖: common/librdma_utils.a (请先 make -C ../../common)

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11

# 公共工具库路径
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

# 目标文件
TARGETS = iwarp_query

all: check_common $(TARGETS)

# 检查公共库是否已编译
check_common:
	@if [ ! -f $(COMMON_DIR)/librdma_utils.a ]; then \
		echo "正在编译公共工具库..."; \
		$(MAKE) -C $(COMMON_DIR); \
	fi

iwarp_query: iwarp_query.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean check_common
