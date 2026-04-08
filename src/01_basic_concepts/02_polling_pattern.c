/**
 * 轮询模式练习
 * 
 * RDMA中使用轮询(polling)而非阻塞等待
 * 本程序演示常见的轮询模式
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/time.h>

/* 模拟RDMA中的Completion Queue */
#define CQ_SIZE 16

struct completion_entry {
    int status;      /* 0=success, 1=fail */
    int opcode;     /* 操作类型 */
    int byte_len;   // 传输字节数
};

struct completion_queue {
    struct completion_entry entries[CQ_SIZE];
    int head;
    int tail;
    int count;
};

/* 初始化CQ */
void cq_init(struct completion_queue *cq)
{
    memset(cq, 0, sizeof(struct completion_queue));
}

/* 模拟生产者：添加完成事件 */
int cq_push(struct completion_queue *cq, int status, int opcode, int len)
{
    if (cq->count >= CQ_SIZE) {
        return -1;  /* CQ满 */
    }
    
    cq->entries[cq->tail].status = status;
    cq->entries[cq->tail].opcode = opcode;
    cq->entries[cq->tail].byte_len = len;
    
    cq->tail = (cq->tail + 1) % CQ_SIZE;
    cq->count++;
    
    return 0;
}

/* 轮询模式1：非阻塞轮询 */
int poll_cq_nonblocking(struct completion_queue *cq, struct completion_entry *entry)
{
    if (cq->count == 0) {
        return 0;  /* 没有完成事件 */
    }
    
    /* 取出队首 */
    *entry = cq->entries[cq->head];
    cq->head = (cq->head + 1) % CQ_SIZE;
    cq->count--;
    
    return 1;
}

/* 轮询模式2：阻塞轮询（带超时） */
int poll_cq_blocking(struct completion_queue *cq, struct completion_entry *entry, int timeout_ms)
{
    struct timeval start, now;
    int elapsed = 0;
    
    gettimeofday(&start, NULL);
    
    while (elapsed < timeout_ms) {
        if (poll_cq_nonblocking(cq, entry)) {
            return 1;
        }
        
        usleep(1000);  /* 休眠1ms */
        gettimeofday(&now, NULL);
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + 
                  (now.tv_usec - start.tv_usec) / 1000;
    }
    
    return 0;  /* 超时 */
}

int main(int argc, char *argv[])
{
    struct completion_queue cq;
    struct completion_entry entry;
    int i;
    
    printf("=== 轮询模式练习 ===\n\n");
    
    /* 初始化CQ */
    cq_init(&cq);
    
    /* 模拟添加一些完成事件 */
    printf("模拟添加3个完成事件...\n");
    cq_push(&cq, 0, 1, 1024);  /* Send完成 */
    cq_push(&cq, 0, 2, 2048);  /* Write完成 */
    cq_push(&cq, 0, 3, 512);   /* Read完成 */
    
    /* 非阻塞轮询 */
    printf("\n--- 非阻塞轮询 ---\n");
    while (poll_cq_nonblocking(&cq, &entry)) {
        printf("完成事件: opcode=%d, bytes=%d, status=%s\n",
               entry.opcode, entry.byte_len,
               entry.status == 0 ? "SUCCESS" : "FAIL");
    }
    
    /* 再次添加事件 */
    cq_push(&cq, 0, 1, 4096);
    
    /* 阻塞轮询（带超时） */
    printf("\n--- 阻塞轮询 ---\n");
    if (poll_cq_blocking(&cq, &entry, 1000)) {
        printf("收到完成事件: opcode=%d, bytes=%d\n",
               entry.opcode, entry.byte_len);
    } else {
        printf("轮询超时\n");
    }
    
    printf("\n=== 练习完成 ===\n");
    printf("\n注意：RDMA中的ibv_poll_cq()类似这个poll_cq_nonblocking()\n");
    printf("     它是非阻塞的，需要在循环中不断调用\n");
    
    return 0;
}
