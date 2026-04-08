# Makefile for resources

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

all: 01_init_resources

01_init_resources: 01_init_resources.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f 01_init_resources

.PHONY: all clean
