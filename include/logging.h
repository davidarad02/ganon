#ifndef GANON_LOGGING_H
#define GANON_LOGGING_H

#ifdef __DEBUG__
#define LOG_INFO(msg, ...)  log_message("INFO", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_DEBUG(msg, ...) log_message("DEBUG", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#define LOG_TRACE(msg, ...) log_message("TRACE", __FILE__, __LINE__, msg, ##__VA_ARGS__)
#else
#define LOG_INFO(msg, ...)  ((void)0)
#define LOG_DEBUG(msg, ...) ((void)0)
#define LOG_TRACE(msg, ...) ((void)0)
#endif

void log_message(const char *level, const char *file, int line, const char *msg, ...);

#endif