# Makefile for ch02-network-layer/02-roce
#
# Compile RoCE GID query example program
# Dependency: common/librdma_utils.a (please run make -C ../../common first)

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11

# Common utility library path
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

# Target files
TARGETS = roce_gid_query

all: check_common $(TARGETS)

# Check if common library is compiled
check_common:
	@if [ ! -f $(COMMON_DIR)/librdma_utils.a ]; then \
		echo "Compiling common utility library..."; \
		$(MAKE) -C $(COMMON_DIR); \
	fi

roce_gid_query: roce_gid_query.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean check_common
