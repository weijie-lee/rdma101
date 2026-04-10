# Makefile for ch05-communication/02-send-recv

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

COMMON_DIR = ../../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

TARGETS = 01_loopback_send_recv send_recv 02_sge_demo 03_rnr_error_demo

all: $(TARGETS)

01_loopback_send_recv: 01_loopback_send_recv.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

send_recv: send_recv.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

02_sge_demo: 02_sge_demo.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

03_rnr_error_demo: 03_rnr_error_demo.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
