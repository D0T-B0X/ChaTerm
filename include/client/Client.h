#ifndef __CLIENT_H__
#define __CLIENT_H__

// Error codes for command parsing
#define INVALID_COMMAND 0
#define VALID_COMMAND 1
#define EMPTY_INPUT 2
#define NO_KEY 3
#define INVALID_FORMAT 4

/**
 * @brief 
 * 
 */
void enableClient();

/**
 * @brief 
 * 
 */
void clientLoop();

/**
 * @brief 
 * 
 */
void destroyClient();

#endif // __CLIENT_H__