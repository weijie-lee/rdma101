# RDMA CM 连接管理示例 Makefile
#
# 编译: make
# 清理: make clean
#
# 依赖: librdmacm-dev, libibverbs-dev

CC = gcc
CFLAGS = -Wall -O2 -g
LIBS = -lrdmacm -libverbs

TARGET = rdma_cm_example
SRC = rdma_cm_example.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
	@echo "编译完成: $(TARGET)"
	@echo "  服务器: ./$(TARGET) -s <port>"
	@echo "  客户端: ./$(TARGET) -c <server_ip> <port>"

clean:
	rm -f $(TARGET)
