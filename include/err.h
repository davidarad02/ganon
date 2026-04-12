#ifndef GANON_ERR_H
#define GANON_ERR_H

typedef enum {
    /* Success */
    E__SUCCESS = 0,

    /* main.c */
    /* Demo error for function conventions */
    E__MAIN__RC_DEMO__SOME_ERROR,

    /* args.c */
    E__ARGS__INVALID_ARGUMENTS,
    E__ARGS__CONFLICTING_ARGUMENTS,
    E__ARGS__MISSING_VALUE,
    E__ARGS__INVALID_VALUE,
    E__ARGS__INVALID_ARGUMENT,
} err_t;

#endif