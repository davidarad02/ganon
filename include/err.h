#ifndef GANON_ERR_H
#define GANON_ERR_H

typedef enum {
    /* Success */
    E__SUCCESS = 0,

    /* Generic errors */
    E__INVALID_ARG_NULL_POINTER = 0x001,

    /* main.c */
    E__MAIN__RC_DEMO__SOME_ERROR = 0x100,

    /* args.c */
    E__ARGS__NULL_POINTER = 0x200,
    E__ARGS__CONFLICTING_ARGUMENTS,
    E__ARGS__MISSING_REQUIRED_ARGUMENT,
    E__ARGS__INVALID_FORMAT,
    E__ARGS__UNKNOWN_FLAG,
} err_t;

#endif