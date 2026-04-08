/**
 * 结构体和指针练习
 * 
 * RDMA编程中大量使用结构体和指针，本程序演示：
 * 1. 结构体定义和初始化
 * 2. 结构体指针使用
 * 3. 内存分配和释放
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RDMA中常见的结构体定义 */
struct rdma_device {
    char name[32];
    char guid[32];
    int port_num;
};

/* 类似RDMA中的ibv_context */
struct rdma_context {
    struct rdma_device *device;
    int fd;
    void *priv;
};

/* 动态创建结构体 */
struct rdma_context* create_context(const char *dev_name)
{
    struct rdma_context *ctx;
    
    /* 分配内存 */
    ctx = malloc(sizeof(struct rdma_context));
    if (!ctx) {
        return NULL;
    }
    
    /* 清零 */
    memset(ctx, 0, sizeof(struct rdma_context));
    
    /* 分配设备结构体 */
    ctx->device = malloc(sizeof(struct rdma_device));
    if (!ctx->device) {
        free(ctx);
        return NULL;
    }
    
    /* 设置设备信息 */
    strncpy(ctx->device->name, dev_name, sizeof(ctx->device->name) - 1);
    ctx->device->port_num = 1;
    ctx->fd = 123;  /* 模拟文件描述符 */
    
    return ctx;
}

/* 释放结构体 */
void destroy_context(struct rdma_context *ctx)
{
    if (!ctx) return;
    
    if (ctx->device) {
        free(ctx->device);
    }
    free(ctx);
}

int main(int argc, char *argv[])
{
    struct rdma_context *ctx;
    
    printf("=== 结构体指针练习 ===\n\n");
    
    /* 创建上下文 */
    ctx = create_context("mlx5_0");
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* 使用指针访问成员 */
    printf("Device: %s\n", ctx->device->name);
    printf("Port: %d\n", ctx->device->port_num);
    printf("FD: %d\n", ctx->fd);
    
    /* 使用箭头操作符 */
    printf("\nUsing arrow operator:\n");
    printf("Device: %s\n", ctx->device->name);
    
    /* 修改成员 */
    ctx->device->port_num = 2;
    printf("Modified port: %d\n", ctx->device->port_num);
    
    /* 释放内存 */
    destroy_context(ctx);
    
    printf("\n=== 练习完成 ===\n");
    return 0;
}
