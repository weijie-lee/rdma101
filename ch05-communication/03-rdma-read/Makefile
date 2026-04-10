# Makefile for ch05-communication/03-rdma-read

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

COMMON_DIR = ../../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

TARGETS = rdma_read 01_batch_read

all: $(TARGETS)

rdma_read: rdma_read.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

01_batch_read: 01_batch_read.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
