#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>

#include "helper/UI/ServerUI.h"

// Helper to convert color names to ANSI escape codes
static const char*
colorToAnsi(const char* color) {
    if (strcmp(color, "BLACK")   == 0) return "\033[30m";
    if (strcmp(color, "RED")     == 0) return "\033[31m";
    if (strcmp(color, "GREEN")   == 0) return "\033[32m";
    if (strcmp(color, "YELLOW")  == 0) return "\033[33m";
    if (strcmp(color, "BLUE")    == 0) return "\033[34m";
    if (strcmp(color, "MAGENTA") == 0) return "\033[35m";
    if (strcmp(color, "CYAN")    == 0) return "\033[36m";
    if (strcmp(color, "WHITE")   == 0) return "\033[37m";
    if (strcmp(color, "GRAY")    == 0) return "\033[90m";
    // Default to white (or bright white)
    return "\033[37m";
}

void initServerUI(const char* ipAddress, uint16_t port) {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int rows = w.ws_row;
    int cols = w.ws_col;

    // Clear screen and hide cursor
    printf("\033[2J\033[?25l"); 

    // ==========================================
    // 1. DRAW HEADER (Row 1)
    // ==========================================
    printf("\033[1;1H");     // Move cursor to Row 1, Col 1
    printf("\033[45;1;37m"); // Magenta Background, Bold White Text
    
    char header[] = " ChaTerm Server Active ";
    int padLeft = (cols - strlen(header)) / 2;
    int padRight = cols - padLeft - strlen(header);
    
    for(int i = 0; i < padLeft; i++) putchar(' ');
    printf("%s", header);
    for(int i = 0; i < padRight; i++) putchar(' ');
    printf("\033[0m");       // Reset formatting

    // ==========================================
    // 2. DRAW FOOTER (Last Row)
    // ==========================================
    printf("\033[%d;1H", rows); // Move cursor to bottom row
    printf("\033[44;1;37m");    // Blue Background, Bold White Text
    
    char footer[128];
    snprintf(footer, sizeof(footer), " BIND IP: %s | PORT: %d ", ipAddress, port);
    
    printf("%s", footer);
    for(size_t i = strlen(footer); i < cols; i++) putchar(' ');
    printf("\033[0m");

    // ==========================================
    // 3. SET SCROLLING REGION
    // ==========================================
    // \033[<top>;<bottom>r restricts scrolling to a specific block of rows
    printf("\033[2;%dr", rows - 1); 

    // Move cursor to the top of the new scrolling region (Row 2, Col 1)
    printf("\033[2;1H");
    
    fflush(stdout);
}

void serverLog(const char* color, const char* event, const char* message) {
    // Get current time
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // Print timestamp in gray, event in specified color, and the message
    // A newline here will automatically push older logs up without touching the footer
    printf("\033[90m[%02d:%02d:%02d]\033[0m %s[%s]\033[0m %s\n", 
        t->tm_hour, t->tm_min, t->tm_sec, 
        colorToAnsi(color), event, message);
    
    fflush(stdout);
}

void cleanupServerUI(void) {
    // \033[r resets the scrolling region to the entire terminal
    printf("\033[r");      
    // Clear screen and show cursor again
    printf("\033[2J\033[H\033[?25h"); 
    fflush(stdout);
}