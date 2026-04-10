# ch06-connection/03-ud-mode Makefile
#
# Build UD (Unreliable Datagram) mode loopback test
#
# Dependencies: libibverbs-dev, common/librdma_utils.a

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = -libverbs -L../../common -lrdma_utils

# Common utility library path
COMMON_LIB = ../../common/librdma_utils.a

TARGETS = ud_loopback

all: $(COMMON_LIB) $(TARGETS)

# Ensure common library is built
$(COMMON_LIB):
	$(MAKE) -C ../../common

ud_loopback: ud_loopback.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
