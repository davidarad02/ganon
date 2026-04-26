#ifndef GANON_SKINS_SKIN_UDP_QUIC_H
#define GANON_SKINS_SKIN_UDP_QUIC_H

#include "skin.h"

/* Public functions to get the skin operations and register the skin. */
const skin_ops_t *SKIN_UDP_QUIC__ops(void);
err_t SKIN_UDP_QUIC__register(void);

#endif /* #ifndef GANON_SKINS_SKIN_UDP_QUIC_H */