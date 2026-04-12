#ifndef GANON_ERR_H
#define GANON_ERR_H

typedef enum {
    E__SUCCESS = 0,
    E__GANON_FAILURE,
    E__MAIN__RC_DEMO__SOME_ERROR,
    E__MAIN__FAILURE,
} err_t;

#endif