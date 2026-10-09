#ifndef __CLIENT_TABLE_H__
#define __CLIENT_TABLE_H__

#include "Common.h"

// Error codes (codes are outside the maximum usage range)
#define TABLE_FULL 10001 
#define NO_CONNECT 10002
#define USER_NOT_FOUND 10003
#define NO_SHRINK 10004

/**
 * @brief 
 * 
 */
void clientTableInit();

/**
 * @brief 
 * 
 * @param user 
 * @return int8_t 
 */
uint16_t connectUserInClientTable(UserData user);

/**
 * @brief 
 * 
 * @param username 
 * @return int8_t 
 */
uint16_t disconnectUserFromClientTable(uint32_t userFD);

/**
 * @brief Get the List Of Online Users object
 * 
 * @param users 
 */
void getListOfOnlineUsers(char* users, size_t usersSize);

/**
 * @brief Get the User By F D object
 * 
 * @param FD 
 * @return UserData* 
 */
UserData* getUserByFD(uint32_t FD);

/**
 * @brief Get the User By Username object
 * 
 * @param username 
 * @return UserData* 
 */
UserData* getUserByUsername(const char* username);

// ---------- SYNCHRONIZATION ----------

/**
 * @brief 
 * 
 */
void lockClientTable();

/**
 * @brief 
 * 
 */
void unlockClientTable();

#endif // __CLIENT_TABLE_H__