#ifndef GANON_LOGGING_H
#define GANON_LOGGING_H

#include <stddef.h>

typedef enum {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
} log_level_t;

extern log_level_t g_log_level;

void log_message(const char *level, const char *file, int line, const char *msg, ...);

#ifdef __DEBUG__
#define LOG_INFO(msg, ...)  do { if (LOG_LEVEL_INFO >= g_log_level) log_message("INFO", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_DEBUG(msg, ...) do { if (LOG_LEVEL_DEBUG >= g_log_level) log_message("DEBUG", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_TRACE(msg, ...) do { if (LOG_LEVEL_TRACE >= g_log_level) log_message("TRACE", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_WARNING(msg, ...) log_message("WARN", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) log_message("WARN", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) log_message("ERROR", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#else
#define LOG_INFO(msg, ...)  ((void)0)
#define LOG_DEBUG(msg, ...)  ((void)0)
#define LOG_TRACE(msg, ...) ((void)0)
#define LOG_WARNING(msg, ...) ((void)0)
#define LOG_WARN(msg, ...) ((void)0)
#define LOG_ERROR(msg, ...) ((void)0)
#endif /* #ifdef __DEBUG__ */

#endif /* #ifndef GANON_LOGGING_H */