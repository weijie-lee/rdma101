# ch07-engineering/04-error-handling Makefile
#
# Build two error handling demo programs:
#   - trigger_errors:   Deliberately trigger RDMA errors and demonstrate recovery
#   - error_diagnosis:  RDMA error code diagnostic tool

CC = gcc
CFLAGS = -Wall -O2 -g -I../../common
LDFLAGS = -libverbs -L../../common -lrdma_utils

# Common utility library path
COMMON_LIB = ../../common/librdma_utils.a

TARGETS = trigger_errors error_diagnosis

all: $(COMMON_LIB) $(TARGETS)

# Ensure common library is built
$(COMMON_LIB):
	$(MAKE) -C ../../common

trigger_errors: trigger_errors.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

error_diagnosis: error_diagnosis.c $(COMMON_LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
