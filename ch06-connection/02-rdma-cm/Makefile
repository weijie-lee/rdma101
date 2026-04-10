# RDMA CM Connection Management Example Makefile
#
# Build: make
# Clean: make clean
#
# Dependencies: librdmacm-dev, libibverbs-dev

CC = gcc
CFLAGS = -Wall -O2 -g
LIBS = -lrdmacm -libverbs

TARGET = rdma_cm_example
SRC = rdma_cm_example.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
	@echo "Build complete: $(TARGET)"
	@echo "  Server: ./$(TARGET) -s <port>"
	@echo "  Client: ./$(TARGET) -c <server_ip> <port>"

clean:
	rm -f $(TARGET)
