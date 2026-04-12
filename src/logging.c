#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_BOLD    "\033[1m"

static const char *get_level_color(const char *level) {
    if (level[0] == 'I') {
        return COLOR_CYAN;
    }
    if (level[0] == 'D') {
        return COLOR_WHITE;
    }
    if (level[0] == 'T') {
        return COLOR_RESET;
    }
    if (level[0] == 'E') {
        return COLOR_RED;
    }
    if (level[0] == 'W') {
        return COLOR_YELLOW;
    }
    return COLOR_RESET;
}

static const char *get_level_pad(const char *level) {
    if (level[0] == 'I') {
        return " ";
    }
    return "";
}

void log_message(const char *level, const char *file, int line, const char *msg, ...) {
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);

    struct tm *tm_info = localtime(&tv.tv_sec);
    char timestamp[32];
    (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const char *filename = file;
    for (const char *p = file; *p != '\0'; p++) {
        if (*p == '/') {
            filename = p + 1;
        }
    }

    const char *color = get_level_color(level);
    const char *pad = get_level_pad(level);

    va_list args;
    va_start(args, msg);

    printf(COLOR_BOLD);
    printf("%s.%06ld ", timestamp, tv.tv_usec);
    printf(COLOR_RESET "[");
    printf(COLOR_BOLD "%s", color);
    printf("%s%s", level, pad);
    printf(COLOR_RESET "] ");
    vprintf(msg, args);
    printf(" [");
    printf(COLOR_BOLD "%s:%d", filename, line);
    printf(COLOR_RESET "]\n");
    va_end(args);
}