# ch07-engineering/04-error-handling Makefile
#
# 编译两个错误处理演示程序:
#   - trigger_errors:   故意触发 RDMA 错误并展示恢复
#   - error_diagnosis:  RDMA 错误码诊断工具

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = -libverbs -L../../common -lrdma_utils

# 公共工具库路径
COMMON_LIB = ../../common/librdma_utils.a

TARGETS = trigger_errors error_diagnosis

all: $(COMMON_LIB) $(TARGETS)

# 确保公共库已编译
$(COMMON_LIB):
	$(MAKE) -C ../../common

trigger_errors: trigger_errors.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

error_diagnosis: error_diagnosis.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
