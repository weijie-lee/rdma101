# 第零章：预备知识

## 学习目标
- 掌握Socket编程基础
- 理解TCP/UDP通信流程
- 能够编写简单的C/S程序
- 为RDMA连接建立打下基础

---

## 0.1 Socket编程概述

Socket（套接字）是网络通信的抽象接口，是应用程序与网络协议栈之间的桥梁。

### 为什么RDMA需要Socket？

RDMA本身是"直连"通信，但需要**建立连接**：
1. 交换QP信息（QP号、LID/GID）
2. 交换MR信息（内存地址、rkey）
3. 协商通信参数

这些信息交换通常通过**传统Socket**完成。

---

## 0.2 TCP Server/Client 模型

### TCP通信流程

```
Server                              Client
  │                                   │
  │  1. socket()                      │
  │  2. bind()                        │
  │  3. listen()                      │
  │  4. accept() ◀─── connect() ──────│
  │  5. read()/write()                │
  │  6. close()                       │
```

### Server 代码框架
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int server_fd, client_fd;
    struct sockaddr_in addr, client_addr;
    char buffer[1024];
    socklen_t client_len;
    
    // 1. 创建socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // 2. 绑定地址
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    
    // 3. 监听
    listen(server_fd, 5);
    
    // 4. 接受连接
    client_len = sizeof(client_addr);
    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    
    // 5. 通信
    read(client_fd, buffer, sizeof(buffer));
    printf("Received: %s\n", buffer);
    write(client_fd, "OK", 2);
    
    // 6. 关闭
    close(client_fd);
    close(server_fd);
    
    return 0;
}
```

### Client 代码框架
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int sock_fd;
    struct sockaddr_in server_addr;
    
    // 1. 创建socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // 2. 连接服务器
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    // 3. 通信
    write(sock_fd, "Hello", 5);
    char buffer[1024] = {0};
    read(sock_fd, buffer, sizeof(buffer));
    printf("Server: %s\n", buffer);
    
    // 4. 关闭
    close(sock_fd);
    
    return 0;
}
```

---

## 0.3 UDP Server/Client 模型

### UDP通信流程（无连接）

```
Server                              Client
  │                                   │
  │  1. socket()                      │
  │  2. bind()                        │
  │  3. recvfrom()/sendto() ──────────│
  │◀──────────────────────────────────│
  │  4. close()                       │
```

### UDP Server
```c
int main() {
    int sock_fd;
    struct sockaddr_in addr, client_addr;
    char buffer[1024];
    socklen_t len;
    
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr));
    
    // 接收数据
    len = sizeof(client_addr);
    recvfrom(sock_fd, buffer, sizeof(buffer), 0, 
             (struct sockaddr*)&client_addr, &len);
    
    printf("Received: %s\n", buffer);
    
    // 发送回复
    sendto(sock_fd, "OK", 2, 0, 
           (struct sockaddr*)&client_addr, len);
    
    close(sock_fd);
    return 0;
}
```

---

## 0.4 网络字节序

### 什么是字节序？

| 概念 | 说明 |
|------|------|
| **大端序 (Big-Endian)** | 高字节在低地址 |
| **小端序 (Little-Endian)** | 低字节在低地址 |
| **网络字节序** | 统一使用大端序 |

### 字节序转换函数

```c
#include <arpa/inet.h>

// 主机序 → 网络序
uint32_t htonl(uint32_t hostlong);   // 32位
uint16_t htons(uint16_t hostshort);   // 16位

// 网络序 → 主机序
uint32_t ntohl(uint32_t netlong);    // 32位
uint16_t ntohs(uint16_t netshort);   // 16位
```

### 使用示例
```c
uint16_t port = 8080;
uint16_t net_port = htons(port);  // 转换为网络字节序

uint32_t ip = inet_addr("192.168.1.100");  // 字符串转IP
// 或
inet_pton(AF_INET, "192.168.1.100", &addr.sin_addr);  // 推荐
```

---

## 0.5 结构体对齐

### sockaddr_in 结构体
```c
struct sockaddr_in {
    sa_family_t    sin_family;    // 地址族 (AF_INET)
    in_port_t      sin_port;      // 端口号 (网络字节序)
    struct in_addr sin_addr;      // IP地址
    char           sin_zero[8];   // 填充(必须为0)
};

struct in_addr {
    in_addr_t s_addr;  // 32位IP地址
};
```

### 内存操作
```c
// 清零
memset(&addr, 0, sizeof(addr));

// 复制
memcpy(dest, src, size);
```

---

## 0.6 RDMA连接信息交换

### 需要交换的信息

RDMA建立连接需要交换以下信息：

```c
struct rdma_connection_info {
    uint32_t qp_num;      // QP编号
    uint16_t lid;         // 本地ID (InfiniBand)
    uint32_t gid_index;   // GID索引 (RoCE)
    uint64_t buf_addr;    // 缓冲区地址
    uint32_t buf_rkey;    // 远程密钥
};
```

### 交换流程

```
┌─────────────────────────────────────────────────────┐
│                  TCP 握手阶段                        │
├─────────────────────────────────────────────────────┤
│  Server                          Client             │
│    │                                │               │
│    │◀─── connect() ────────────────│               │
│    │───── accept() ────────────────▶│               │
│    │                                │               │
│    │      交换 QP 信息               │               │
│    │◀──── QP_num, LID ─────────────│               │
│    │──── QP_num, LID ──────────────▶│               │
│    │                                │               │
│    │      交换 MR 信息               │               │
│    │◀──── addr, rkey ──────────────│               │
│    │──── addr, rkey ───────────────▶│               │
│    │                                │               │
├─────────────────────────────────────────────────────┤
│                  RDMA 连接建立                        │
├─────────────────────────────────────────────────────┤
│  Server                          Client             │
│    │                                │               │
│    │  QP: INIT → RTR → RTS          │               │
│    │                                │  QP: INIT → RTR → RTS │
│    │                                │               │
├─────────────────────────────────────────────────────┤
│                  RDMA 通信阶段                        │
└─────────────────────────────────────────────────────┘
```

---

## 0.7 编译与运行

### 编译
```bash
gcc -o server server.c
gcc -o client client.c
```

### 运行
```bash
# 终端1
./server 0.0.0.0 9999

# 终端2
./client 127.0.0.1 9999
```

---

## 📂 相关示例

本教程中，所有RDMA示例都包含内置的Socket交换功能：
- `send_recv.c` - 通过TCP交换QP/MR信息
- `rdma_read.c` - 通过TCP交换连接信息

---

## 练习题

1. TCP和UDP的主要区别是什么？
2. 为什么RDMA需要先用Socket交换信息？
3. htons和htonl的区别是什么？
4. sockaddr_in中sin_zero的作用？

---

## 下一步

进入下一章：[第一章：RDMA基础概念](../ch01-intro/README.md)
