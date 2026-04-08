# Makefile for RDMA examples

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11
LDFLAGS = -libverbs -lrdmacm

all:
	(cd 01-initialization && $(MAKE))
	(cd 02-qp-state && $(MAKE))
	(cd 02-send-recv && $(MAKE))
	(cd 03-rdma-read && $(MAKE))
	(cd 04-atomic && $(MAKE))

clean:
	(cd 01-initialization && $(MAKE) clean)
	(cd 02-qp-state && $(MAKE) clean)
	(cd 02-send-recv && $(MAKE) clean)
	(cd 03-rdma-read && $(MAKE) clean)
	(cd 04-atomic && $(MAKE) clean)

.PHONY: all clean
