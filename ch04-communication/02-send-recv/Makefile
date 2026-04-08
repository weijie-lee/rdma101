# Makefile for Send/Recv example

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

all: send_recv

send_recv: send_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f send_recv

.PHONY: all clean
