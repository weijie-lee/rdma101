# Makefile for QP State example

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

all: qp_state

qp_state: qp_state.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f qp_state

.PHONY: all clean
