# Makefile for RDMA examples

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs -lrdmacm

all:
	(cd 01-initialization && make)
	(cd 01-rdma-write && make)

clean:
	(cd 01-initialization && make clean)
	(cd 01-rdma-write && make clean)

.PHONY: all clean
