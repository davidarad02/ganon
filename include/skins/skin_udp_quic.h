#ifndef GANON_SKIN_UDP_QUIC_H
#define GANON_SKIN_UDP_QUIC_H

#include "err.h"
#include "skin.h"

const skin_ops_t *SKIN_UDP_QUIC__ops(void);
err_t             SKIN_UDP_QUIC__register(void);

#endif /* GANON_SKIN_UDP_QUIC_H */
