#ifndef GANON_SKIN_TCP_CHACHA20_H
#define GANON_SKIN_TCP_CHACHA20_H

#include "skin.h"

/* Return the singleton vtable for the tcp-chacha20 skin. */
const skin_ops_t *SKIN_TCP_CHACHA20__ops(void);

/* Register this skin in the global registry (calls SKIN__register). */
err_t SKIN_TCP_CHACHA20__register(void);

#endif /* #ifndef GANON_SKIN_TCP_CHACHA20_H */
