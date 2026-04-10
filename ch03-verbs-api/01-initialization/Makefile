# Makefile for ch03-verbs-api/01-initialization

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

COMMON_DIR = ../../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

TARGETS = 01_init_resources 02_multi_device

all: $(TARGETS)

01_init_resources: 01_init_resources.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

02_multi_device: 02_multi_device.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
