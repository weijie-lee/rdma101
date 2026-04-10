/**
 * Polling Pattern Exercise
 *
 * RDMA uses polling instead of blocking waits.
 * This program demonstrates common polling patterns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/time.h>

/* Simulating RDMA Completion Queue */
#define CQ_SIZE 16

struct completion_entry {
    int status;      /* 0=success, 1=fail */
    int opcode;     /* Operation type */
    int byte_len;   // Bytes transferred
};

struct completion_queue {
    struct completion_entry entries[CQ_SIZE];
    int head;
    int tail;
    int count;
};

/* Initialize CQ */
void cq_init(struct completion_queue *cq)
{
    memset(cq, 0, sizeof(struct completion_queue));
}

/* Simulated producer: add completion event */
int cq_push(struct completion_queue *cq, int status, int opcode, int len)
{
    if (cq->count >= CQ_SIZE) {
        return -1;  /* CQ full */
    }
    
    cq->entries[cq->tail].status = status;
    cq->entries[cq->tail].opcode = opcode;
    cq->entries[cq->tail].byte_len = len;
    
    cq->tail = (cq->tail + 1) % CQ_SIZE;
    cq->count++;
    
    return 0;
}

/* Polling mode 1: non-blocking poll */
int poll_cq_nonblocking(struct completion_queue *cq, struct completion_entry *entry)
{
    if (cq->count == 0) {
        return 0;  /* No completion events */
    }
    
    /* Dequeue head element */
    *entry = cq->entries[cq->head];
    cq->head = (cq->head + 1) % CQ_SIZE;
    cq->count--;
    
    return 1;
}

/* Polling mode 2: blocking poll (with timeout) */
int poll_cq_blocking(struct completion_queue *cq, struct completion_entry *entry, int timeout_ms)
{
    struct timeval start, now;
    int elapsed = 0;
    
    gettimeofday(&start, NULL);
    
    while (elapsed < timeout_ms) {
        if (poll_cq_nonblocking(cq, entry)) {
            return 1;
        }
        
        usleep(1000);  /* Sleep 1ms */
        gettimeofday(&now, NULL);
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + 
                  (now.tv_usec - start.tv_usec) / 1000;
    }
    
    return 0;  /* Timeout */
}

int main(int argc, char *argv[])
{
    struct completion_queue cq;
    struct completion_entry entry;
    int i;
    
    printf("=== Polling Pattern Exercise ===\n\n");
    
    /* Initialize CQ */
    cq_init(&cq);
    
    /* Simulate adding some completion events */
    printf("Simulating adding 3 completion events...\n");
    cq_push(&cq, 0, 1, 1024);  /* Send complete */
    cq_push(&cq, 0, 2, 2048);  /* Write complete */
    cq_push(&cq, 0, 3, 512);   /* Read complete */
    
    /* Non-blocking poll */
    printf("\n--- Non-blocking Poll ---\n");
    while (poll_cq_nonblocking(&cq, &entry)) {
        printf("Completion event: opcode=%d, bytes=%d, status=%s\n",
               entry.opcode, entry.byte_len,
               entry.status == 0 ? "SUCCESS" : "FAIL");
    }
    
    /* Add event again */
    cq_push(&cq, 0, 1, 4096);
    
    /* Blocking poll (with timeout) */
    printf("\n--- Blocking Poll ---\n");
    if (poll_cq_blocking(&cq, &entry, 1000)) {
        printf("Received completion event: opcode=%d, bytes=%d\n",
               entry.opcode, entry.byte_len);
    } else {
        printf("Poll timed out\n");
    }
    
    printf("\n=== Exercise Complete ===\n");
    printf("\nNote: ibv_poll_cq() in RDMA is similar to this poll_cq_nonblocking()\n");
    printf("     It is non-blocking and needs to be called repeatedly in a loop\n");
    
    return 0;
}
