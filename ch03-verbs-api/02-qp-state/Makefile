# Makefile for ch03-verbs-api/02-qp-state

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

COMMON_DIR = ../../common
COMMON_INC = -I$(COMMON_DIR)
COMMON_LIB = $(COMMON_DIR)/librdma_utils.a

TARGETS = qp_state 01_qp_error_recovery

all: $(TARGETS)

qp_state: qp_state.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

01_qp_error_recovery: 01_qp_error_recovery.c $(COMMON_LIB)
	$(CC) $(CFLAGS) $(COMMON_INC) -o $@ $< $(COMMON_LIB) $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
