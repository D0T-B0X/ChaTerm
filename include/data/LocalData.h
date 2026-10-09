#ifndef __LOCAL_DATA_H__
#define __LOCAL_DATA_H__

#include "Common.h"

// Return codes
#define FILE_NO_OPEN 1
#define FILE_WRITE_ERROR 2
#define FILE_WRITE_SUCCESS 3
#define FILE_IN_USE 4
#define USER_VALID 5
#define USER_INVALID 6
#define USER_ALREADY_EXISTS 7
#define PASS_INVALID 8

/**
 * @brief Get the Local Data object
 * 
 */
void getLocalData();

/**
 * @brief 
 * 
 * @param username 
 * @return true 
 * @return false 
 */
bool isUserRegistered(const char* username);

/**
 * @brief 
 * 
 * @param user 
 * @return uint8_t 
 */
uint8_t verifyUser(const char* username, const char* password);

/**
 * @brief 
 * 
 * @param user 
 * @return uint8_t 
 */
uint8_t saveNewUser(UserData* user);

/**
 * @brief Get the User Salt object
 * 
 * @param username 
 * @return char* 
 */
uchar* getUserSalt(const char* username, uchar* salt);

/**
 * @brief Get the User Key object
 * 
 * @param username 
 * @param key 
 * @return uchar* 
 */
uchar* getUserKey(const char* username, uchar* key);

#endif // __LOCAL_DATA_H__