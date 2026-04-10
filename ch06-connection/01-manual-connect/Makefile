# Manual TCP Exchange Connection Example Makefile
#
# Build: make
# Clean: make clean
#
# Dependencies: libibverbs-dev, common utility library (../../common/librdma_utils.a)

CC = gcc
CFLAGS = -Wall -O2 -g

# Common utility library path
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

TARGET = manual_connect
SRC = manual_connect.c

.PHONY: all clean common

all: common $(TARGET)

# Ensure common library is built
common:
	$(MAKE) -C $(COMMON_DIR)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)
	@echo "Build complete: $(TARGET)"
	@echo "  Server: ./$(TARGET) -s [tcp_port]"
	@echo "  Client: ./$(TARGET) -c <server_ip> [tcp_port]"

clean:
	rm -f $(TARGET)
