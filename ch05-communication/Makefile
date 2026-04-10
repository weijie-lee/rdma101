# Top-level Makefile for ch05-communication
# 先编译公共库，再编译各子目录的示例

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11
LDFLAGS = -libverbs -lrdmacm

COMMON_DIR = ../common
SUBDIRS = 01-rdma-write 02-send-recv 03-rdma-read 04-atomic

all: common $(SUBDIRS)

common:
	$(MAKE) -C $(COMMON_DIR)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	$(MAKE) -C $(COMMON_DIR) clean
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done

.PHONY: all clean common $(SUBDIRS)
