#ifndef GANON_COMMON_H
#define GANON_COMMON_H

#include "err.h"

#define FAIL_IF(condition, error) \
    do { if (condition) { rc = error; goto l_cleanup; } } while (0)

#endif