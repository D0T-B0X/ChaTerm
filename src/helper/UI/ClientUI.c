#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "helper/UI/ClientUI.h"

static struct termios orig_termios;

/* ---------------------------------------------------------
 * Frame buffer
 * --------------------------------------------------------- */
typedef struct { char *data; size_t len; size_t cap; } Buf;

static void bufInit(Buf *b) {
    b->cap = 8192; b->len = 0;
    b->data = malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}
static void bufFree(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void bufAppend(Buf *b, const char *fmt, ...) {
    va_list args, cp;
    va_start(args, fmt);
    va_copy(cp, args);
    int needed = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (needed < 0) { va_end(args); return; }

    if (b->len + (size_t)needed + 1 > b->cap) {
        while (b->len + (size_t)needed + 1 > b->cap) b->cap *= 2;
        char *grown = realloc(b->data, b->cap);
        if (!grown) { va_end(args); return; }
        b->data = grown;
    }
    vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, args);
    b->len += (size_t)needed;
    va_end(args);
}

static void bufFlush(Buf *b) {
    if (b->len) write(STDOUT_FILENO, b->data, b->len);
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

/* ---------------------------------------------------------
 * Terminal mode
 * --------------------------------------------------------- */
static void disableRawMode(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    printf("\033[?1049l\033[?25h");
    fflush(stdout);
}

static void enableRawMode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[?1049h\033[?25l");
    fflush(stdout);
}

void initClientUI(void) { enableRawMode(); }
void endClientUI(void) { disableRawMode(); }

/* ---------------------------------------------------------
 * Palette 
 * --------------------------------------------------------- */
typedef struct { int r, g, b; } RGB;
static const RGB PANEL_BG = {26, 27, 38};
static const RGB BORDER   = {122, 162, 247};
static const RGB TITLE    = {224, 228, 245};
static const RGB SUBTEXT  = {150, 160, 200};
static const RGB HINT     = {104, 112, 148};
static const RGB ACCENT   = {122, 162, 247};
static const RGB DANGER   = {247, 118, 142};
#define BOLD "\033[1m"

static void fg(Buf *b, RGB c) { bufAppend(b, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b); }
static void bgc(Buf *b, RGB c) { bufAppend(b, "\033[48;2;%d;%d;%dm", c.r, c.g, c.b); }
static void panelReset(Buf *b) { bufAppend(b, "\033[0m"); bgc(b, PANEL_BG); }

static void fillPanel(Buf *b, int rows, int cols) {
    bgc(b, PANEL_BG);
    char *row = malloc((size_t)cols + 1);
    if (row) { memset(row, ' ', (size_t)cols); row[cols] = '\0'; }
    for (int r = 1; r <= rows; r++) {
        bufAppend(b, "\033[%d;1H", r);
        if (row) bufAppend(b, "%s", row);
    }
    free(row);
}

static void drawBorders(Buf *b, int rows, int cols) {
    fg(b, BORDER);
    bufAppend(b, BOLD);
    bufAppend(b, "\033[1;1H\xe2\x95\xad");
    for (int i = 0; i < cols - 2; i++) bufAppend(b, "\xe2\x94\x81");
    bufAppend(b, "\xe2\x95\xae");
    for (int r = 2; r < rows; r++) {
        bufAppend(b, "\033[%d;1H\xe2\x94\x83", r);
        bufAppend(b, "\033[%d;%dH\xe2\x94\x83", r, cols);
    }
    bufAppend(b, "\033[%d;1H\xe2\x95\xb0", rows);
    for (int i = 0; i < cols - 2; i++) bufAppend(b, "\xe2\x94\x81");
    bufAppend(b, "\xe2\x95\xaf");
    panelReset(b);
}

static void hDivider(Buf *b, int row, int cols) {
    fg(b, BORDER);
    bufAppend(b, BOLD "\033[%d;1H\xe2\x94\xa3", row);
    for (int i = 0; i < cols - 2; i++) bufAppend(b, "\xe2\x94\x81");
    bufAppend(b, "\033[%d;%dH\xe2\x94\xab", row, cols);
    panelReset(b);
}

// Centered text within a specific horizontal range (used for popups)
static void printTextInBox(Buf *b, int row, int startCol, int width, const char *text, RGB color, int bold) {
    int len = (int)strlen(text);
    int padLeft = (width - len) / 2;
    if (padLeft < 0) padLeft = 0;
    bufAppend(b, "\033[%d;%dH", row, startCol + padLeft);
    fg(b, color);
    if (bold) bufAppend(b, BOLD);
    bufAppend(b, "%s", text);
    panelReset(b);
}

static void printTextCenter(Buf *b, int row, int cols, const char *text, RGB color, int bold) {
    printTextInBox(b, row, 2, cols - 2, text, color, bold);
}

static void currentSize(int *rows, int *cols) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    *rows = w.ws_row;
    *cols = w.ws_col;
}

/* ---------------------------------------------------------
 * Connect form (Blocking allowed during setup)
 * --------------------------------------------------------- */
#define HOST_MAXLEN 128
#define PORT_MAXLEN 6

#define T_CMD  "\x1b[93m"
#define T_ARG  "\x1b[95m"
#define T_DESC "\x1b[97m"
#define T_STRC "\x1b[96m"
#define T_RST  "\x1b[0m\x1b[48;2;26;27;38m"

static void drawConnectForm(Buf *b, int rows, int cols, const char *host, const char *port, int focus, const char *errMsg) {
    bufAppend(b, "\033[2J");
    fillPanel(b, rows, cols);
    drawBorders(b, rows, cols);

    int y = (rows - 12) / 2;
    if (y < 2) y = 2;

    printTextCenter(b, y, cols, "C H A T E R M", TITLE, 1);
    y += 1;
    printTextCenter(b, y, cols, "connect to a server", SUBTEXT, 1);
    y += 2;

    char hostLine[200];
    snprintf(hostLine, sizeof hostLine, "%s server ip / hostname:  %s%s",
             focus == 0 ? "\xe2\x96\xb8" : " ", host, focus == 0 ? "\xe2\x96\x8f" : "");
    printTextCenter(b, y, cols, hostLine, focus == 0 ? TITLE : SUBTEXT, 1);
    y += 2;

    char portLine[64];
    snprintf(portLine, sizeof portLine, "%s port:  %s%s",
             focus == 1 ? "\xe2\x96\xb8" : " ", port, focus == 1 ? "\xe2\x96\x8f" : "");
    printTextCenter(b, y, cols, portLine, focus == 1 ? TITLE : SUBTEXT, 1);
    y += 2;

    if (errMsg && errMsg[0]) {
        printTextCenter(b, y, cols, errMsg, DANGER, 1);
        y += 2;
    }

    printTextCenter(b, y + 1, cols, "tab switch field  \xc2\xb7  enter connect  \xc2\xb7  esc quit", HINT, 1);
    bufFlush(b);
}

int ClientUI_PromptConnect(char *hostOut, size_t hostSz, char *portOut, size_t portSz) {
    char host[HOST_MAXLEN] = "172.16.0.2";
    char port[PORT_MAXLEN] = "5713";
    int hostLen = (int)strlen(host);
    int portLen = (int)strlen(port);
    int focus = 0;
    char err[64] = "";

    Buf frame;
    bufInit(&frame);

    while (1) {
        int rows, cols;
        currentSize(&rows, &cols);
        drawConnectForm(&frame, rows, cols, host, port, focus, err);
        err[0] = '\0';

        // Use read() to avoid blocking on escape sequences
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) { bufFree(&frame); return 0; }

        if (c == '\t') {
            focus = 1 - focus;
        } else if (c == 27) { // ESC
            // Check if there are more bytes (escape sequence)
            unsigned char next[2];
            ssize_t next_n = read(STDIN_FILENO, next, 2);
            if (next_n == 0) {
                // ESC alone -> quit
                bufFree(&frame);
                return 0;
            } else if (next_n == 1) {
                // Single extra char (unlikely) - treat as ESC alone
                bufFree(&frame);
                return 0;
            } else {
                // Check for arrow keys: \x1b[A or \x1b[B
                if (next[0] == '[' && (next[1] == 'A' || next[1] == 'B')) {
                    focus = 1 - focus;
                }
                // Any other escape sequence: ignore
            }
        } else if (c == '\n' || c == '\r') {
            if (hostLen == 0) { snprintf(err, sizeof err, "enter a server address"); continue; }
            if (portLen == 0) { snprintf(err, sizeof err, "enter a port"); continue; }
            int portNum = atoi(port);
            if (portNum < 1 || portNum > 65535) { snprintf(err, sizeof err, "port must be 1-65535"); continue; }

            strncpy(hostOut, host, hostSz - 1); hostOut[hostSz - 1] = '\0';
            strncpy(portOut, port, portSz - 1); portOut[portSz - 1] = '\0';
            bufFree(&frame);
            return 1;
        } else if (c == 127 || c == 8) {
            if (focus == 0 && hostLen > 0) host[--hostLen] = '\0';
            if (focus == 1 && portLen > 0) port[--portLen] = '\0';
        } else if (c >= 32 && c < 127) {
            if (focus == 0 && hostLen < HOST_MAXLEN - 1) {
                host[hostLen++] = (char)c; host[hostLen] = '\0';
            }
            if (focus == 1 && portLen < PORT_MAXLEN - 1 && c >= '0' && c <= '9') {
                port[portLen++] = (char)c; port[portLen] = '\0';
            }
        }
    }
}

void 
ClientUI_ShowHelp(ClientTerm* term) {
    if (!term) return;

    ClientUI_TermAppendLine(term, T_STRC "+------------------------- COMMAND MENU -------------------------+" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "|" T_CMD " REGISTER     " T_ARG "<user> <pass> KEY <key>  " T_DESC ": Create a new account   " T_STRC "|" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "|" T_CMD " CONNECT      " T_ARG "<user> <pass>            " T_DESC ": Login to your account  " T_STRC "|" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "|" T_CMD " SEND TO      " T_ARG "<user> <message>         " T_DESC ": Send a direct message  " T_STRC "|" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "|" T_CMD " SENDFILE TO  " T_ARG "<user> <filename>        " T_DESC ": Send a file to a user  " T_STRC "|" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "|" T_CMD " LIST         " T_ARG "                         " T_DESC ": Show online users      " T_STRC "|" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "|" T_CMD " QUIT         " T_ARG "                         " T_DESC ": Disconnect and exit    " T_STRC "|" T_RST);
    ClientUI_TermAppendLine(term, T_STRC "+----------------------------------------------------------------+" T_RST);
}

void ClientUI_ShowConnecting(const char *host, const char *port) {
    static const char *spin[] = {".  ", ".. ", "...", " ..", "  .", "   "};
    static int frame = 0;
    int rows, cols;
    currentSize(&rows, &cols);

    Buf b;
    bufInit(&b);
    bufAppend(&b, "\033[2J");
    fillPanel(&b, rows, cols);
    drawBorders(&b, rows, cols);

    char msg[160];
    snprintf(msg, sizeof msg, "connecting to %s:%s%s", host, port, spin[frame % 6]);
    printTextCenter(&b, rows / 2, cols, msg, TITLE, 1);
    printTextCenter(&b, rows / 2 + 2, cols, "this can take a few seconds", HINT, 1);

    bufFlush(&b);
    bufFree(&b);
    frame++;
}

void ClientUI_ShowMessage(const char *message, const char *hint, int isError) {
    int rows, cols;
    currentSize(&rows, &cols);

    Buf b;
    bufInit(&b);
    bufAppend(&b, "\033[2J");
    fillPanel(&b, rows, cols);
    drawBorders(&b, rows, cols);

    printTextCenter(&b, rows / 2, cols, message, isError ? DANGER : TITLE, 1);
    if (hint && hint[0]) printTextCenter(&b, rows / 2 + 2, cols, hint, HINT, 1);

    bufFlush(&b);
    bufFree(&b);
}

int ClientUI_WaitKey(void) { return getchar(); }

/* ---------------------------------------------------------
 * Chat terminal screen
 * --------------------------------------------------------- */
#define MAX_HISTORY 400
#define INPUT_MAXLEN 256

typedef struct { char **lines; int count; int cap; } History;

static char *dupLine(const char *s) {
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

static void historyInit(History *h) {
    h->cap = 64; h->count = 0;
    h->lines = malloc(sizeof(char *) * h->cap);
}

static void historyPush(History *h, const char *line) {
    if (h->count == h->cap) {
        h->cap *= 2;
        h->lines = realloc(h->lines, sizeof(char *) * h->cap);
    }
    h->lines[h->count++] = dupLine(line);
    if (h->count > MAX_HISTORY) {
        free(h->lines[0]);
        memmove(h->lines, h->lines + 1, sizeof(char *) * (h->count - 1));
        h->count--;
    }
}

static void historyFree(History *h) {
    for (int i = 0; i < h->count; i++) free(h->lines[i]);
    free(h->lines);
    h->lines = NULL; h->count = h->cap = 0;
}

struct ClientTerm {
    char host[HOST_MAXLEN];
    char port[PORT_MAXLEN];
    History hist;
    char input[INPUT_MAXLEN];
    int inputLen;
    char submitted[INPUT_MAXLEN];
    int connected;
    
    char popupMsg[128];
    int popupError;
    int hasPopup;
};

ClientTerm *ClientUI_TermCreate(const char *host, const char *port) {
    ClientTerm *t = malloc(sizeof(ClientTerm));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    strncpy(t->host, host, sizeof t->host - 1);
    strncpy(t->port, port, sizeof t->port - 1);
    historyInit(&t->hist);
    t->connected = 1;
    return t;
}

void ClientUI_TermDestroy(ClientTerm *term) {
    if (!term) return;
    historyFree(&term->hist);
    free(term);
}

void ClientUI_TermAppendLine(ClientTerm *term, const char *line) {
    if (!term || !line) return;
    historyPush(&term->hist, line);
}

void ClientUI_TermSetConnected(ClientTerm *term, int connected) {
    if (!term) return;
    term->connected = connected;
}

void ClientUI_TermSetPopup(ClientTerm *term, const char *message, int isError) {
    if (!term || !message) return;
    strncpy(term->popupMsg, message, sizeof(term->popupMsg) - 1);
    term->popupMsg[sizeof(term->popupMsg) - 1] = '\0';
    term->popupError = isError;
    term->hasPopup = 1;
}

void ClientUI_TermDraw(ClientTerm *term) {
    if (!term) return;

    int rows, cols;
    currentSize(&rows, &cols);
    Buf b;
    bufInit(&b);

    if (rows < 20 || cols < 72) {
        bufAppend(&b, "\033[2J\033[H");
        fillPanel(&b, rows, cols);
        fg(&b, TITLE);
        bufAppend(&b, "\033[1;1H" BOLD "Terminal window too small -- resize to width 72+ to continue.");
        panelReset(&b);
        bufFlush(&b);
        bufFree(&b);
        return;
    }

    bufAppend(&b, "\033[2J");
    fillPanel(&b, rows, cols);
    drawBorders(&b, rows, cols);

    int y = 2;
    char header[160];
    snprintf(header, sizeof header, "CHATERM  \xe2\x80\x94  %s:%s", term->host, term->port);
    printTextCenter(&b, y, cols, header, TITLE, 1);
    y += 1;

    hDivider(&b, y, cols);
    y += 1;

    int msgTop = y;
    int msgBottom = rows - 5;
    if (msgBottom < msgTop) msgBottom = msgTop;
    int visibleLines = msgBottom - msgTop + 1;

    int startIdx = term->hist.count - visibleLines;
    if (startIdx < 0) startIdx = 0;

    for (int i = 0; i < visibleLines; i++) {
        int idx = startIdx + i;
        if (idx >= term->hist.count) break;
        int row = msgTop + i;
        
        char lineBuf[520];
        strncpy(lineBuf, term->hist.lines[idx], sizeof lineBuf - 1);
        lineBuf[sizeof lineBuf - 1] = '\0';
        
        int maxW = cols - 4;
        if (maxW > 0) {
            int visCount = 0;
            int inAnsi = 0;
            int truncateIdx = -1;
            
            // Calculate true visible length, ignoring ANSI escape sequences
            for (int k = 0; lineBuf[k] != '\0'; k++) {
                if (lineBuf[k] == '\033') {
                    inAnsi = 1;
                } else if (inAnsi) {
                    if (lineBuf[k] >= 0x40 && lineBuf[k] <= 0x7E) inAnsi = 0;
                } else {
                    visCount++;
                    if (visCount > maxW) {
                        truncateIdx = k;
                        break;
                    }
                }
            }
            
            if (truncateIdx != -1) {
                lineBuf[truncateIdx] = '\0';
                // Ensure any pending ANSI sequence is closed
                strncat(lineBuf, "\033[0m\033[48;2;26;27;38m", sizeof(lineBuf) - strlen(lineBuf) - 1);
            }
        }
        
        bufAppend(&b, "\033[%d;3H", row);
        fg(&b, SUBTEXT);
        bufAppend(&b, BOLD "%s", lineBuf);
        panelReset(&b);
    }

    y = msgBottom + 1;
    hDivider(&b, y, cols);
    y += 1;

    const char *statusText = term->connected ? "\xe2\x97\x8f connected" : "\xe2\x97\x8b disconnected";
    printTextCenter(&b, y, cols, statusText, term->connected ? ACCENT : DANGER, 1);
    y += 1;

    fg(&b, ACCENT);
    bufAppend(&b, BOLD "\033[%d;3H> ", y);
    fg(&b, TITLE);
    bufAppend(&b, BOLD "%s\xe2\x96\x8f", term->input);
    panelReset(&b);
    y += 1;

    printTextCenter(&b, y, cols, "enter send  \xc2\xb7  esc quit", HINT, 1);

    // ---------------------------------------------------------
    // OVERLAY: Modal Popup Message
    // ---------------------------------------------------------
    if (term->hasPopup) {
        int pW = 54;
        if (pW > cols - 4) pW = cols - 4; 
        int pH = 6;
        int pX = (cols - pW) / 2 + 1;
        int pY = (rows - pH) / 2;
        
        // Draw popup background and border
        for (int r = 0; r < pH; r++) {
            bufAppend(&b, "\033[%d;%dH", pY + r, pX);
            bgc(&b, PANEL_BG);
            for (int c = 0; c < pW; c++) bufAppend(&b, " ");
        }
        
        fg(&b, term->popupError ? DANGER : ACCENT);
        bufAppend(&b, "\033[%d;%dH\xe2\x95\xad", pY, pX);
        for (int i = 0; i < pW - 2; i++) bufAppend(&b, "\xe2\x94\x81");
        bufAppend(&b, "\xe2\x95\xae");
        
        for (int r = 1; r < pH - 1; r++) {
            bufAppend(&b, "\033[%d;%dH\xe2\x94\x83", pY + r, pX);
            bufAppend(&b, "\033[%d;%dH\xe2\x94\x83", pY + r, pX + pW - 1);
        }
        
        bufAppend(&b, "\033[%d;%dH\xe2\x95\xb0", pY + pH - 1, pX);
        for (int i = 0; i < pW - 2; i++) bufAppend(&b, "\xe2\x94\x81");
        bufAppend(&b, "\xe2\x95\xaf");
        
        // Centered text inside the popup
        printTextInBox(&b, pY + 2, pX + 1, pW - 2, term->popupMsg, term->popupError ? DANGER : TITLE, 1);
        printTextInBox(&b, pY + 4, pX + 1, pW - 2, "press any key to dismiss", HINT, 0);
        panelReset(&b);
    }

    bufFlush(&b);
    bufFree(&b);
}

int ClientUI_TermHandleInput(ClientTerm *term, const char *seq, ssize_t len) {
    if (!term || len <= 0) return 0;

    if (term->hasPopup) {
        term->hasPopup = 0;
        return 0; 
    }

    for (ssize_t i = 0; i < len; i++) {
        char c = seq[i];

        if (c == 27) { // ESC
            // If there is not enough data for an escape sequence, treat as quit
            if (i + 1 >= len) return -1;
            
            // Check for common escape sequences (ignore them)
            if (seq[i+1] == '[') {
                // Skip until final byte (0x40-0x7E)
                ssize_t j = i + 2;
                while (j < len && !(seq[j] >= 0x40 && seq[j] <= 0x7E)) j++;
                if (j < len) {
                    i = j; // skip entire sequence
                    continue;
                } else {
                    // Incomplete sequence, ignore remaining bytes
                    break;
                }
            } else {
                // Unknown escape, treat as quit
                return -1;
            }
        }

        if (c == '\n' || c == '\r') {
            if (term->inputLen == 0) continue;
            
            term->input[term->inputLen] = '\0';
            strncpy(term->submitted, term->input, sizeof term->submitted - 1);
            term->submitted[sizeof term->submitted - 1] = '\0';
            
            term->inputLen = 0;
            term->input[0] = '\0';
            return 1;
        }

        if (c == 127 || c == 8) {
            if (term->inputLen > 0) term->input[--term->inputLen] = '\0';
            continue;
        }

        if (c >= 32 && c < 127) {
            if (term->inputLen < INPUT_MAXLEN - 1) {
                term->input[term->inputLen++] = c;
                term->input[term->inputLen] = '\0';
            }
        }
    }
    
    return 0; 
}

const char *ClientUI_TermTakeSubmittedLine(ClientTerm *term) {
    if (!term) return "";
    return term->submitted;
}