/*
 * tcp-tls13 skin: TCP + TLS 1.3 (mbedTLS 3.x).
 *
 * Uses an in-memory self-signed ECDSA P-256 cert generated at listener
 * startup.  Client side skips certificate verification — ganon's NODE_INIT
 * exchange is the real authentication.
 *
 * Wire frame format (identical to tcp-plain, inside the TLS stream):
 *   [4 bytes] big-endian payload length
 *   [N bytes] serialized protocol_msg_t + data
 *
 * ALPN: "tls13-ganon" (TLS 1.3 encrypts the ALPN extension).
 */

#include "skins/skin_tcp_tls13.h"
#include "skins/tls_common.h"
#include "err.h"
#include "skin.h"

#define TLS13_ALPN "tls13-ganon"

static err_t tls13_listener_create(const addr_t *addr,
                                    skin_listener_t **out_listener,
                                    int *out_listen_fd) {
    return tls_common_listener_create(addr,
                                      MBEDTLS_SSL_VERSION_TLS1_3,
                                      MBEDTLS_SSL_VERSION_TLS1_3,
                                      TLS13_ALPN,
                                      SKIN_TCP_TLS13__ops(),
                                      out_listener,
                                      out_listen_fd);
}

static err_t tls13_connect(const char *ip, int port, int timeout,
                            transport_t **out_transport) {
    return tls_common_connect(ip, port, timeout,
                              MBEDTLS_SSL_VERSION_TLS1_3,
                              MBEDTLS_SSL_VERSION_TLS1_3,
                              TLS13_ALPN,
                              SKIN_TCP_TLS13__ops(),
                              out_transport);
}

static const skin_ops_t s_tls13_ops = {
    .skin_id            = SKIN_ID__TLS13,
    .name               = "tcp-tls13",
    .listener_create    = tls13_listener_create,
    .listener_accept    = tls_common_listener_accept,
    .listener_destroy   = tls_common_listener_destroy,
    .connect            = tls13_connect,
    .send_msg           = tls_common_send_msg,
    .recv_msg           = tls_common_recv_msg,
    .transport_destroy  = tls_common_transport_destroy,
};

const skin_ops_t *SKIN_TCP_TLS13__ops(void) {
    return &s_tls13_ops;
}

err_t SKIN_TCP_TLS13__register(void) {
    return SKIN__register(&s_tls13_ops);
}
