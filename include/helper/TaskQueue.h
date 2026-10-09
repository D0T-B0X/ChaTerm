#ifndef __TASK_QUEUE_H__
#define __TASK_QUEUE_H__

#include <stdint.h>

#include "Common.h"

// Error codes
#define QUEUE_FULL 1

/**
 * @brief 
 * 
 */
void initTaskQueue();

/**
 * @brief 
 * 
 * @return uint8_t 
 */
uint8_t pushTask(uint32_t FD, Command* command);

/**
 * @brief 
 * 
 */
Task* popTask();

#endif // __TASK_QUEUE_H__