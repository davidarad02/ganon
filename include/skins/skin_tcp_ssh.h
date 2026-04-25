#ifndef GANON_SKIN_TCP_SSH_H
#define GANON_SKIN_TCP_SSH_H

#include "skin.h"

/* Return the singleton vtable for the tcp-ssh skin. */
const skin_ops_t *SKIN_TCP_SSH__ops(void);

/* Register this skin in the global registry (calls SKIN__register). */
err_t SKIN_TCP_SSH__register(void);

#endif /* #ifndef GANON_SKIN_TCP_SSH_H */
