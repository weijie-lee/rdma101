# ch07-engineering/02-tuning Makefile
#
# 编译三个性能调优演示程序:
#   - inline_data:     Inline Data 优化对比
#   - unsignaled_send: Unsignaled Completion 优化对比
#   - srq_demo:        Shared Receive Queue 演示

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = -libverbs -L../../common -lrdma_utils

# 公共工具库路径
COMMON_LIB = ../../common/librdma_utils.a

TARGETS = inline_data unsignaled_send srq_demo

all: $(COMMON_LIB) $(TARGETS)

# 确保公共库已编译
$(COMMON_LIB):
	$(MAKE) -C ../../common

inline_data: inline_data.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

unsignaled_send: unsignaled_send.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

srq_demo: srq_demo.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
