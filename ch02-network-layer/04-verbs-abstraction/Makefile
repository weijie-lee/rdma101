# Makefile for ch02-network-layer/04-verbs-abstraction
#
# Compile Verbs unified abstraction layer demo program
# Dependency: common/librdma_utils.a (please run make -C ../../common first)

CC = gcc
CFLAGS = -Wall -O2 -g -std=c11

# Common utility library path
COMMON_DIR = ../../common
INCLUDES = -I$(COMMON_DIR)
LIBS = $(COMMON_DIR)/librdma_utils.a -libverbs

# Target files
TARGETS = verbs_any_transport

all: check_common $(TARGETS)

# Check if common library is compiled
check_common:
	@if [ ! -f $(COMMON_DIR)/librdma_utils.a ]; then \
		echo "Compiling common utility library..."; \
		$(MAKE) -C $(COMMON_DIR); \
	fi

verbs_any_transport: verbs_any_transport.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean check_common
