# Makefile for RDMA Read example

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

all: rdma_read

rdma_read: rdma_read.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rdma_read

.PHONY: all clean
