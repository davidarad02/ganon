#ifndef GANON_SKINS_QUIC_NGTCP2_CRYPTO_H
#define GANON_SKINS_QUIC_NGTCP2_CRYPTO_H

#include <ngtcp2/ngtcp2.h>

/* Free all stable crypto buffers that were allocated on behalf of conn
 * during the QUIC handshake.  Must be called from transport_destroy. */
void quic_crypto_free_conn_data(ngtcp2_conn *conn);

#endif /* GANON_SKINS_QUIC_NGTCP2_CRYPTO_H */
