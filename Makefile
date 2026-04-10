# RDMA 101 Tutorial - Root Makefile
#
# Usage:
#   make all       - Build all example programs
#   make clean     - Clean all build artifacts
#   make common    - Build common library only
#   make setup     - One-click SoftRoCE environment setup (requires sudo)
#   make test      - Run full test suite (requires SoftRoCE)
#   make check-env - Check RDMA environment status

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

# Build common library first, then each chapter
all: common chapters

# Common utility library (dependency for all programs)
common:
	$(MAKE) -C common

# Build chapters in order
chapters: common
	@echo "=== Building ch00-prerequisites ==="
	-$(MAKE) -C ch00-prerequisites
	@echo "=== Building ch02-network-layer ==="
	-$(MAKE) -C ch02-network-layer/01-infiniband
	-$(MAKE) -C ch02-network-layer/02-roce
	-$(MAKE) -C ch02-network-layer/03-iwarp
	-$(MAKE) -C ch02-network-layer/04-verbs-abstraction
	@echo "=== Building ch03-verbs-api ==="
	-$(MAKE) -C ch03-verbs-api/01-initialization
	-$(MAKE) -C ch03-verbs-api/02-qp-state
	-$(MAKE) -C ch03-verbs-api/03-pd
	-$(MAKE) -C ch03-verbs-api/04-mr
	-$(MAKE) -C ch03-verbs-api/05-cq
	@echo "=== Building ch05-communication ==="
	-$(MAKE) -C ch05-communication/01-rdma-write
	-$(MAKE) -C ch05-communication/02-send-recv
	-$(MAKE) -C ch05-communication/03-rdma-read
	-$(MAKE) -C ch05-communication/04-atomic
	@echo "=== Building ch06-connection ==="
	-$(MAKE) -C ch06-connection/01-manual-connect
	-$(MAKE) -C ch06-connection/02-rdma-cm
	-$(MAKE) -C ch06-connection/03-ud-mode
	@echo "=== Building ch07-engineering ==="
	-$(MAKE) -C ch07-engineering/02-tuning
	-$(MAKE) -C ch07-engineering/04-error-handling
	@echo "=== Building ch09-quickref ==="
	-$(MAKE) -C ch09-quickref
	@echo ""
	@echo "=== Build complete ==="

clean:
	$(MAKE) -C common clean
	-$(MAKE) -C ch00-prerequisites clean
	-$(MAKE) -C ch02-network-layer/01-infiniband clean
	-$(MAKE) -C ch02-network-layer/02-roce clean
	-$(MAKE) -C ch02-network-layer/03-iwarp clean
	-$(MAKE) -C ch02-network-layer/04-verbs-abstraction clean
	-$(MAKE) -C ch03-verbs-api/01-initialization clean
	-$(MAKE) -C ch03-verbs-api/02-qp-state clean
	-$(MAKE) -C ch03-verbs-api/03-pd clean
	-$(MAKE) -C ch03-verbs-api/04-mr clean
	-$(MAKE) -C ch03-verbs-api/05-cq clean
	-$(MAKE) -C ch05-communication/01-rdma-write clean
	-$(MAKE) -C ch05-communication/02-send-recv clean
	-$(MAKE) -C ch05-communication/03-rdma-read clean
	-$(MAKE) -C ch05-communication/04-atomic clean
	-$(MAKE) -C ch06-connection/01-manual-connect clean
	-$(MAKE) -C ch06-connection/02-rdma-cm clean
	-$(MAKE) -C ch06-connection/03-ud-mode clean
	-$(MAKE) -C ch07-engineering/02-tuning clean
	-$(MAKE) -C ch07-engineering/04-error-handling clean
	-$(MAKE) -C ch09-quickref clean

.PHONY: all common chapters clean setup test check-env

# ========== SoftRoCE Shortcuts ==========

# One-click SoftRoCE environment setup
setup:
	@echo "=== Setting up SoftRoCE environment ==="
	@sudo ./scripts/setup_softrce.sh

# Run full test suite
test: all
	@echo "=== Running full test suite ==="
	@./scripts/run_all_tests.sh

# Check environment status
check-env:
	@echo "=== Checking RDMA environment ==="
	@./ch09-quickref/env_check.sh
