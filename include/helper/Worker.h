#ifndef __WORKER_H__
#define __WORKER_H__

#include "Common.h"

/**
 * @brief 
 * 
 * @param task 
 * @return true 
 * @return false 
 */
bool Worker_registerUser(Task* task);

/**
 * @brief 
 * 
 * @param task 
 * @return true 
 * @return false 
 */
bool Worker_connectUser(Task* task);

/**
 * @brief 
 * 
 * @param command 
 * @return char* 
 */
bool Worker_sendMessage(Task* task);

/**
 * @brief 
 * 
 * @param command 
 * @return char* 
 */
bool Worker_sendFile(Task* task);

/**
 * @brief 
 * 
 * @param task 
 * @return true 
 * @return false 
 */
bool Worker_listOnlineUsers(Task* task);

/**
 * @brief 
 * 
 * @param task 
 * @return true 
 * @return false 
 */
bool Worker_sendGoodbye(Task* task);

/**
 * @brief 
 * 
 * @param clientFD 
 */
void Worker_handleUnrecognizedCommand(uint32_t clientFD);

#endif // __WORKER_H__