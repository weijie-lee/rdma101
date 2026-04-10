/**
 * Struct and Pointer Exercise
 *
 * RDMA programming heavily uses structs and pointers. This program demonstrates:
 * 1. Struct definition and initialization
 * 2. Struct pointer usage
 * 3. Memory allocation and deallocation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Common struct definition in RDMA */
struct rdma_device {
    char name[32];
    char guid[32];
    int port_num;
};

/* Similar to ibv_context in RDMA */
struct rdma_context {
    struct rdma_device *device;
    int fd;
    void *priv;
};

/* Dynamically create struct */
struct rdma_context* create_context(const char *dev_name)
{
    struct rdma_context *ctx;
    
    /* Allocate memory */
    ctx = malloc(sizeof(struct rdma_context));
    if (!ctx) {
        return NULL;
    }
    
    /* Zero out */
    memset(ctx, 0, sizeof(struct rdma_context));
    
    /* Allocate device struct */
    ctx->device = malloc(sizeof(struct rdma_device));
    if (!ctx->device) {
        free(ctx);
        return NULL;
    }
    
    /* Set device information */
    strncpy(ctx->device->name, dev_name, sizeof(ctx->device->name) - 1);
    ctx->device->port_num = 1;
    ctx->fd = 123;  /* Simulated file descriptor */
    
    return ctx;
}

/* Free struct */
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
    
    printf("=== Struct Pointer Exercise ===\n\n");
    
    /* Create context */
    ctx = create_context("mlx5_0");
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    /* Access members via pointer */
    printf("Device: %s\n", ctx->device->name);
    printf("Port: %d\n", ctx->device->port_num);
    printf("FD: %d\n", ctx->fd);
    
    /* Using arrow operator */
    printf("\nUsing arrow operator:\n");
    printf("Device: %s\n", ctx->device->name);
    
    /* Modify member */
    ctx->device->port_num = 2;
    printf("Modified port: %d\n", ctx->device->port_num);
    
    /* Free memory */
    destroy_context(ctx);
    
    printf("\n=== Exercise Complete ===\n");
    return 0;
}
