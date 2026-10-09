#ifndef __SERVER_UI_H__
#define __SERVER_UI_H__

#include <stdint.h>

// Color definitions for your logs
#define LOG_INFO    "\033[1;36m" // Cyan
#define LOG_SUCCESS "\033[1;32m" // Green
#define LOG_WARN    "\033[1;33m" // Yellow
#define LOG_ERROR   "\033[1;31m" // Red

/**
 * @brief Initializes the server terminal interface, drawing the header, 
 *        footer, and setting the scrolling margins.
 */
void initServerUI(const char* ipAddress, uint16_t port);

/**
 * @brief Prints a formatted log message into the scrolling area.
 */
void serverLog(const char* color, const char* event, const char* message);

/**
 * @brief Resets the terminal margins and clears the screen before exiting.
 */
void cleanupServerUI(void);

#endif // __SERVER_UI_H__