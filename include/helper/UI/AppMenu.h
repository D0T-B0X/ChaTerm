#ifndef __APP_MENU_H__
#define __APP_MENU_H__

typedef enum {
    MODE_SERVER,
    MODE_CLIENT,
    MODE_EXIT
} AppMode;

/**
 * @brief Renders the TUI menu and captures user selection.
 * 
 * @return AppMode The selected instance mode.
 */
AppMode getInitialMode(void);

#endif // __APP_MENU_H__