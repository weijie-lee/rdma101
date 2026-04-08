# Makefile for basic concepts

CC = gcc
CFLAGS = -Wall -O2 -g

all: 01_struct_pointer 02_polling_pattern

01_struct_pointer: 01_struct_pointer.c
	$(CC) $(CFLAGS) -o $@ $<

02_polling_pattern: 02_polling_pattern.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f 01_struct_pointer 02_polling_pattern

.PHONY: all clean
