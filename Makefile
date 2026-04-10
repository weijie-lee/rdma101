# RDMA 101 教程 - 根 Makefile
#
# 使用方法:
#   make all       - 编译所有示例程序
#   make clean     - 清理所有编译产物
#   make common    - 仅编译公共库
#   make setup     - 一键配置 SoftRoCE 环境 (需要 sudo)
#   make test      - 运行全量测试 (需要 SoftRoCE)
#   make check-env - 检查 RDMA 环境状态

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

# 先编译公共库，再编译各章节
all: common chapters

# 公共工具库 (所有程序的依赖)
common:
	$(MAKE) -C common

# 各章节按顺序编译
chapters: common
	@echo "=== 编译 ch00-prerequisites ==="
	-$(MAKE) -C ch00-prerequisites
	@echo "=== 编译 ch02-network-layer ==="
	-$(MAKE) -C ch02-network-layer/01-infiniband
	-$(MAKE) -C ch02-network-layer/02-roce
	-$(MAKE) -C ch02-network-layer/03-iwarp
	-$(MAKE) -C ch02-network-layer/04-verbs-abstraction
	@echo "=== 编译 ch03-verbs-api ==="
	-$(MAKE) -C ch03-verbs-api/01-initialization
	-$(MAKE) -C ch03-verbs-api/02-qp-state
	-$(MAKE) -C ch03-verbs-api/03-pd
	-$(MAKE) -C ch03-verbs-api/04-mr
	-$(MAKE) -C ch03-verbs-api/05-cq
	@echo "=== 编译 ch05-communication ==="
	-$(MAKE) -C ch05-communication/01-rdma-write
	-$(MAKE) -C ch05-communication/02-send-recv
	-$(MAKE) -C ch05-communication/03-rdma-read
	-$(MAKE) -C ch05-communication/04-atomic
	@echo "=== 编译 ch06-connection ==="
	-$(MAKE) -C ch06-connection/01-manual-connect
	-$(MAKE) -C ch06-connection/02-rdma-cm
	-$(MAKE) -C ch06-connection/03-ud-mode
	@echo "=== 编译 ch07-engineering ==="
	-$(MAKE) -C ch07-engineering/02-tuning
	-$(MAKE) -C ch07-engineering/04-error-handling
	@echo "=== 编译 ch09-quickref ==="
	-$(MAKE) -C ch09-quickref
	@echo ""
	@echo "=== 编译完成 ==="

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

# ========== SoftRoCE 快捷命令 ==========

# 一键配置 SoftRoCE 环境
setup:
	@echo "=== 配置 SoftRoCE 环境 ==="
	@sudo ./scripts/setup_softrce.sh

# 运行全量测试
test: all
	@echo "=== 运行全量测试 ==="
	@./scripts/run_all_tests.sh

# 检查环境状态
check-env:
	@echo "=== 检查 RDMA 环境 ==="
	@./ch09-quickref/env_check.sh
