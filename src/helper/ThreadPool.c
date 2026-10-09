#include "helper/ThreadPool.h"
#include "helper/TaskQueue.h"
#include "helper/Worker.h"
#include "Common.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#define POOL_SIZE 12

static void*
workerLoop(void* args) {
    while(true) {
        Task* task = popTask();
        if (!task) continue;

        CommandType commandType = task->command.commandType;
        if (commandType == REGISTER) { Worker_registerUser(task); }
        else if (commandType == CONNECT) { Worker_connectUser(task); }
        else if (commandType == SEND_TO) { Worker_sendMessage(task); }
        else if (commandType == LIST) { Worker_listOnlineUsers(task); }
        else if (commandType == QUIT) { Worker_sendGoodbye(task); }
        else if (
            commandType == SENDFILE_TO || 
            commandType == SENDFILE_DATA || 
            commandType == SENDFILE_END
        ) { Worker_sendFile(task); }
        else { Worker_handleUnrecognizedCommand(task->sender); }

        free(task);
    }

    return NULL;
}

void
initThreadPool() {
    pthread_t threadPool[POOL_SIZE];

    for(int thread = 0; thread < POOL_SIZE; ++thread) {
        if (pthread_create(&threadPool[thread], NULL, workerLoop, NULL) != 0) {
            printf("CRITICAL ERROR: failed to create worker threads\n");
            exit(EXIT_FAILURE);
        }

        pthread_detach(threadPool[thread]);
    }
}