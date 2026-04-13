#ifndef GANON_COMMON_H
#define GANON_COMMON_H

#include <stddef.h>

#include "err.h"

typedef int bool_t;

#define false ((bool_t)0)
#define true ((bool_t)1)

#define FAIL_IF(condition, error) \
    if (condition) { rc = error; goto l_cleanup; }

#define FAIL(error) \
    { rc = error; goto l_cleanup; }

#define BREAK_IF(condition) \
    if (condition) { break; }

#define CONTINUE_IF(condition) \
    if (condition) { continue; }

#define FREE(ptr) \
    do { \
        if (NULL != (ptr)) { \
            free((ptr)); \
            (ptr) = NULL; \
        } \
    } while (0)

#define VALIDATE_ARGS(...) \
    do { \
        const void *args[] = { __VA_ARGS__ }; \
        for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++) { \
            if (NULL == args[i]) { \
                rc = E__INVALID_ARG_NULL_POINTER; \
                goto l_cleanup; \
            } \
        } \
    } while (0)

#define VALIDATE_ARGS_ERRNO(err, ...) \
    do { \
        const void *args[] = { __VA_ARGS__ }; \
        for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++) { \
            if (NULL == args[i]) { \
                rc = err; \
                goto l_cleanup; \
            } \
        } \
    } while (0)

#endif /* #ifndef GANON_COMMON_H */