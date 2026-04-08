/**
 * TCP Client 示例
 * 演示基本的socket编程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 1024

int main(int argc, char *argv[])
{
    int sock_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    
    printf("TCP Client Example\n");
    printf("==================\n\n");
    
    // 1. 创建socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket failed");
        return 1;
    }
    printf("[1] Socket created: fd=%d\n", sock_fd);
    
    // 2. 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // 转换IP地址
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return 1;
    }
    printf("[2] Server address set: %s:%d\n", server_ip, port);
    
    // 3. 连接服务器
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        return 1;
    }
    printf("[3] Connected to server\n");
    
    // 4. 发送数据
    const char *message = "Hello from client!";
    write(sock_fd, message, strlen(message));
    printf("[4] Sent: %s\n", message);
    
    // 5. 接收回复
    memset(buffer, 0, BUFFER_SIZE);
    ssize_t n = read(sock_fd, buffer, BUFFER_SIZE - 1);
    
    if (n > 0) {
        printf("[5] Received (%zd bytes): %s\n", n, buffer);
    }
    
    // 6. 关闭socket
    close(sock_fd);
    printf("[6] Connection closed\n");
    
    return 0;
}
