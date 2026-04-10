# ch06-connection/03-ud-mode Makefile
#
# 编译 UD (Unreliable Datagram) 模式环回测试
#
# 依赖: libibverbs-dev, common/librdma_utils.a

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = -libverbs -L../../common -lrdma_utils

# 公共工具库路径
COMMON_LIB = ../../common/librdma_utils.a

TARGETS = ud_loopback

all: $(COMMON_LIB) $(TARGETS)

# 确保公共库已编译
$(COMMON_LIB):
	$(MAKE) -C ../../common

ud_loopback: ud_loopback.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
