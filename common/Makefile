# RDMA Common Utility Library

CC = gcc
CFLAGS = -Wall -O2 -g
AR = ar

LIB = librdma_utils.a
OBJS = rdma_utils.o

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

rdma_utils.o: rdma_utils.c rdma_utils.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB)

.PHONY: all clean
