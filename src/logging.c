#include <stdio.h>
#include <stdarg.h>
#include <time.h>

void log_message(const char *level, const char *file, int line, const char *msg, ...) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    va_list args;
    va_start(args, msg);

    printf("%s [%s] ", timestamp, level);
    vprintf(msg, args);
    printf(" [%s:%d]\n", file, line);
    va_end(args);
}