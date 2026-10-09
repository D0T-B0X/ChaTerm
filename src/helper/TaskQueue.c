#include "helper/TaskQueue.h"

#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>

#define TASK_LIMIT 128

typedef struct {
    Task*               head;
    Task*               tail;
    uint16_t            size;
} TaskQueue;

static TaskQueue        tq;
static pthread_mutex_t  taskQueueLock;
static pthread_cond_t   taskQueueCond;

void
initTaskQueue() {
    tq = (TaskQueue) {
        .head = NULL,
        .tail = NULL,
        .size = 0
    };

    taskQueueLock = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
    taskQueueCond = (pthread_cond_t) PTHREAD_COND_INITIALIZER;
}

uint8_t
pushTask(uint32_t clientFD, Command* command) {
    pthread_mutex_lock(&taskQueueLock);

    if (tq.size == TASK_LIMIT) { 
        pthread_mutex_unlock(&taskQueueLock);
        return QUEUE_FULL; 
    }

    // Allocate a new task node
    Task* task = malloc(sizeof(Task));
    if (!task) {
        pthread_mutex_unlock(&taskQueueLock);
        return QUEUE_FULL; // treat as full on allocation failure
    }

    task->sender = clientFD;
    task->command = *command;
    task->next = NULL;

    // The queue now owns the command; free the original pointer
    free(command);

    if (tq.head == NULL) {
        tq.head = task;
        tq.tail = task;
    } else {
        tq.tail->next = task;
        tq.tail = task;
    }
    tq.size++;

    // Signal one waiting thread (if any)
    pthread_cond_signal(&taskQueueCond);
    pthread_mutex_unlock(&taskQueueLock);

    return 0;
}

Task*
popTask() {
    pthread_mutex_lock(&taskQueueLock);

    while (tq.size == 0) { 
        pthread_cond_wait(&taskQueueCond, &taskQueueLock); 
    }

    Task* returnTask = tq.head;
    tq.head = tq.head->next;

    if (tq.head == NULL) { 
        tq.tail = NULL; 
    }

    tq.size--;

    pthread_mutex_unlock(&taskQueueLock);
    return returnTask;
}