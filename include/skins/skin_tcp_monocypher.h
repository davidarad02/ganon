#ifndef GANON_SKIN_TCP_MONOCYPHER_H
#define GANON_SKIN_TCP_MONOCYPHER_H

#include "skin.h"

/* Return the singleton vtable for the tcp-monocypher skin. */
const skin_ops_t *SKIN_TCPM__ops(void);

/* Register this skin in the global registry (calls SKIN__register). */
err_t SKIN_TCPM__register(void);

#endif /* #ifndef GANON_SKIN_TCP_MONOCYPHER_H */
