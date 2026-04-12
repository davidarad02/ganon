#ifndef GANON_COMMON_H
#define GANON_COMMON_H

#include <stddef.h>

#include "err.h"

#define FAIL_IF(condition, error) \
    if (condition) { rc = error; goto l_cleanup; }

#define FAIL(error) \
    { rc = error; goto l_cleanup; }

#define BREAK_IF(condition) \
    if (condition) { break; }

#define CONTINUE_IF(condition) \
    if (condition) { continue; }

#endif