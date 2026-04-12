#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

static const char *get_level_color(const char *level) {
    if (level[0] == 'I') return COLOR_CYAN;
    if (level[0] == 'D') return COLOR_YELLOW;
    if (level[0] == 'T') return COLOR_RESET;
    return COLOR_RESET;
}

void log_message(const char *level, const char *file, int line, const char *msg, ...) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm *tm_info = localtime(&tv.tv_sec);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const char *filename = file;
    for (const char *p = file; *p; p++) {
        if (*p == '/') filename = p + 1;
    }

    const char *color = get_level_color(level);

    va_list args;
    va_start(args, msg);

    printf(COLOR_BOLD "%s.%06ld " COLOR_RESET "[" COLOR_BOLD "%s%s%s" COLOR_RESET "] ",
           timestamp, tv.tv_usec, color, level, COLOR_RESET);
    vprintf(msg, args);
    printf(" [" COLOR_BOLD "%s:%d" COLOR_RESET "]\n" COLOR_RESET, filename, line);
    va_end(args);
}