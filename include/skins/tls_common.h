#ifndef GANON_SKINS_TLS_COMMON_H
#define GANON_SKINS_TLS_COMMON_H

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <stdint.h>
#include <stddef.h>

#include "common.h"
#include "err.h"
#include "skin.h"
#include "transport.h"

/*
 * Shared per-listener state for the mbedTLS TCP skins (tcp-tls12, tcp-tls13).
 */
typedef struct {
    int                          listen_fd;
    addr_t                       addr;
    mbedtls_x509_crt             cert;
    mbedtls_pk_context           pkey;
    mbedtls_entropy_context      entropy;    /* thread-safe */
    mbedtls_ssl_protocol_version min_ver;
    mbedtls_ssl_protocol_version max_ver;
    char                         alpn_str[32];
    const char                  *alpn_list[2]; /* { alpn_str, NULL } or { NULL, NULL } */
    const skin_ops_t            *skin;
} tls_common_listener_t;

/*
 * Shared per-connection context for mbedTLS TCP skins.
 * Stored in transport_t.skin_ctx.
 */
typedef struct {
    int                      fd;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       cfg;
    mbedtls_entropy_context  entropy;   /* own entropy; ctr_drbg holds a pointer to it */
    mbedtls_ctr_drbg_context ctr_drbg;
    char                     alpn_str[32];
    const char              *alpn_list[2]; /* points into alpn_str */
} tls_common_ctx_t;

/*
 * Generate a self-signed ECDSA P-256 certificate in memory.
 * cert and pkey are initialised by this function; caller must call
 * mbedtls_x509_crt_free + mbedtls_pk_free on failure and eventual cleanup.
 *
 * If out_cert_der / out_cert_der_len are non-NULL, a malloc'd copy of the
 * raw DER bytes is returned (caller frees).
 * Returns 0 on success, negative on error.
 */
int tls_common_generate_cert(mbedtls_x509_crt        *cert,
                              mbedtls_pk_context      *pkey,
                              mbedtls_entropy_context *entropy,
                              uint8_t                **out_cert_der,
                              size_t                  *out_cert_der_len);

/*
 * Skin vtable entry points called by the thin wrappers in skin_tcp_tls1{2,3}.c
 */
err_t tls_common_listener_create(const addr_t              *addr,
                                  mbedtls_ssl_protocol_version min_ver,
                                  mbedtls_ssl_protocol_version max_ver,
                                  const char                *alpn,
                                  const skin_ops_t          *skin,
                                  skin_listener_t          **out_listener,
                                  int                       *out_listen_fd);

err_t tls_common_listener_accept(skin_listener_t  *l,
                                  transport_t     **out_transport);

void  tls_common_listener_destroy(skin_listener_t *l);

err_t tls_common_connect(const char               *ip,
                          int                       port,
                          int                       connect_timeout_sec,
                          mbedtls_ssl_protocol_version min_ver,
                          mbedtls_ssl_protocol_version max_ver,
                          const char               *alpn,
                          const skin_ops_t         *skin,
                          transport_t             **out_transport);

err_t tls_common_send_msg(transport_t           *t,
                           const protocol_msg_t  *msg,
                           const uint8_t         *data);

err_t tls_common_recv_msg(transport_t      *t,
                           protocol_msg_t   *msg,
                           uint8_t         **out_data);

void  tls_common_transport_destroy(transport_t *t);

#endif /* GANON_SKINS_TLS_COMMON_H */
