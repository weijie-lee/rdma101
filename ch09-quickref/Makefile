# ch09-quickref Makefile
#
# Build targets:
#   hello_rdma       - Minimal complete RDMA Send/Recv Loopback example
#   error_cheatsheet - WC error code lookup tool
#
# Usage:
#   make              # Build all targets
#   make hello_rdma   # Build hello_rdma only
#   make clean        # Clean build artifacts

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# Common utility library path
COMMON_DIR = ../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

# Link libibverbs
LDFLAGS = -libverbs

# ========== Build Targets ==========

all: hello_rdma error_cheatsheet

# hello_rdma depends on the common utility library
hello_rdma: hello_rdma.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

# error_cheatsheet doesn't depend on the common library, but needs libibverbs headers
error_cheatsheet: error_cheatsheet.c
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(LDFLAGS)

# If the common utility library doesn't exist, build it first
$(COMMON_LIB):
	$(MAKE) -C $(COMMON_DIR)

# ========== Clean ==========

clean:
	rm -f hello_rdma error_cheatsheet

.PHONY: all clean
