#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

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

    va_list args;
    va_start(args, msg);

    printf("%s.%06ld [%s] ", timestamp, tv.tv_usec, level);
    vprintf(msg, args);
    printf(" [%s:%d]\n", filename, line);
    va_end(args);
}