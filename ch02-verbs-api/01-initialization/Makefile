# Makefile for RDMA examples

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs -lrdmacm

TARGETS = rdma_init rdma_write

all: $(TARGETS)

rdma_init: rdma_init.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rdma_write: rdma_write.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
