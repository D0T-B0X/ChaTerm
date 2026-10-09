#ifndef __SERVER_H__
#define __SERVER_H__

#include <stddef.h>

#include "Common.h"

// Error codes
#define BUFFER_OVERFLOW 1
#define TASK_QUEUE_FULL 2

/**
 * @brief 
 * 
 */
void enableServer();

/**
 * @brief 
 * 
 */
void serverLoop();

#endif // __SERVER_H__