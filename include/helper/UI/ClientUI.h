#ifndef __CLIENT_UI_H__
#define __CLIENT_UI_H__

#include <stddef.h>
#include <sys/types.h> // For ssize_t

// --- UI Color Palette ---
// System messages (bright yellow)
#define C_SYS  "\x1b[93m"
// Local user input indicator (cyan)
#define C_YOU  "\x1b[96m"
// Error messages (bright red)
#define C_ERR  "\x1b[91m"
// Success messages (bright green) — added for server responses
#define C_GRN  "\x1b[92m"
// General red (same as C_ERR but kept for clarity)
#define C_RED  "\x1b[91m"
// Incoming messages (cyan, same as C_YOU but distinct purpose)
#define C_CYN  "\x1b[96m"
// CRITICAL: Resets text color AND restores the custom UI background!
#define C_RST  "\x1b[0m\x1b[48;2;26;27;38m"

// Opaque struct representing the terminal chat state
typedef struct ClientTerm ClientTerm;

// Setup & Teardown
void initClientUI(void);
void endClientUI(void);

// Blocking Setup Screens (Safe to run before epoll starts)
int  ClientUI_PromptConnect(char *hostOut, size_t hostSz, char *portOut, size_t portSz);
void ClientUI_ShowConnecting(const char *host, const char *port);
void ClientUI_ShowMessage(const char *message, const char *hint, int isError);
void ClientUI_ShowHelp(ClientTerm* term);
int  ClientUI_WaitKey(void);

// Async Chat Terminal Functions (Designed for the epoll loop)
ClientTerm* ClientUI_TermCreate(const char *host, const char *port);
void        ClientUI_TermDestroy(ClientTerm *term);
void        ClientUI_TermAppendLine(ClientTerm *term, const char *line);
void        ClientUI_TermSetConnected(ClientTerm *term, int connected);
void        ClientUI_TermSetPopup(ClientTerm *term, const char *message, int isError);
void        ClientUI_TermDraw(ClientTerm *term);

/**
 * @brief Parses a buffer of bytes read from STDIN via epoll.
 * @return 1 if a message was submitted (Enter), -1 if user pressed ESC, 0 otherwise.
 */
int         ClientUI_TermHandleInput(ClientTerm *term, const char *seq, ssize_t len);
const char* ClientUI_TermTakeSubmittedLine(ClientTerm *term);

#endif // __CLIENT_UI_H__