# ch07-engineering/02-tuning Makefile
#
# Build three performance tuning demo programs:
#   - inline_data:     Inline Data optimization comparison
#   - unsignaled_send: Unsignaled Completion optimization comparison
#   - srq_demo:        Shared Receive Queue demo

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = -libverbs -L../../common -lrdma_utils

# Common utility library path
COMMON_LIB = ../../common/librdma_utils.a

TARGETS = inline_data unsignaled_send srq_demo

all: $(COMMON_LIB) $(TARGETS)

# Ensure common library is built
$(COMMON_LIB):
	$(MAKE) -C ../../common

inline_data: inline_data.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

unsignaled_send: unsignaled_send.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

srq_demo: srq_demo.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
