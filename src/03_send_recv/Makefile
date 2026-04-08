# Makefile for send_recv

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

all: 01_loopback_send_recv

01_loopback_send_recv: 01_loopback_send_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f 01_loopback_send_recv

.PHONY: all clean
