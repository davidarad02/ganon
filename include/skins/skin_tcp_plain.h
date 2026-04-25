#ifndef GANON_SKIN_TCP_PLAIN_H
#define GANON_SKIN_TCP_PLAIN_H

#include "skin.h"

/* Return the singleton vtable for the tcp-plain skin. */
const skin_ops_t *SKIN_TCP_PLAIN__ops(void);

/* Register this skin in the global registry (calls SKIN__register). */
err_t SKIN_TCP_PLAIN__register(void);

#endif /* #ifndef GANON_SKIN_TCP_PLAIN_H */
