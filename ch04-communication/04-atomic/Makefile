# Makefile for Atomic Operations example

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

all: atomic_ops

atomic_ops: atomic_ops.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f atomic_ops

.PHONY: all clean
