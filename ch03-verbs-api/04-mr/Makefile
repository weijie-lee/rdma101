# Makefile for MR examples

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = ../../common/librdma_utils.a -libverbs

all: mr_access_flags mr_multi_reg

mr_access_flags: mr_access_flags.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

mr_multi_reg: mr_multi_reg.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f mr_access_flags mr_multi_reg

.PHONY: all clean
