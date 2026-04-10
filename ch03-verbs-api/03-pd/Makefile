# Makefile for PD isolation example

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = ../../common/librdma_utils.a -libverbs

all: pd_isolation

pd_isolation: pd_isolation.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f pd_isolation

.PHONY: all clean
