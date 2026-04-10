# Makefile for CQ examples

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = ../../common/librdma_utils.a -libverbs -lpthread

all: cq_event_driven cq_overflow

cq_event_driven: cq_event_driven.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

cq_overflow: cq_overflow.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f cq_event_driven cq_overflow

.PHONY: all clean
