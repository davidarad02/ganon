#ifndef GANON_LOGGING_H
#define GANON_LOGGING_H

#include <stddef.h>

typedef enum {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
} log_level_t;

void log_message(const char *level, const char *file, int line, const char *msg, ...);
void log_set_level(log_level_t level);
log_level_t log_get_level(void);
void log_init_from_env(void);

#ifdef __DEBUG__
#define LOG_INFO(msg, ...)  do { if (LOG_LEVEL_INFO >= log_get_level()) log_message("INFO", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_DEBUG(msg, ...) do { if (LOG_LEVEL_DEBUG >= log_get_level()) log_message("DEBUG", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_TRACE(msg, ...) do { if (LOG_LEVEL_TRACE >= log_get_level()) log_message("TRACE", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_WARNING(msg, ...) log_message("WARN", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) log_message("WARN", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) log_message("ERROR", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#else
#define LOG_INFO(msg, ...)  do { if (LOG_LEVEL_INFO >= log_get_level()) log_message("INFO", __FILE__, __LINE__, msg, ##__VA_ARGS__); } while (0)
#define LOG_DEBUG(msg, ...)  ((void)0)
#define LOG_TRACE(msg, ...) ((void)0)
#define LOG_WARNING(msg, ...) log_message("WARN", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) log_message("WARN", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) log_message("ERROR", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#endif

#endif