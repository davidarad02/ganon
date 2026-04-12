#ifndef GANON_ERR_H
#define GANON_ERR_H

typedef enum {
    /* Success */
    E__SUCCESS = 0,

    /* main.c */
    E__MAIN__RC_DEMO__SOME_ERROR = 0x100,

    /* args.c */
    E__ARGS__INVALID_ARGUMENTS = 0x200,
    E__ARGS__CONFLICTING_ARGUMENTS,
    E__ARGS__MISSING_VALUE,
    E__ARGS__INVALID_VALUE,
    E__ARGS__INVALID_ARGUMENT,
} err_t;

#endif