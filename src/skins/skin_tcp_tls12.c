/*
 * tcp-tls12 skin: TCP + TLS 1.2 (mbedTLS 3.x).
 *
 * Uses an in-memory self-signed ECDSA P-256 cert generated at listener
 * startup.  Client side skips certificate verification — ganon's NODE_INIT
 * exchange is the real authentication.
 *
 * Wire frame format (identical to tcp-plain, inside the TLS stream):
 *   [4 bytes] big-endian payload length
 *   [N bytes] serialized protocol_msg_t + data
 *
 * ALPN: none (TLS 1.2 ALPN is cleartext and more fingerprintable).
 */

#include "skins/skin_tcp_tls12.h"
#include "skins/tls_common.h"
#include "err.h"
#include "skin.h"

static err_t tls12_listener_create(const addr_t *addr,
                                    skin_listener_t **out_listener,
                                    int *out_listen_fd) {
    return tls_common_listener_create(addr,
                                      MBEDTLS_SSL_VERSION_TLS1_2,
                                      MBEDTLS_SSL_VERSION_TLS1_2,
                                      NULL,
                                      SKIN_TCP_TLS12__ops(),
                                      out_listener,
                                      out_listen_fd);
}

static err_t tls12_connect(const char *ip, int port, int timeout,
                            transport_t **out_transport) {
    return tls_common_connect(ip, port, timeout,
                              MBEDTLS_SSL_VERSION_TLS1_2,
                              MBEDTLS_SSL_VERSION_TLS1_2,
                              NULL,
                              SKIN_TCP_TLS12__ops(),
                              out_transport);
}

static const skin_ops_t s_tls12_ops = {
    .skin_id            = SKIN_ID__TLS12,
    .name               = "tcp-tls12",
    .listener_create    = tls12_listener_create,
    .listener_accept    = tls_common_listener_accept,
    .listener_destroy   = tls_common_listener_destroy,
    .connect            = tls12_connect,
    .send_msg           = tls_common_send_msg,
    .recv_msg           = tls_common_recv_msg,
    .transport_destroy  = tls_common_transport_destroy,
};

const skin_ops_t *SKIN_TCP_TLS12__ops(void) {
    return &s_tls12_ops;
}

err_t SKIN_TCP_TLS12__register(void) {
    return SKIN__register(&s_tls12_ops);
}
