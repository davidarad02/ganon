#ifndef GANON_SKINS_SKIN_UDP_QUIC_INTERNAL_H
#define GANON_SKINS_SKIN_UDP_QUIC_INTERNAL_H

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_picotls.h>
#include <picotls.h>
#include <picotls/mbedtls.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <psa/crypto.h>
#include <pthread.h>
#include <sys/socket.h>

#include "transport.h"

/* Sign certificate callback struct for picotls mbedTLS integration. */
typedef struct {
    ptls_sign_certificate_t  super;
    mbedtls_pk_context      *key;
} quic_sign_cert_t;

typedef struct quic_pending_data {
    uint64_t                   offset;
    size_t                     len;
    uint8_t                   *buf;
    struct quic_pending_data *next;
} quic_pending_data_t;

/* Per-listener state. */
typedef struct {
    int                              listen_fd;
    addr_t                           addr;

    ptls_context_t                   tls_ctx;
    quic_sign_cert_t                sign_cert;   /* embedded — not a pointer */
    ptls_key_exchange_algorithm_t   *key_ex[2];
    ptls_cipher_suite_t             *ciphers[3];
    mbedtls_pk_context               pkey;
    uint8_t                         *cert_der;
    size_t                           cert_der_len;
    ptls_iovec_t                     cert_iov[1];
} skin_quic_listener_t;

/* Per-connection state. */
typedef struct {
    int                              udp_fd;
    int                              is_incoming;
    int                              is_server;
    struct sockaddr_storage          remote_addr;
    socklen_t                        remote_addrlen;
    struct sockaddr_storage          local_addr;
    socklen_t                        local_addrlen;

    ngtcp2_conn                     *conn;
    ngtcp2_crypto_picotls_ctx        cptls;
    ngtcp2_crypto_conn_ref           conn_ref;

    int                              handshake_done;
    int64_t                          stream_id;

    pthread_mutex_t                  conn_mutex;
    pthread_t                        io_thread;
    volatile int                     io_running;

    int                              rx_pipe[2]; /* I/O thread writes, recv reads */
    int                              tx_pipe[2]; /* send writes, I/O thread reads */

    /* Stream reassembly buffer (I/O thread side) */
    uint8_t                         *stream_buf;
    size_t                           stream_buf_len;
    size_t                           stream_buf_cap;

    /* TX buffer assembled from tx_pipe reads (I/O thread side) */
    uint8_t                         *tx_pipe_buf;
    size_t                           tx_pipe_buf_len;
    size_t                           tx_pipe_buf_cap;

    /* Pending stream data awaiting ngtcp2 ACK */
    quic_pending_data_t            *pending_data;
    quic_pending_data_t            *pending_data_tail;
    uint64_t                         stream_acked_offset;
    uint64_t                         stream_sent_offset;
    uint64_t                         stream_queued_offset;

    /* QUIC transport-params extension storage for picotls.
     * exts[1].type MUST be UINT16_MAX sentinel from the moment this
     * is assigned to handshake_properties.additional_extensions so that
     * picotls does not walk off the end of the two-entry array. */
    ptls_raw_extension_t             exts[2];

    ngtcp2_ccerr                     last_error;

    /* Client-side TLS context (unused on server) */
    ptls_context_t                   tls_ctx_client;
    ptls_key_exchange_algorithm_t   *key_ex_c[2];
    ptls_cipher_suite_t             *ciphers_c[3];
    ptls_iovec_t                     alpn_list[1];
} skin_quic_ctx_t;

#endif /* GANON_SKINS_SKIN_UDP_QUIC_INTERNAL_H */
