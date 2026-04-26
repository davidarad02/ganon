#ifndef GANON_SKINS_QUIC_TLS_H
#define GANON_SKINS_QUIC_TLS_H

#include "skin_udp_quic_internal.h"

/*
 * Generate a self-signed ECDSA P-256 certificate in memory and populate the
 * fields of the listener's TLS context.  Called once from listener_create.
 *
 * On success tls_ctx, sign_cert, pkey, cert_der / cert_der_len, cert_iov are
 * all populated and 0 is returned.  On failure returns a negative error code
 * and the caller must not use any of the output pointers.
 */
int quic_tls_listener_init(ptls_context_t          *tls_ctx,
                             quic_sign_cert_t       *sign_cert,
                             mbedtls_pk_context      *pkey,
                             ptls_key_exchange_algorithm_t **key_ex  /* [2] */,
                             ptls_cipher_suite_t           **ciphers /* [3] */,
                             uint8_t                **cert_der,
                             size_t                  *cert_der_len,
                             ptls_iovec_t            *cert_iov);

#endif /* GANON_SKINS_QUIC_TLS_H */
