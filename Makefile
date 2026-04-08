# Makefile for RDMA examples

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS_LIB = -libverbs
LDFLAGS =

all:
	# 前置练习（不需要RDMA库）
	(cd src/01_basic_concepts && $(MAKE) all)
	# 资源初始化
	(cd src/02_resources && $(MAKE) all)
	# Send/Recv
	(cd src/03_send_recv && $(MAKE) all)

clean:
	(cd src/01_basic_concepts && $(MAKE) clean)
	(cd src/02_resources && $(MAKE) clean)
	(cd src/03_send_recv && $(MAKE) clean)

.PHONY: all clean
