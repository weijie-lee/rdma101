# 第零章：Socket编程预备知识

## 学习目标

| 目标 | 说明 |
|------|------|
| 理解Socket基本概念 | 理解Socket是什么，为什么需要Socket |
| 掌握TCP/UDP编程 | 能够编写简单的TCP/UDP服务器和客户端 |
| 理解网络字节序 | 掌握htons/htonl等函数的使用 |
| 理解RDMA信息交换 | 理解RDMA如何通过Socket交换连接信息 |

---

## 0.1 Socket概述

### 什么是Socket？

Socket（套接字）是**应用程序与网络协议栈之间的接口**。

```
┌────────────────────────────────────────┐
│           应用程序 A                    │
│                                        │
│         ┌──────────┐                   │
│         │  Socket  │ ◀── 编程接口      │
│         └──────────┘                   │
└────────────┬───────────────────────────┘
             │
             ▼
┌────────────────────────────────────────┐
│           操作系统内核                   │
│         TCP/UDP 协议栈                   │
└────────────┬───────────────────────────┘
             │
             ▼
┌────────────────────────────────────────┐
│           网卡驱动                       │
└────────────────────────────────────────┘
```

### Socket类型

| 类型 | 说明 | 特点 |
|------|------|------|
| **SOCK_STREAM** | TCP | 面向连接、可靠 |
| **SOCK_DGRAM** | UDP | 无连接、快速 |
| **SOCK_RDM** | RDMA | 可靠数据报 |

---

## 0.2 TCP编程

### TCP通信流程

```
Server                              Client
  │                                   │
  │ 1. socket() 创建socket            │
  │ 2. bind() 绑定地址端口            │
  │ 3. listen() 监听                 │
  │ 4. accept() ◀── connect() ──────│
  │ 5. read()/write()                │
  │ 6. close()                       │
```

### 关键API说明

#### socket() - 创建socket
```c
int sock = socket(AF_INET, SOCK_STREAM, 0);
// 参数: 地址族, socket类型, 协议(0=自动选择)
// 返回: 文件描述符
```

#### bind() - 绑定地址
```c
struct sockaddr_in addr;
addr.sin_family = AF_INET;           // IPv4
addr.sin_port = htons(8080);        // 端口(网络字节序)
addr.sin_addr.s_addr = INADDR_ANY;  // 任意IP

bind(sock, (struct sockaddr*)&addr, sizeof(addr));
```

#### listen() - 监听
```c
listen(sock, 5);  // 5: 队列长度
```

#### accept() - 接受连接
```c
int client_fd = accept(sock, &client_addr, &len);
// 返回: 客户端socket描述符
```

#### connect() - 连接服务器
```c
struct sockaddr_in server_addr;
// ... 设置服务器地址 ...
connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
```

---

## 0.3 UDP编程

### UDP通信流程（无连接）

```
Server                              Client
  │                                   │
  │ 1. socket()                       │
  │ 2. bind()                         │
  │ 3. recvfrom() ◀─── sendto() ─────│
  │ 4. sendto() ────▶ recvfrom()     │
```

### 关键API说明

#### recvfrom() - 接收数据
```c
char buffer[1024];
struct sockaddr_in client_addr;
socklen_t len = sizeof(client_addr);

recvfrom(sock, buffer, sizeof(buffer), 0,
         (struct sockaddr*)&client_addr, &len);
```

#### sendto() - 发送数据
```c
sendto(sock, data, len, 0,
       (struct sockaddr*)&client_addr, sizeof(client_addr));
```

---

## 0.4 网络字节序

### 字节序概念

| 字节序 | 说明 | 示例 (0x12345678) |
|--------|------|-------------------|
| **大端序** | 高字节在低地址 | 12 34 56 78 |
| **小端序** | 低字节在低地址 | 78 56 34 12 |

**网络字节序统一使用大端序**

### 转换函数

```c
#include <arpa/inet.h>

// 主机序 → 网络序
uint32_t htonl(uint32_t hostlong);   // 32位 (IP)
uint16_t htons(uint16_t hostshort);   // 16位 (端口)

// 网络序 → 主机序
uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);
```

### 使用示例
```c
uint16_t port = 8080;
uint16_t net_port = htons(port);  // 小端→大端

uint32_t ip = inet_addr("192.168.1.1");  // 字符串转IP
// 推荐方式:
inet_pton(AF_INET, "192.168.1.1", &addr.sin_addr);
```

---

## 0.5 RDMA连接信息交换

### 为什么需要Socket交换信息？

RDMA是**点对点直接通信**，但在建立连接前需要知道对端的：
1. **QP号** - 目标QP
2. **LID/GID** - 目标网卡地址
3. **MR信息** - 可访问的内存区域

这些信息通过传统Socket交换。

### 信息交换流程

```
┌─────────────────────────────────────────────────────┐
│                   TCP 握手阶段                        │
├─────────────────────────────────────────────────────┤
│  Server                          Client             │
│    │                                │               │
│    │◀─── connect() ───────────────│               │
│    │────── accept() ──────────────▶│               │
│    │                                │               │
│    │      交换 QP 信息               │               │
│    │◀──── QP_num, LID ─────────────│               │
│    │──── QP_num, LID ──────────────▶│               │
│    │                                │               │
│    │      交换 MR 信息               │               │
│    │◀──── addr, rkey ──────────────│               │
│    │──── addr, rkey ───────────────▶│               │
├─────────────────────────────────────────────────────┤
│                   RDMA 连接建立                      │
│  Server QP: INIT → RTR → RTS                       │
│  Client QP: INIT → RTR → RTS                       │
├─────────────────────────────────────────────────────┤
│                   RDMA 通信                         │
└─────────────────────────────────────────────────────┘
```

### 交换的数据结构

```c
struct rdma_info {
    uint32_t qp_num;      // QP编号
    uint16_t lid;         // 本地ID (InfiniBand)
    uint32_t gid_index;  // GID索引 (RoCE)
    uint64_t buf_addr;   // 缓冲区虚拟地址
    uint32_t buf_rkey;   // 远程密钥
};
```

---

## 0.6 编译与运行

### 编译
```bash
gcc -o server tcp_server.c
gcc -o client tcp_client.c
```

### 运行测试

```bash
# 终端1 - 启动服务器
./tcp_server

# 终端2 - 启动客户端
./tcp_client 127.0.0.1 9999
```

### 预期输出

**Server端:**
```
[1] Socket created: fd=3
[2] Bound to port 9999
[3] Listening...
[4] Client connected: 127.0.0.1:xxxxx
[5] Received (18 bytes): Hello from client!
[6] Reply sent
[7] Connection closed
```

**Client端:**
```
[1] Socket created: fd=3
[2] Server address set: 127.0.0.1:9999
[3] Connected to server
[4] Sent: Hello from client!
[5] Received (17 bytes): Hello from server!
[6] Connection closed
```

---

## 0.7 常见错误排查

| 错误 | 原因 | 解决方法 |
|------|------|----------|
| `bind failed: Address already in use` | 端口被占用 | 更换端口或等待释放 |
| `connect failed: Connection refused` | 服务器未启动 | 先启动服务器 |
| `Invalid address` | IP地址格式错误 | 检查IP地址格式 |
| 客户端收不到数据 | 防火墙阻塞 | 关闭防火墙或开放端口 |

---

## 练习题

1. **简答题**: TCP和UDP的主要区别是什么？
2. **编程题**: 编写一个UDP服务器，接收客户端消息并打印
3. **概念题**: 为什么RDMA需要先用Socket交换信息？
4. **转换题**: 将端口8080转换为网络字节序

---

## 下一步

进入下一章：[第一章：RDMA基础概念](../ch01-intro/README.md)
