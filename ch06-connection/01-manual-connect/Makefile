# 手动 TCP 交换建连示例 Makefile
#
# 编译: make
# 清理: make clean
#
# 依赖: libibverbs-dev, 公共工具库 (../../common/librdma_utils.a)

CC = gcc
CFLAGS = -Wall -O2 -g

# 公共工具库路径
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

TARGET = manual_connect
SRC = manual_connect.c

.PHONY: all clean common

all: common $(TARGET)

# 确保公共库已编译
common:
	$(MAKE) -C $(COMMON_DIR)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)
	@echo "编译完成: $(TARGET)"
	@echo "  服务器: ./$(TARGET) -s [tcp_port]"
	@echo "  客户端: ./$(TARGET) -c <server_ip> [tcp_port]"

clean:
	rm -f $(TARGET)
