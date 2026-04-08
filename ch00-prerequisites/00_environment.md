# 环境搭建指南

## 学习目标

| 目标 | 说明 |
|------|------|
| 安装开发环境 | 掌握Ubuntu下RDMA开发环境搭建 |
| 配置SoftRoCE | 使用软件模拟RDMA环境（无需真实网卡） |
| 验证环境 | 确保ibv_devinfo正常工作 |

---

## 1.1 Ubuntu开发环境搭建

### 安装依赖

```bash
# 更新软件源
sudo apt update

# 安装RDMA开发库
sudo apt install -y libibverbs-dev librdmacm-dev

# 安装工具和测试软件
sudo apt install -y ibverbs-utils rdma-core perftest

# 安装编译工具
sudo apt install -y build-essential gcc gdb make
```

### 验证安装

```bash
# 检查库文件
ls -la /usr/lib/x86_64-linux-gnu/libibverbs*

# 检查头文件
ls -la /usr/include/infiniband/
```

---

## 1.2 SoftRoCE配置（软件模拟）

**注意**：如果没有真实RDMA网卡，使用SoftRoCE软件模拟

### 加载内核模块

```bash
# 检查是否已加载
lsmod | grep rdma

# 加载rdma_rxe模块
sudo modprobe rdma_rxe
```

### 绑定网卡

```bash
# 查看网卡列表
ip link show

# 假设网卡名为eth0，绑定到SoftRoCE
sudo rdma link add rxe0 type rxe netdev eth0

# 查看已绑定的设备
rdma link
```

### 验证环境

```bash
# 查看RDMA设备
ibv_devices

# 查看设备详细信息
ibv_devinfo -d rxe0
```

**预期输出**：
```
device: rxe0
    transport:              InfiniBand (0)
    fw_ver:                N/A
    node_guid:             0000:0000:0000:0000
    sys_image_guid:        0000:0000:0000:0000
    vendor_id:             000000
    vendor_part_id:        0
    hw_ver:                0x00000000
    phys_port_cnt:         1
        port 1:
            state: PORT_ACTIVE (4)
            max_mtu: 4096 (5)
            active_mtu: 1024 (3)
            sm_lid:           0
            port_lid:         0
            port_lmc:         0x00
            link_layer:       Ethernet
```

---

## 1.3 快速测试

### 基础连接测试

```bash
# 终端1 - 服务端
ibv_rc_pingpong -g 0 -d rxe0

# 终端2 - 客户端
ibv_rc_pingpong -g 0 -d rxe0 <server_ip>
```

### 带宽测试

```bash
# 发送端
ibv_send_bw -d rxe0

# 接收端
ibv_send_bw -d rxe0 <server_ip>
```

---

## 1.4 编译命令速查

### 编译选项

```bash
# 前置练习（不需要RDMA库）
gcc -o program program.c

# RDMA程序
gcc -o program program.c -libverbs -lrdmacm

# 或者用Makefile
make
make clean
```

### Makefile模板

```makefile
CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs

TARGET = program

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
```

---

## 1.5 常见问题排查

| 问题 | 原因 | 解决方法 |
|------|------|----------|
| `ibv_devices` 无输出 | SoftRoCE未配置 | 检查 `rdma link` |
| `ibv_devinfo` 报错 | 权限不足 | 使用 `sudo` 运行 |
| 编译报错找不到头文件 | 库未安装 | `apt install libibverbs-dev` |
| 运行时报错 | QP状态不对 | 检查状态机转换 |

---

## 下一步

进入下一章：[第二章：前置基础（C语言）](../src/01_basic_concepts/README.md)
