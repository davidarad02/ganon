#ifndef GANON_SKIN_UDP_QUIC_INTERNAL_H
#define GANON_SKIN_UDP_QUIC_INTERNAL_H

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_picotls.h>
#include <picotls.h>
#include <pthread.h>
#include <sys/socket.h>

typedef struct quic_crypto_data {
    uint8_t *buf;
    struct quic_crypto_data *next;
} quic_crypto_data_t;

typedef struct quic_pending_data {
    uint64_t offset;
    size_t len;
    uint8_t *buf;
    struct quic_pending_data *next;
} quic_pending_data_t;

typedef struct skin_quic_ctx {
    int                         udp_fd;

    ngtcp2_conn                *conn;
    ngtcp2_crypto_picotls_ctx   cptls;
    ngtcp2_crypto_conn_ref      conn_ref;

    /* For the picotls context on client side */
    ptls_context_t              tls_ctx_client;
    ptls_key_exchange_algorithm_t *key_ex_c[2];
    ptls_cipher_suite_t           *ciphers_c[3];
    ptls_iovec_t                  alpn_list[1];  /* ALPN for client side */

    struct sockaddr_storage     local_addr;
    socklen_t                   local_addrlen;
    struct sockaddr_storage     remote_addr;
    socklen_t                   remote_addrlen;

    int64_t                     stream_id;       /* -1 until opened */
    int                         handshake_done;
    int                         is_server;

    /* Pipes: I/O thread ↔ ganon layer */
    int                         rx_pipe[2];      /* quic-thread writes, recv_msg reads */
    int                         tx_pipe[2];      /* send_msg writes, quic-thread reads */

    pthread_t                   io_thread;
    volatile int                io_running;
    /* Stream data reassembly buffer (used inside I/O thread) */
    uint8_t                    *stream_buf;
    size_t                      stream_buf_len;
    size_t                      stream_buf_cap;

    /* Transmit data buffer (used inside I/O thread) */
    uint8_t                    *tx_pipe_buf;
    size_t                      tx_pipe_buf_len;
    size_t                      tx_pipe_buf_cap;

    pthread_mutex_t             conn_mutex;

    /* TLS extensions storage */
    ptls_raw_extension_t        exts[2];

    ngtcp2_ccerr                last_error;

    /* Pending stream data for lifecycle management */
    struct quic_pending_data *pending_data;
    struct quic_pending_data *pending_data_tail;
    uint64_t stream_acked_offset;
    uint64_t stream_sent_offset;
    uint64_t stream_queued_offset;

    /* Handshake data that must remain stable in memory */
    struct quic_crypto_data *crypto_data;
} skin_quic_ctx_t;

#endif /* GANON_SKIN_UDP_QUIC_INTERNAL_H */
