/**
 * TCP Server Example
 * Demonstrates basic socket programming
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 9999
#define BUFFER_SIZE 1024

int main(int argc, char *argv[])
{
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    char buffer[BUFFER_SIZE];
    
    printf("TCP Server Example\n");
    printf("==================\n\n");
    
    // 1. Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return 1;
    }
    printf("[1] Socket created: fd=%d\n", server_fd);
    
    // 2. Bind address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;          // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Any IP
    server_addr.sin_port = htons(PORT);       // Port
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        return 1;
    }
    printf("[2] Bound to port %d\n", PORT);
    
    // 3. Listen
    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        return 1;
    }
    printf("[3] Listening...\n");
    
    // 4. Accept connection
    client_len = sizeof(client_addr);
    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd < 0) {
        perror("accept failed");
        return 1;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    printf("[4] Client connected: %s:%d\n", client_ip, ntohs(client_addr.sin_port));
    
    // 5. Receive data
    memset(buffer, 0, BUFFER_SIZE);
    ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
    
    if (n > 0) {
        printf("[5] Received (%zd bytes): %s\n", n, buffer);
    }
    
    // 6. Send reply
    const char *reply = "Hello from server!";
    write(client_fd, reply, strlen(reply));
    printf("[6] Reply sent\n");
    
    // 7. Close connection
    close(client_fd);
    close(server_fd);
    printf("[7] Connection closed\n");
    
    return 0;
}
