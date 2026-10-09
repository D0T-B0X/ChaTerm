#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

#include "helper/UI/AppMenu.h"

/* Link with -lm (sin). */

static struct termios orig_termios;

/* ---------------------------------------------------------
 * Frame buffer -- whole frame assembled here, one write() per
 * frame, so redraws are atomic (no tearing).
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

static void bufAppendChar(Buf *b, char c, int count) {
    if (count <= 0) return;
    if (b->len + (size_t)count + 1 > b->cap) {
        while (b->len + (size_t)count + 1 > b->cap) b->cap *= 2;
        char *grown = realloc(b->data, b->cap);
        if (!grown) return;
        b->data = grown;
    }
    memset(b->data + b->len, c, (size_t)count);
    b->len += (size_t)count;
    b->data[b->len] = '\0';
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

/* ---------------------------------------------------------
 * Palette -- fixed, dark, no hue-cycling. Tokyo-Night-ish.
 * --------------------------------------------------------- */

typedef struct { int r, g, b; } RGB;

static const RGB PANEL_BG = {26, 27, 38};     /* solid opaque backdrop  */
static const RGB BORDER   = {122, 162, 247};  /* blue                   */
static const RGB TITLE    = {224, 228, 245};  /* bright, bold           */
static const RGB SUBTEXT  = {150, 160, 200};  /* muted but legible      */
static const RGB HINT     = {104, 112, 148};  /* dimmer, still legible  */
static const RGB ACCENT   = {122, 162, 247};  /* selected: blue         */
static const RGB DANGER   = {247, 118, 142};  /* selected: quit (pink)  */

#define BOLD "\033[1m"
#define NOBOLD "\033[22m"

static void fg(Buf *b, RGB c) { bufAppend(b, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b); }
static void bgc(Buf *b, RGB c) { bufAppend(b, "\033[48;2;%d;%d;%dm", c.r, c.g, c.b); }

/* Reset attributes but re-assert the panel background, so no
 * cell ever falls back to the terminal's own (blurred) bg. */
static void panelReset(Buf *b) {
    bufAppend(b, "\033[0m");
    bgc(b, PANEL_BG);
}

/* Paints the entire screen with the opaque panel color. This is
 * what actually defeats compositor blur: blur only shows through
 * cells left at the terminal's default (transparent) background,
 * so every cell here gets an explicit, opaque background first. */
static void fillPanel(Buf *b, int rows, int cols) {
    bgc(b, PANEL_BG);
    char *row = malloc((size_t)cols + 1);
    if (row) { memset(row, ' ', (size_t)cols); row[cols] = '\0'; }
    for (int r = 1; r <= rows; r++) {
        bufAppend(b, "\033[%d;1H", r);
        if (row) bufAppend(b, "%s", row);
        else bufAppendChar(b, ' ', cols);
    }
    free(row);
}

/* ---------------------------------------------------------
 * 5x5 block glyphs for the banner text.
 * --------------------------------------------------------- */

static const char *glyphRow(char ch, int row) {
    static const char *T[5] = {"#####","..#..","..#..","..#..","..#.."};
    static const char *E[5] = {"#####","#....","####.","#....","#####"};
    static const char *R[5] = {"####.","#...#","####.","#..#.","#...#"};
    static const char *M[5] = {"#...#","##.##","#.#.#","#...#","#...#"};
    static const char *C[5] = {".####","#....","#....","#....",".####"};
    static const char *H[5] = {"#...#","#...#","#####","#...#","#...#"};
    static const char *A[5] = {".###.","#...#","#####","#...#","#...#"};
    static const char *SP[5] = {".....",".....",".....",".....","....."};
    switch (ch) {
        case 'T': return T[row]; case 'E': return E[row]; case 'R': return R[row];
        case 'M': return M[row]; case 'C': return C[row]; case 'H': return H[row];
        case 'A': return A[row]; default: return SP[row];
    }
}

static void drawBanner(Buf *b, int row, int cols, const char *text) {
    int n = (int)strlen(text);
    const int glyphW = 5, gap = 1;
    int totalWidth = n * glyphW + (n - 1) * gap;
    int startCol = 2 + (cols - 2 - totalWidth) / 2;
    if (startCol < 2) startCol = 2;

    for (int r = 0; r < 5; r++) {
        bufAppend(b, "\033[%d;%dH", row + r, startCol);
        fg(b, BORDER);
        bufAppend(b, BOLD);
        for (int i = 0; i < n; i++) {
            const char *pattern = glyphRow(text[i], r);
            for (int col = 0; col < glyphW; col++) {
                bufAppend(b, pattern[col] == '#' ? "\xe2\x96\x88" : " "); /* U+2588 */
            }
            if (i < n - 1) bufAppend(b, " ");
        }
    }
    panelReset(b);
}

/* ---------------------------------------------------------
 * Border -- heavy-weight straight edges, rounded corners.
 * Static bold color, no animation.
 * --------------------------------------------------------- */

static void drawBorders(Buf *b, int rows, int cols) {
    fg(b, BORDER);
    bufAppend(b, BOLD);

    bufAppend(b, "\033[1;1H\xe2\x95\xad"); /* ╭ */
    for (int i = 0; i < cols - 2; i++) bufAppend(b, "\xe2\x94\x81"); /* ━ */
    bufAppend(b, "\xe2\x95\xae"); /* ╮ */

    for (int r = 2; r < rows; r++) {
        bufAppend(b, "\033[%d;1H\xe2\x94\x83", r); /* ┃ */
        bufAppend(b, "\033[%d;%dH\xe2\x94\x83", r, cols);
    }

    bufAppend(b, "\033[%d;1H\xe2\x95\xb0", rows); /* ╰ */
    for (int i = 0; i < cols - 2; i++) bufAppend(b, "\xe2\x94\x81");
    bufAppend(b, "\xe2\x95\xaf"); /* ╯ */

    panelReset(b);
}

static void hDivider(Buf *b, int row, int cols) {
    fg(b, BORDER);
    bufAppend(b, BOLD "\033[%d;1H\xe2\x94\xa3", row); /* ┣ */
    for (int i = 0; i < cols - 2; i++) bufAppend(b, "\xe2\x94\x81");
    bufAppend(b, "\033[%d;%dH\xe2\x94\xab", row, cols); /* ┫ */
    panelReset(b);
}

static void printTextCenter(Buf *b, int row, int cols, const char *text, RGB color, int bold) {
    int innerWidth = cols - 2;
    int len = (int)strlen(text);
    int padLeft = (innerWidth - len) / 2;
    if (padLeft < 0) padLeft = 0;
    bufAppend(b, "\033[%d;2H", row);
    bufAppendChar(b, ' ', padLeft);
    fg(b, color);
    if (bold) bufAppend(b, BOLD);
    bufAppend(b, "%s", text);
    panelReset(b);
}

/* Selected item: solid filled pill, dark bold text on a bright
 * background -- highest-contrast element on screen. Unselected
 * items stay bold too (per request) just at lower saturation,
 * so they're never washed out under blur. */
static void printMenuItem(Buf *b, int row, const char *text, int isSelected,
                           int isDestructive, int cols, double pulse) {
    int innerWidth = cols - 2;
    int btnWidth = 24;
    int padLeft = (innerWidth - btnWidth) / 2;
    if (padLeft < 0) padLeft = 0;

    bufAppend(b, "\033[%d;2H", row);
    bufAppendChar(b, ' ', padLeft);

    int textLen = (int)strlen(text);

    if (isSelected) {
        RGB base = isDestructive ? DANGER : ACCENT;
        RGB fill = { (int)(base.r * pulse), (int)(base.g * pulse), (int)(base.b * pulse) };

        int bgWidth = btnWidth - 4;
        if (bgWidth < textLen) bgWidth = textLen + 2;
        int pL = (bgWidth - textLen) / 2;
        int pR = bgWidth - textLen - pL;

        fg(b, BORDER); bufAppend(b, BOLD "\xe2\x96\xb8 "); /* ▸ */
        bgc(b, fill);
        bufAppend(b, "\033[1m\033[38;2;18;18;26m"); /* bold, near-black text on bright fill */
        bufAppendChar(b, ' ', pL);
        bufAppend(b, "%s", text);
        bufAppendChar(b, ' ', pR);
        panelReset(b);
        fg(b, BORDER); bufAppend(b, BOLD " \xe2\x97\x82"); /* ◂ */
        panelReset(b);
    } else {
        int pL = (btnWidth - textLen) / 2;
        int pR = btnWidth - textLen - pL;
        if (pL < 0) pL = 0;
        if (pR < 0) pR = 0;
        bufAppendChar(b, ' ', pL + 2);
        fg(b, SUBTEXT);
        bufAppend(b, BOLD "%s", text);
        panelReset(b);
        bufAppendChar(b, ' ', pR + 2);
    }
}

/* ---------------------------------------------------------
 * Live system stats
 * --------------------------------------------------------- */

static void getSystemStats(char *out, size_t outsz) {
    double uptimeSec = 0, l1 = 0, l5 = 0, l15 = 0;
    int haveUptime = 0, haveLoad = 0;

    FILE *fu = fopen("/proc/uptime", "r");
    if (fu) { if (fscanf(fu, "%lf", &uptimeSec) == 1) haveUptime = 1; fclose(fu); }

    FILE *fl = fopen("/proc/loadavg", "r");
    if (fl) { if (fscanf(fl, "%lf %lf %lf", &l1, &l5, &l15) == 3) haveLoad = 1; fclose(fl); }

    if (haveUptime && haveLoad) {
        int h = (int)(uptimeSec / 3600);
        int m = ((int)uptimeSec % 3600) / 60;
        snprintf(out, outsz, "up %dh %dm   \xc2\xb7   load %.2f %.2f %.2f", h, m, l1, l5, l15);
    } else {
        snprintf(out, outsz, "system stats unavailable");
    }
}

/* ---------------------------------------------------------
 * Frame
 * --------------------------------------------------------- */

static void printMenu(Buf *b, int selected, int rows, int cols, double phase) {
    if (rows < 26 || cols < 60) {
        bufAppend(b, "\033[2J\033[H");
        fillPanel(b, rows, cols);
        fg(b, TITLE);
        bufAppend(b, "\033[1;1H" BOLD "Terminal window too small -- resize to continue.");
        panelReset(b);
        bufFlush(b);
        return;
    }

    bufAppend(b, "\033[2J");
    fillPanel(b, rows, cols);
    drawBorders(b, rows, cols);

    int blockHeight = 22;
    int y = (rows - blockHeight) / 2;
    if (y < 2) y = 2;

    drawBanner(b, y, cols, "CHATERM");
    y += 6;

    hDivider(b, y, cols);
    y += 1;

    printTextCenter(b, y + 1, cols, "select instance mode", SUBTEXT, 1);
    y += 3;

    double pulse = 0.88 + 0.12 * sin(phase * 2.2); /* subtle, single-hue only */

    printMenuItem(b, y, "SERVER", selected == 0, 0, cols, pulse);
    y += 2;
    printMenuItem(b, y, "CLIENT", selected == 1, 0, cols, pulse);
    y += 2;
    printMenuItem(b, y, "QUIT", selected == 2, 1, cols, pulse);
    y += 2;

    hDivider(b, y, cols);
    y += 1;

    char stats[96];
    getSystemStats(stats, sizeof(stats));
    printTextCenter(b, y + 1, cols, stats, HINT, 1);
    y += 2;

    printTextCenter(b, y + 1, cols, "\xe2\x86\x91 \xe2\x86\x93 move   \xc2\xb7   enter select   \xc2\xb7   esc quit", HINT, 1);

    bufFlush(b);
}

/* ---------------------------------------------------------
 * Input loop
 * --------------------------------------------------------- */

AppMode getInitialMode(void) {
    int selected = 0;
    const int num_options = 3;
    struct winsize w;
    Buf frame;

    bufInit(&frame);
    enableRawMode();

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double phase = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;

        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        printMenu(&frame, selected, w.ws_row, w.ws_col, phase);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000}; /* ~20fps while idle */

        int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &fds)) continue;

        int c = getchar();

        if (c == EOF) {
            disableRawMode(); bufFree(&frame);
            return MODE_EXIT;
        } else if (c == '\033') {
            int c2 = getchar();
            if (c2 == '[') {
                switch (getchar()) {
                    case 'A': if (selected > 0) selected--; break;
                    case 'B': if (selected < num_options - 1) selected++; break;
                }
            } else {
                disableRawMode(); bufFree(&frame);
                return MODE_EXIT;
            }
        } else if (c == '\n' || c == '\r') {
            disableRawMode(); bufFree(&frame);
            if (selected == 0) return MODE_SERVER;
            if (selected == 1) return MODE_CLIENT;
            return MODE_EXIT;
        }
    }
}