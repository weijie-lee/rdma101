# Makefile for ch05-communication/01-rdma-write

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

COMMON_DIR = ../../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

TARGETS = rdma_write 01_write_imm

all: $(TARGETS)

rdma_write: rdma_write.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

01_write_imm: 01_write_imm.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
