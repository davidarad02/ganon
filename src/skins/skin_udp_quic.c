/*
 * udp-quic skin: QUIC transport using ngtcp2 + picotls (OpenSSL crypto).
 *
 * Each ganon peer connection maps to one QUIC connection over UDP.
 * A single bidirectional stream carries all ganon protocol frames, using
 * the same 4-byte big-endian length-prefix framing as tcp-plain.
 *
 * Architecture:
 *   - listener: one shared UDP socket; accepts new QUIC connections.
 *   - per-connection: background I/O thread manages the ngtcp2 event loop.
 *   - recv_msg / send_msg communicate with the I/O thread via pipe pairs.
 *
 * TLS: self-signed ECDSA P-256 certificate generated at listener creation.
 * Client side skips certificate verification (ganon authentication is at
 * the protocol layer via node IDs).
 *
 * x64 only — picotls-openssl requires OpenSSL, cross-compilation would
 * need a different crypto backend.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_picotls.h>

#include <picotls.h>
#ifdef SKIN_QUIC_USE_MBEDTLS
#include <picotls/mbedtls.h>
#include <psa/crypto.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

/* Custom sign_certificate implementation for mbedTLS as picotls doesn't provide one */
typedef struct st_ptls_mbedtls_sign_certificate_t {
    ptls_sign_certificate_t super;
    mbedtls_pk_context *key;
} ptls_mbedtls_sign_certificate_t;

#else
#include <picotls/openssl.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include "common.h"
#include "err.h"
#include "logging.h"
#include "protocol.h"
#include "skin.h"
#include "skins/skin_udp_quic.h"
#include "transport.h"
#include "skin_udp_quic_internal.h"

/* ── Constants ─────────────────────────────────────────────────────────────── */

#define QUIC_ALPN        "\x05ganon"   /* 1-byte length + "ganon" */
#define QUIC_ALPN_LEN    6
#define QUIC_MAX_PKT     1500
#define QUIC_RX_BUF_INIT 65536
#define QUIC_PIPE_MSG_HDR 4            /* uint32_t frame length prefix in pipe */

/* ── Timestamp helper ───────────────────────────────────────────────────────── */

#ifdef SKIN_QUIC_USE_MBEDTLS
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static int g_rng_init = 0;

static void quic_rng_init(void) {
    if (!g_rng_init) {
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_ctr_drbg);
        mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)"ganon_quic", 10);
        g_rng_init = 1;
    }
}
#endif

static ngtcp2_tstamp quic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * NGTCP2_SECONDS +
           (ngtcp2_tstamp)ts.tv_nsec;
}

/* ── Listener state ─────────────────────────────────────────────────────────── */

typedef struct {
    int                              listen_fd;
    addr_t                           addr;

    /* TLS context shared across accepted connections */
    ptls_context_t                   tls_ctx;
#ifdef SKIN_QUIC_USE_MBEDTLS
    ptls_mbedtls_sign_certificate_t  sign_cert;
#else
    ptls_openssl_sign_certificate_t  sign_cert;
#endif
    ptls_key_exchange_algorithm_t   *key_ex[2];
    ptls_cipher_suite_t             *ciphers[3];

    /* Self-signed cert + key */
#ifdef SKIN_QUIC_USE_MBEDTLS
    mbedtls_pk_context               pkey;
#else
    EVP_PKEY                        *pkey;
#endif
    uint8_t                         *cert_der;
    size_t                           cert_der_len;
    ptls_iovec_t                     cert_iov[1];
} skin_quic_listener_t;

/* ── Forward declarations ────────────────────────────────────────────────────── */

static ngtcp2_conn *quic_get_conn(ngtcp2_crypto_conn_ref *ref);
static void *quic_io_thread(void *arg);
static err_t quic_flush_send(skin_quic_ctx_t *ctx);
static err_t quic_write_stream(skin_quic_ctx_t *ctx, const uint8_t *data, size_t datalen);
static err_t quic_service_tx(skin_quic_ctx_t *ctx);
static int quic_drain_stream_buf(skin_quic_ctx_t *ctx);
static err_t quic_start_io_thread(skin_quic_ctx_t *ctx);

/* ── Randomness ─────────────────────────────────────────────────────────────── */

static void quic_random(void *buf, size_t len) {
#ifdef SKIN_QUIC_USE_MBEDTLS
    quic_rng_init();
    mbedtls_ctr_drbg_random(&g_ctr_drbg, buf, len);
#else
    RAND_bytes(buf, (int)len);
#endif
}

static void quic_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* ── ngtcp2 callbacks ────────────────────────────────────────────────────────── */

static ngtcp2_conn *quic_get_conn(ngtcp2_crypto_conn_ref *ref) {
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)ref->user_data;
    return ctx->conn;
}

static void quic_rand_cb(uint8_t *dest, size_t destlen,
                          const ngtcp2_rand_ctx *rand_ctx) {
    (void)rand_ctx;
    quic_random(dest, destlen);
}

static int quic_get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                          uint8_t *token, size_t cidlen,
                                          void *user_data) {
    (void)conn;
    (void)user_data;
    quic_random(cid->data, cidlen);
    cid->datalen = cidlen;
    quic_random(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

static int quic_drain_stream_buf(skin_quic_ctx_t *ctx) {
    while (ctx->stream_buf_len >= QUIC_PIPE_MSG_HDR) {
        uint32_t frame_len = ((uint32_t)ctx->stream_buf[0] << 24) |
                             ((uint32_t)ctx->stream_buf[1] << 16) |
                             ((uint32_t)ctx->stream_buf[2] <<  8) |
                             ((uint32_t)ctx->stream_buf[3]);

        if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
            LOG_ERROR("QUIC: invalid frame length %u", frame_len);
            return -1;
        }

        if (ctx->stream_buf_len < QUIC_PIPE_MSG_HDR + frame_len) {
            break;  /* incomplete frame, wait for more data */
        }

        /* Write length prefix + payload atomically to rx_pipe */
        /* We write: [4-byte frame_len][frame_len bytes] so recv_msg can read them */
        size_t total = QUIC_PIPE_MSG_HDR + frame_len;
        ssize_t written = write(ctx->rx_pipe[1], ctx->stream_buf, total);
        if (written < 0) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                break; /* Pipe full, try again later from I/O loop */
            }
            LOG_ERROR("QUIC: rx_pipe write failed: %s", strerror(errno));
            return -1;
        }
        if ((size_t)written < total) {
            /* Partial write */
            ctx->stream_buf_len -= (size_t)written;
            memmove(ctx->stream_buf, ctx->stream_buf + (size_t)written, ctx->stream_buf_len);
            break;
        }

        /* Shift buffer */
        ctx->stream_buf_len -= total;
        if (ctx->stream_buf_len > 0) {
            memmove(ctx->stream_buf, ctx->stream_buf + total, ctx->stream_buf_len);
        }
    }
    return 0;
}

static int quic_drain_tx_pipe_buf(skin_quic_ctx_t *ctx) {
    while (ctx->tx_pipe_buf_len >= QUIC_PIPE_MSG_HDR) {
        uint32_t frame_len = ((uint32_t)ctx->tx_pipe_buf[0] << 24) |
                             ((uint32_t)ctx->tx_pipe_buf[1] << 16) |
                             ((uint32_t)ctx->tx_pipe_buf[2] <<  8) |
                             ((uint32_t)ctx->tx_pipe_buf[3]);

        if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
            LOG_ERROR("QUIC: invalid tx frame length %u", frame_len);
            return -1;
        }

        if (ctx->tx_pipe_buf_len < QUIC_PIPE_MSG_HDR + frame_len) {
            break;  /* incomplete frame */
        }

        size_t total = QUIC_PIPE_MSG_HDR + frame_len;
        if (ctx->stream_id != -1) {
            quic_write_stream(ctx, ctx->tx_pipe_buf, total);
        }

        /* Shift buffer */
        ctx->tx_pipe_buf_len -= total;
        if (ctx->tx_pipe_buf_len > 0) {
            memmove(ctx->tx_pipe_buf, ctx->tx_pipe_buf + total, ctx->tx_pipe_buf_len);
        }
    }
    return 0;
}

static int quic_recv_stream_data_cb(ngtcp2_conn *conn,
                                     uint32_t flags, int64_t stream_id,
                                     uint64_t offset, const uint8_t *data,
                                     size_t datalen, void *user_data,
                                     void *stream_user_data) {
    (void)flags;
    (void)stream_id;
    (void)offset;
    (void)stream_user_data;

    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;

    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
    ngtcp2_conn_extend_max_offset(conn, datalen);

    if (0 == datalen) {
        return 0;
    }

    /* Append to reassembly buffer */
    if (ctx->stream_buf_len + datalen > ctx->stream_buf_cap) {
        size_t new_cap = ctx->stream_buf_cap + datalen + QUIC_RX_BUF_INIT;
        uint8_t *nb = realloc(ctx->stream_buf, new_cap);
        if (NULL == nb) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        ctx->stream_buf     = nb;
        ctx->stream_buf_cap = new_cap;
    }
    memcpy(ctx->stream_buf + ctx->stream_buf_len, data, datalen);
    ctx->stream_buf_len += datalen;

    /* Drain complete frames (4-byte length prefix + payload) and write to rx_pipe */
    if (0 != quic_drain_stream_buf(ctx)) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}

static int quic_stream_open_cb(ngtcp2_conn *conn, int64_t stream_id,
                                 void *user_data) {
    (void)conn;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;
    /* Server: accept the first bidirectional stream the client opens */
    if (ctx->is_server && -1 == ctx->stream_id) {
        ctx->stream_id = stream_id;
        LOG_DEBUG("QUIC: server accepted stream %lld", (long long)stream_id);
    }
    return 0;
}

static int quic_handshake_completed_cb(ngtcp2_conn *conn, void *user_data) {
    (void)conn;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;
    ctx->handshake_done = 1;
    LOG_INFO("QUIC: handshake completed (is_server=%d)", ctx->is_server);
    return 0;
}

static int quic_extend_max_local_streams_bidi_cb(ngtcp2_conn *conn,
                                                   uint64_t max_streams,
                                                   void *user_data) {
    (void)max_streams;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;

    /* Client: open the one bidirectional stream we'll use */
    if (!ctx->is_server && -1 == ctx->stream_id) {
        int64_t stream_id;
        int rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL);
        if (0 == rv) {
            ctx->stream_id = stream_id;
            LOG_DEBUG("QUIC: client opened stream %lld", (long long)stream_id);
        }
    }
    return 0;
}

static int quic_acked_stream_data_offset_cb(ngtcp2_conn *conn, int64_t stream_id,
                                            uint64_t offset, uint64_t datalen,
                                            void *user_data, void *stream_user_data) {
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;
    (void)conn; (void)stream_id; (void)stream_user_data;

    uint64_t acked_upto = offset + datalen;
    
    /* Free all pending data that is now fully acked */
    quic_pending_data_t **curr = &ctx->pending_data;
    while (*curr) {
        quic_pending_data_t *p = *curr;
        if (p->offset + p->len <= acked_upto) {
            *curr = p->next;
            if (p == ctx->pending_data_tail) {
                ctx->pending_data_tail = NULL;
            }
            free(p->buf);
            free(p);
        } else {
            curr = &((*curr)->next);
        }
    }

    if (acked_upto > ctx->stream_acked_offset) {
        ctx->stream_acked_offset = acked_upto;
    }

    return 0;
}

static ngtcp2_callbacks g_client_callbacks = {
    .client_initial              = ngtcp2_crypto_client_initial_cb,
    .recv_crypto_data            = ngtcp2_crypto_recv_crypto_data_cb,
    .encrypt                     = ngtcp2_crypto_encrypt_cb,
    .decrypt                     = ngtcp2_crypto_decrypt_cb,
    .hp_mask                     = ngtcp2_crypto_hp_mask_cb,
    .recv_retry                  = ngtcp2_crypto_recv_retry_cb,
    .rand                        = quic_rand_cb,
    .get_new_connection_id       = quic_get_new_connection_id_cb,
    .update_key                  = ngtcp2_crypto_update_key_cb,
    .delete_crypto_aead_ctx      = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx    = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data     = ngtcp2_crypto_get_path_challenge_data_cb,
    .version_negotiation         = ngtcp2_crypto_version_negotiation_cb,
    .recv_stream_data            = quic_recv_stream_data_cb,
    .stream_open                 = quic_stream_open_cb,
    .handshake_completed         = quic_handshake_completed_cb,
    .extend_max_local_streams_bidi = quic_extend_max_local_streams_bidi_cb,
    .acked_stream_data_offset    = quic_acked_stream_data_offset_cb,
};

static ngtcp2_callbacks g_server_callbacks = {
    .recv_client_initial         = ngtcp2_crypto_recv_client_initial_cb,
    .recv_crypto_data            = ngtcp2_crypto_recv_crypto_data_cb,
    .encrypt                     = ngtcp2_crypto_encrypt_cb,
    .decrypt                     = ngtcp2_crypto_decrypt_cb,
    .hp_mask                     = ngtcp2_crypto_hp_mask_cb,
    .rand                        = quic_rand_cb,
    .get_new_connection_id       = quic_get_new_connection_id_cb,
    .update_key                  = ngtcp2_crypto_update_key_cb,
    .delete_crypto_aead_ctx      = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx    = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data     = ngtcp2_crypto_get_path_challenge_data_cb,
    .version_negotiation         = ngtcp2_crypto_version_negotiation_cb,
    .recv_stream_data            = quic_recv_stream_data_cb,
    .stream_open                 = quic_stream_open_cb,
    .handshake_completed         = quic_handshake_completed_cb,
    .acked_stream_data_offset    = quic_acked_stream_data_offset_cb,
};

/* ── Send helper ─────────────────────────────────────────────────────────────── */

static err_t quic_flush_send(skin_quic_ctx_t *ctx) {
    err_t rc = E__SUCCESS;
    uint8_t pkt[QUIC_MAX_PKT];
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;

    ngtcp2_path_storage_zero(&ps);

    for (;;) {
        ngtcp2_tstamp ts = quic_now();
        pthread_mutex_lock(&ctx->conn_mutex);
        ngtcp2_ssize n = ngtcp2_conn_write_pkt(ctx->conn, &ps.path, &pi,
                                                pkt, sizeof(pkt), ts);
        pthread_mutex_unlock(&ctx->conn_mutex);
        if (0 == n) {
            break;
        }
        if (0 > n) {
            if (NGTCP2_ERR_WRITE_MORE == (int)n) {
                continue;
            }
            LOG_WARN("ngtcp2_conn_write_pkt: %s", ngtcp2_strerror((int)n));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        ssize_t sent = send(ctx->udp_fd, pkt, (size_t)n, 0);
        if (0 > sent) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARN("QUIC sendto: %s", strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
    }

l_cleanup:
    return rc;
}

/* ── Stream write helper (from I/O thread) ───────────────────────────────────── */

static err_t quic_write_stream(skin_quic_ctx_t *ctx,
                                const uint8_t *data, size_t datalen) {
    err_t rc = E__SUCCESS;

    /* Allocate stable copy and append to FIFO queue */
    quic_pending_data_t *p = malloc(sizeof(*p));
    FAIL_IF(NULL == p, E__INVALID_ARG_NULL_POINTER);
    p->offset = ctx->stream_queued_offset;
    p->len = datalen;
    p->buf = malloc(datalen);
    if (NULL == p->buf) {
        free(p);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    memcpy(p->buf, data, datalen);
    p->next = NULL;

    if (ctx->pending_data_tail) {
        ctx->pending_data_tail->next = p;
        ctx->pending_data_tail = p;
    } else {
        ctx->pending_data = ctx->pending_data_tail = p;
    }
    ctx->stream_queued_offset += datalen;

l_cleanup:
    return rc;
}

static err_t quic_service_tx(skin_quic_ctx_t *ctx) {
    err_t rc = E__SUCCESS;
    uint8_t pkt[QUIC_MAX_PKT];
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;

    ngtcp2_path_storage_zero(&ps);

    if (ctx->stream_id == -1) return E__SUCCESS;

    for (;;) {
        /* Find first data that hasn't been sent to ngtcp2 yet */
        quic_pending_data_t *p = ctx->pending_data;
        while (p && p->offset + p->len <= ctx->stream_sent_offset) {
            p = p->next;
        }

        if (!p) break;

        size_t stream_offset = (size_t)(ctx->stream_sent_offset - p->offset);
        ngtcp2_vec vec = {p->buf + stream_offset, p->len - stream_offset};
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_tstamp ts = quic_now();

        pthread_mutex_lock(&ctx->conn_mutex);
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(ctx->conn, &ps.path, &pi,
                                                    pkt, sizeof(pkt),
                                                    &pdatalen,
                                                    NGTCP2_WRITE_STREAM_FLAG_MORE,
                                                    ctx->stream_id,
                                                    &vec, 1, ts);
        pthread_mutex_unlock(&ctx->conn_mutex);

        if (pdatalen > 0) {
            ctx->stream_sent_offset += (uint64_t)pdatalen;
        }

        if (0 > n) {
            if (NGTCP2_ERR_WRITE_MORE == (int)n) {
                continue;
            }
            if (NGTCP2_ERR_STREAM_DATA_BLOCKED == (int)n ||
                NGTCP2_ERR_STREAM_NOT_FOUND == (int)n) {
                break; /* Flow control blocked, stop for now */
            }
            LOG_WARN("ngtcp2_conn_writev_stream: %s", ngtcp2_strerror((int)n));
            rc = E__NET__SOCKET_CONNECT_FAILED;
            goto l_cleanup;
        }

        if (0 == n) {
            break; /* No more packets can be written now */
        }

        ssize_t sent = send(ctx->udp_fd, pkt, (size_t)n, 0);
        if (0 > sent && EAGAIN != errno && EWOULDBLOCK != errno) {
            LOG_WARN("QUIC stream sendto: %s", strerror(errno));
            rc = E__NET__SOCKET_CONNECT_FAILED;
            goto l_cleanup;
        }
    }

    quic_flush_send(ctx);

l_cleanup:
    return rc;
}

/* ── I/O thread ──────────────────────────────────────────────────────────────── */

static void *quic_io_thread(void *arg) {
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)arg;

    while (ctx->io_running) {
        ngtcp2_tstamp now = quic_now();
        ngtcp2_tstamp expiry;

        pthread_mutex_lock(&ctx->conn_mutex);
        expiry = ngtcp2_conn_get_expiry(ctx->conn);
        pthread_mutex_unlock(&ctx->conn_mutex);

        int timeout_ms = 1000;
        if (UINT64_MAX != expiry) {
            if (expiry <= now) {
                timeout_ms = 0;
            } else {
                uint64_t diff_ms = (expiry - now) / 1000000ULL;
                timeout_ms = (int)(diff_ms < 1000 ? diff_ms : 1000);
            }
        }

        struct pollfd fds[3];
        fds[0].fd     = ctx->udp_fd;
        fds[0].events = POLLIN;
        fds[1].fd     = ctx->tx_pipe[0];
        fds[1].events = POLLIN;
        fds[2].fd     = ctx->rx_pipe[1];
        fds[2].events = (ctx->stream_buf_len >= QUIC_PIPE_MSG_HDR) ? POLLOUT : 0;

        poll(fds, 3, timeout_ms);

        now = quic_now();

        /* If rx_pipe is writable and we have data, drain it */
        if (fds[2].revents & POLLOUT) {
            quic_drain_stream_buf(ctx);
        }

        /* Handle timer expiry */
        pthread_mutex_lock(&ctx->conn_mutex);
        if (ngtcp2_conn_get_expiry(ctx->conn) <= now) {
            ngtcp2_conn_handle_expiry(ctx->conn, now);
        }
        pthread_mutex_unlock(&ctx->conn_mutex);
        quic_flush_send(ctx);
        quic_service_tx(ctx);

        /* Receive incoming UDP datagram */
        if (fds[0].revents & POLLIN) {
            uint8_t pkt[65536];
            for (;;) {
                struct sockaddr_storage addr;
                socklen_t addrlen = sizeof(addr);
                ssize_t n = recvfrom(ctx->udp_fd, pkt, sizeof(pkt), 0,
                                      (struct sockaddr *)&addr, &addrlen);
                if (n > 0) {
                    ngtcp2_path path;
                    ngtcp2_addr_init(&path.local,
                                      (struct sockaddr *)&ctx->local_addr,
                                      ctx->local_addrlen);
                    ngtcp2_addr_init(&path.remote,
                                      (struct sockaddr *)&addr, addrlen);
                    ngtcp2_pkt_info pi = {0};

                    pthread_mutex_lock(&ctx->conn_mutex);
                    int rv = ngtcp2_conn_read_pkt(ctx->conn, &path, &pi,
                                                   pkt, (size_t)n, now);
                    pthread_mutex_unlock(&ctx->conn_mutex);

                    if (0 != rv && NGTCP2_ERR_DRAINING != rv) {
                        LOG_WARN("ngtcp2_conn_read_pkt: %s", ngtcp2_strerror(rv));
                        if (ngtcp2_err_is_fatal(rv)) {
                            ctx->io_running = 0;
                            break;
                        }
                    }
                    quic_flush_send(ctx);
                } else {
                    break;
                }
            }
            quic_service_tx(ctx);
        }

        /* Send data from tx_pipe → QUIC stream */
        if (fds[1].revents & POLLIN) {
            uint8_t buf[16384];
            for (;;) {
                ssize_t n = read(ctx->tx_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (ctx->tx_pipe_buf_len + (size_t)n > ctx->tx_pipe_buf_cap) {
                        size_t new_cap = ctx->tx_pipe_buf_cap + (size_t)n + QUIC_RX_BUF_INIT;
                        uint8_t *nb = realloc(ctx->tx_pipe_buf, new_cap);
                        if (nb) {
                            ctx->tx_pipe_buf = nb;
                            ctx->tx_pipe_buf_cap = new_cap;
                        }
                    }
                    if (ctx->tx_pipe_buf_len + (size_t)n <= ctx->tx_pipe_buf_cap) {
                        memcpy(ctx->tx_pipe_buf + ctx->tx_pipe_buf_len, buf, (size_t)n);
                        ctx->tx_pipe_buf_len += (size_t)n;
                    }
                } else {
                    break;
                }
            }
            quic_drain_tx_pipe_buf(ctx);
            quic_service_tx(ctx);
        }
    }

    return NULL;
}

/* ── Transport allocation ────────────────────────────────────────────────────── */

static skin_quic_ctx_t *quic_alloc_ctx(void) {
    skin_quic_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        return NULL;
    }
    ctx->stream_id = -1;
    ctx->rx_pipe[0] = ctx->rx_pipe[1] = -1;
    ctx->tx_pipe[0] = ctx->tx_pipe[1] = -1;
    ctx->udp_fd = -1;

    ctx->stream_buf_cap = QUIC_RX_BUF_INIT;
    ctx->stream_buf = malloc(ctx->stream_buf_cap);
    if (NULL == ctx->stream_buf) {
        free(ctx);
        return NULL;
    }

    ctx->tx_pipe_buf_cap = QUIC_RX_BUF_INIT;
    ctx->tx_pipe_buf = malloc(ctx->tx_pipe_buf_cap);
    if (NULL == ctx->tx_pipe_buf) {
        free(ctx->stream_buf);
        free(ctx);
        return NULL;
    }

    pthread_mutex_init(&ctx->conn_mutex, NULL);



    return ctx;
}

static void quic_free_ctx(skin_quic_ctx_t *ctx) {
    if (NULL == ctx) {
        return;
    }
    ctx->io_running = 0;
    if (ctx->rx_pipe[0] >= 0) { close(ctx->rx_pipe[0]); }
    if (ctx->rx_pipe[1] >= 0) { close(ctx->rx_pipe[1]); }
    if (ctx->tx_pipe[0] >= 0) { close(ctx->tx_pipe[0]); }
    if (ctx->tx_pipe[1] >= 0) { close(ctx->tx_pipe[1]); }
    if (ctx->udp_fd >= 0)     { close(ctx->udp_fd); }
    if (NULL != ctx->conn) {
        ngtcp2_crypto_picotls_deconfigure_session(&ctx->cptls);
        if (NULL != ctx->cptls.ptls) {
            ptls_free(ctx->cptls.ptls);
        }
        ngtcp2_conn_del(ctx->conn);
    }

    /* Free all pending stream data */
    quic_pending_data_t *p = ctx->pending_data;
    while (p) {
        quic_pending_data_t *next = p->next;
        free(p->buf);
        free(p);
        p = next;
    }

    /* Free all stable crypto buffers */
    quic_crypto_data_t *c = ctx->crypto_data;
    while (c) {
        quic_crypto_data_t *next = c->next;
        free(c->buf);
        free(c);
        c = next;
    }

    free(ctx->stream_buf);
    free(ctx->tx_pipe_buf);
    pthread_mutex_destroy(&ctx->conn_mutex);
    free(ctx);
}

static err_t quic_start_io_thread(skin_quic_ctx_t *ctx) {
    err_t rc = E__SUCCESS;

    ctx->io_running = 1;
    if (0 != pthread_create(&ctx->io_thread, NULL, quic_io_thread, ctx)) {
        ctx->io_running = 0;
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

l_cleanup:
    return rc;
}

static transport_t *quic_alloc_transport(skin_quic_ctx_t *ctx,
                                          int is_incoming,
                                          const char *ip, int port,
                                          const skin_ops_t *skin) {
    transport_t *t = TRANSPORT__alloc_base(ctx->udp_fd, skin);
    if (NULL == t) {
        return NULL;
    }
    t->is_incoming = is_incoming;
    if (NULL != ip) {
        strncpy(t->client_ip, ip, INET_ADDRSTRLEN - 1);
        t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    }
    t->client_port = port;
    t->skin_ctx = ctx;
    return t;
}

/* ── UDP socket helpers ──────────────────────────────────────────────────────── */

static int quic_udp_socket_bind(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);

    if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "*")) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, ip, &sa.sin_addr);
    }

    if (0 != bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Self-signed certificate generation ─────────────────────────────────────── */

#ifdef SKIN_QUIC_USE_MBEDTLS
static int quic_gen_cert(skin_quic_listener_t *l) {
    int ret;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "ganon_quic_gen_cert";
    unsigned char cert_buf[4096];
    int cert_len;

    mbedtls_x509write_crt_init(&crt);
    mbedtls_pk_init(&l->pkey);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)pers, strlen(pers))) != 0) {
        goto cleanup;
    }

    /* Generate P-256 key pair */
    if ((ret = mbedtls_pk_setup(&l->pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0) {
        goto cleanup;
    }
    if ((ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(l->pkey),
                                  mbedtls_ctr_drbg_random, &ctr_drbg)) != 0) {
        goto cleanup;
    }

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    /* Serial number = 1 */
    unsigned char serial[] = { 0x01 };
    mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));

    mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20301231235959");
    mbedtls_x509write_crt_set_subject_name(&crt, "CN=ganon");
    mbedtls_x509write_crt_set_issuer_name(&crt, "CN=ganon");
    mbedtls_x509write_crt_set_subject_key(&crt, &l->pkey);
    mbedtls_x509write_crt_set_issuer_key(&crt, &l->pkey);

    cert_len = mbedtls_x509write_crt_der(&crt, cert_buf, sizeof(cert_buf),
                                         mbedtls_ctr_drbg_random, &ctr_drbg);
    if (cert_len < 0) {
        ret = cert_len;
        goto cleanup;
    }

    l->cert_der = malloc((size_t)cert_len);
    if (l->cert_der == NULL) {
        ret = -1;
        goto cleanup;
    }
    /* mbedtls_x509write_crt_der writes at the END of the buffer */
    memcpy(l->cert_der, cert_buf + sizeof(cert_buf) - (size_t)cert_len, (size_t)cert_len);
    l->cert_der_len = (size_t)cert_len;

    ret = 0;

cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

static int mbedtls_sign_certificate(ptls_sign_certificate_t *_self, ptls_t *tls, ptls_async_job_t **async,
                                    uint16_t *selected_algorithm, ptls_buffer_t *outbuf, ptls_iovec_t input,
                                    const uint16_t *algorithms, size_t num_algorithms) {
    ptls_mbedtls_sign_certificate_t *self = (ptls_mbedtls_sign_certificate_t *)_self;
    size_t i;
    int ret;

    (void)tls;
    (void)async;

    /* We only support ECDSA with SHA-256 (P-256) */
    int found = 0;
    for (i = 0; i < num_algorithms; i++) {
        if (algorithms[i] == PTLS_SIGNATURE_ECDSA_SECP256R1_SHA256) {
            found = 1;
            break;
        }
    }
    if (!found) return PTLS_ALERT_HANDSHAKE_FAILURE;

    *selected_algorithm = PTLS_SIGNATURE_ECDSA_SECP256R1_SHA256;

    size_t sig_max_len = MBEDTLS_PK_SIGNATURE_MAX_SIZE;
    if ((ret = ptls_buffer_reserve(outbuf, sig_max_len)) != 0) return ret;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

    /* Calculate hash of input */
    unsigned char hash[32];
    size_t hash_out_len;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, input.base, input.len, hash, sizeof(hash), &hash_out_len);
    if (status != PSA_SUCCESS) {
        ret = PTLS_ERROR_LIBRARY;
        goto done;
    }

    size_t sig_len;
    ret = mbedtls_pk_sign(self->key, MBEDTLS_MD_SHA256, hash, hash_out_len,
                          outbuf->base + outbuf->off, sig_max_len, &sig_len,
                          mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret == 0) {
        outbuf->off += sig_len;
    } else {
        ret = PTLS_ERROR_LIBRARY;
    }

done:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

#else
static int quic_gen_cert(skin_quic_listener_t *l) {
    int rv = -1;

    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (NULL == pctx) {
        return -1;
    }
    if (1 != EVP_PKEY_keygen_init(pctx)) {
        goto done;
    }
    if (1 != EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1)) {
        goto done;
    }
    if (1 != EVP_PKEY_keygen(pctx, &l->pkey)) {
        goto done;
    }
    EVP_PKEY_CTX_free(pctx);
    pctx = NULL;

    X509 *cert = X509_new();
    if (NULL == cert) {
        goto done;
    }
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 3650L * 24 * 3600);
    X509_set_pubkey(cert, l->pkey);

    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (const unsigned char *)"ganon", -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509_sign(cert, l->pkey, EVP_sha256());

    int dlen = i2d_X509(cert, NULL);
    if (dlen <= 0) {
        X509_free(cert);
        goto done;
    }
    l->cert_der = malloc((size_t)dlen);
    if (NULL == l->cert_der) {
        X509_free(cert);
        goto done;
    }
    uint8_t *p = l->cert_der;
    i2d_X509(cert, &p);
    l->cert_der_len = (size_t)dlen;
    X509_free(cert);

    rv = 0;
done:
    if (NULL != pctx) {
        EVP_PKEY_CTX_free(pctx);
    }
    return rv;
}
#endif

/* ── listener_create ─────────────────────────────────────────────────────────── */

static err_t quic_listener_create(const addr_t *addr,
                                   skin_listener_t **out_listener,
                                   int *out_listen_fd) {
    err_t rc = E__SUCCESS;
    skin_quic_listener_t *l = NULL;
    int fd = -1;

    VALIDATE_ARGS(addr, out_listener, out_listen_fd);
    *out_listener  = NULL;
    *out_listen_fd = -1;

    l = calloc(1, sizeof(*l));
    FAIL_IF(NULL == l, E__INVALID_ARG_NULL_POINTER);

    l->addr = *addr;

#ifdef SKIN_QUIC_USE_MBEDTLS
    psa_crypto_init();
#endif

    if (0 != quic_gen_cert(l)) {
        LOG_ERROR("QUIC: failed to generate self-signed certificate");
        free(l);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

#ifdef SKIN_QUIC_USE_MBEDTLS
    l->sign_cert.super.cb = mbedtls_sign_certificate;
    l->sign_cert.key      = &l->pkey;
#else
    if (0 != ptls_openssl_init_sign_certificate(&l->sign_cert, l->pkey)) {
        LOG_ERROR("QUIC: ptls_openssl_init_sign_certificate failed");
        free(l->cert_der);
        EVP_PKEY_free(l->pkey);
        free(l);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
#endif

    l->cert_iov[0].base = l->cert_der;
    l->cert_iov[0].len  = l->cert_der_len;

#ifdef SKIN_QUIC_USE_MBEDTLS
    l->key_ex[0] = &ptls_mbedtls_secp256r1;
    l->key_ex[1] = NULL;
    l->ciphers[0] = &ptls_mbedtls_aes128gcmsha256;
    l->ciphers[1] = &ptls_mbedtls_aes256gcmsha384;
    l->ciphers[2] = NULL;
    l->tls_ctx.random_bytes      = ptls_mbedtls_random_bytes;
#else
    l->key_ex[0] = &ptls_openssl_secp256r1;
    l->key_ex[1] = NULL;
    l->ciphers[0] = &ptls_openssl_aes128gcmsha256;
    l->ciphers[1] = &ptls_openssl_aes256gcmsha384;
    l->ciphers[2] = NULL;
    l->tls_ctx.random_bytes      = ptls_openssl_random_bytes;
#endif

    l->tls_ctx.get_time          = &ptls_get_time;
    l->tls_ctx.key_exchanges     = l->key_ex;
    l->tls_ctx.cipher_suites     = l->ciphers;
    l->tls_ctx.certificates.list = l->cert_iov;
    l->tls_ctx.certificates.count = 1;
    l->tls_ctx.sign_certificate  = &l->sign_cert.super;

    ngtcp2_crypto_picotls_configure_server_context(&l->tls_ctx);

    fd = quic_udp_socket_bind(addr->ip, addr->port);
    if (fd < 0) {
        LOG_ERROR("QUIC: failed to bind UDP socket on %s:%d: %s",
                  addr->ip, addr->port, strerror(errno));
#ifdef SKIN_QUIC_USE_MBEDTLS
        mbedtls_pk_free(&l->pkey);
#else
        ptls_openssl_dispose_sign_certificate(&l->sign_cert);
        EVP_PKEY_free(l->pkey);
#endif
        free(l->cert_der);
        free(l);
        FAIL(E__NET__SOCKET_BIND_FAILED);
    }

    /* Non-blocking so the accept loop can poll io_running */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    l->listen_fd   = fd;
    *out_listen_fd = fd;
    *out_listener  = (skin_listener_t *)l;

    LOG_INFO("QUIC listener on %s:%d (udp)", addr->ip, addr->port);

l_cleanup:
    return rc;
}

/* ── listener_accept ─────────────────────────────────────────────────────────── */

static err_t quic_listener_accept(skin_listener_t *sl,
                                   transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_quic_listener_t *l = (skin_quic_listener_t *)sl;
    skin_quic_ctx_t *ctx = NULL;
    transport_t *t = NULL;

    VALIDATE_ARGS(l, out_transport);
    *out_transport = NULL;

    /* Peek at an incoming datagram */
    uint8_t pkt[65536];
    struct sockaddr_storage remote_addr;
    socklen_t remote_addrlen = sizeof(remote_addr);
    ssize_t n = recvfrom(l->listen_fd, pkt, sizeof(pkt), MSG_DONTWAIT,
                          (struct sockaddr *)&remote_addr, &remote_addrlen);
    if (n < 0) {
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);  /* EAGAIN / transient */
    }
    LOG_INFO("QUIC: listener received %zd bytes, pkt[0]=0x%x", n, pkt[0]);

    /* Quick check: is this a QUIC Long Header packet? */
    if (n < (ssize_t)NGTCP2_MAX_UDP_PAYLOAD_SIZE && (pkt[0] & 0x80) == 0) {
        /* Not a valid initial size and not a long header; ignore */
        LOG_WARN("QUIC: Ignoring non-compliant Initial packet");
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    /* Get local address */
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen = sizeof(local_addr);
    getsockname(l->listen_fd, (struct sockaddr *)&local_addr, &local_addrlen);

    /* Create a new UDP socket for this connection (SO_REUSEPORT) */
    int conn_fd = quic_udp_socket_bind(l->addr.ip, l->addr.port);
    if (conn_fd < 0) {
        LOG_ERROR("QUIC: failed to create per-connection socket: %s",
                  strerror(errno));
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    /* Connect to remote so only that peer's packets arrive here */
    if (0 != connect(conn_fd, (struct sockaddr *)&remote_addr, remote_addrlen)) {
        LOG_WARN("QUIC: connect to remote failed: %s", strerror(errno));
        close(conn_fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    ctx = quic_alloc_ctx();
    if (NULL == ctx) {
        close(conn_fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    ctx->udp_fd         = conn_fd;
    ctx->is_server      = 1;
    ctx->remote_addr    = remote_addr;
    ctx->remote_addrlen = remote_addrlen;
    ctx->local_addr     = local_addr;
    ctx->local_addrlen  = local_addrlen;

    ctx->conn_ref.get_conn  = quic_get_conn;
    ctx->conn_ref.user_data = ctx;

    /* Create TLS session */
    ngtcp2_crypto_picotls_ctx_init(&ctx->cptls);
    ctx->cptls.handshake_properties.additional_extensions = ctx->exts;
    ctx->cptls.ptls = ptls_new(&l->tls_ctx, 1 /* is_server */);
    if (NULL == ctx->cptls.ptls) {
        LOG_ERROR("QUIC: ptls_new failed");
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    *ptls_get_data_ptr(ctx->cptls.ptls) = &ctx->conn_ref;

    if (0 != ngtcp2_crypto_picotls_configure_server_session(&ctx->cptls)) {
        LOG_ERROR("QUIC: configure_server_session failed");
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    /* Build ngtcp2 server connection */
    ngtcp2_path path;
    ngtcp2_addr_init(&path.local,  (struct sockaddr *)&local_addr, local_addrlen);
    ngtcp2_addr_init(&path.remote, (struct sockaddr *)&remote_addr, remote_addrlen);

    /* Decode Initial packet header to extract DCID */
    ngtcp2_version_cid vcid;
    int rv = ngtcp2_pkt_decode_version_cid(&vcid, pkt, (size_t)n, 8);
    if (0 != rv) {
        LOG_WARN("QUIC: ngtcp2_pkt_decode_version_cid: %s", ngtcp2_strerror(rv));
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    ngtcp2_cid scid;
    quic_random(scid.data, 8);
    scid.datalen = 8;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = quic_now();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = 8;
    params.initial_max_streams_uni  = 0;
    params.initial_max_data         = 256 * 1024;
    params.initial_max_stream_data_bidi_local  = 128 * 1024;
    params.initial_max_stream_data_bidi_remote = 128 * 1024;

    params.original_dcid_present = 1;
    memcpy(params.original_dcid.data, vcid.dcid, vcid.dcidlen);
    params.original_dcid.datalen = vcid.dcidlen;



    ngtcp2_cid dcid;
    memcpy(dcid.data, vcid.scid, vcid.scidlen);
    dcid.datalen = vcid.scidlen;

    rv = ngtcp2_conn_server_new(&ctx->conn, &dcid, &scid, &path,
                                 vcid.version, &g_server_callbacks,
                                 &settings, &params, NULL, ctx);
    if (0 != rv) {
        LOG_ERROR("QUIC: ngtcp2_conn_server_new: %s", ngtcp2_strerror(rv));
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    ngtcp2_conn_set_tls_native_handle(ctx->conn, &ctx->cptls);

    /* Process the received Initial packet on the NEW connected socket */
    {
        ngtcp2_pkt_info pi = {0};
        pthread_mutex_lock(&ctx->conn_mutex);
        rv = ngtcp2_conn_read_pkt(ctx->conn, &path, &pi, pkt, (size_t)n,
                                   quic_now());
        pthread_mutex_unlock(&ctx->conn_mutex);
        if (0 != rv && NGTCP2_ERR_RETRY != rv) {
            LOG_WARN("QUIC: read_pkt (Initial): %s", ngtcp2_strerror(rv));
        }
        quic_flush_send(ctx);
    }

    /* Perform handshake synchronously (up to 5 seconds) */
    {
        struct pollfd pfd = {ctx->udp_fd, POLLIN, 0};
        ngtcp2_tstamp start_ts = quic_now();
        ngtcp2_tstamp end_ts = start_ts + 5 * NGTCP2_SECONDS;

        while (!ctx->handshake_done) {
            ngtcp2_tstamp now = quic_now();
            if (now >= end_ts) break;

            ngtcp2_tstamp expiry;
            pthread_mutex_lock(&ctx->conn_mutex);
            expiry = ngtcp2_conn_get_expiry(ctx->conn);
            pthread_mutex_unlock(&ctx->conn_mutex);

            int ms = 100;
            if (UINT64_MAX != expiry) {
                if (expiry <= now) {
                    ms = 0;
                } else {
                    uint64_t d = (expiry - now) / 1000000ULL;
                    ms = (int)(d < 100 ? d : 100);
                }
            }

            poll(&pfd, 1, ms);

            if (pfd.revents & POLLIN) {
                uint8_t buf[65536];
                struct sockaddr_storage addr2;
                socklen_t al2 = sizeof(addr2);
                ssize_t m = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0,
                                      (struct sockaddr *)&addr2, &al2);
                if (m > 0) {
                    ngtcp2_path path2;
                    ngtcp2_addr_init(&path2.local,
                                      (struct sockaddr *)&local_addr, local_addrlen);
                    ngtcp2_addr_init(&path2.remote,
                                      (struct sockaddr *)&addr2, al2);
                    ngtcp2_pkt_info pi2 = {0};
                    pthread_mutex_lock(&ctx->conn_mutex);
                    ngtcp2_conn_read_pkt(ctx->conn, &path2, &pi2, buf, (size_t)m,
                                          quic_now());
                    pthread_mutex_unlock(&ctx->conn_mutex);
                    quic_flush_send(ctx);
                }
            } else {
                /* Handle timer */
                ngtcp2_tstamp now = quic_now();
                pthread_mutex_lock(&ctx->conn_mutex);
                if (ngtcp2_conn_get_expiry(ctx->conn) <= now) {
                    ngtcp2_conn_handle_expiry(ctx->conn, now);
                }
                pthread_mutex_unlock(&ctx->conn_mutex);
                quic_flush_send(ctx);
            }
        }

        if (!ctx->handshake_done) {
            LOG_WARN("QUIC: server handshake timed out");
            quic_free_ctx(ctx);
            FAIL(E__NET__SOCKET_ACCEPT_FAILED);
        }
    }

    /* Create pipes */
    if (0 != pipe(ctx->rx_pipe) || 0 != pipe(ctx->tx_pipe)) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    quic_set_nonblocking(ctx->rx_pipe[0]);
    quic_set_nonblocking(ctx->rx_pipe[1]);
    quic_set_nonblocking(ctx->tx_pipe[0]);
    quic_set_nonblocking(ctx->tx_pipe[1]);

    /* Start I/O thread */
    rc = quic_start_io_thread(ctx);
    if (E__SUCCESS != rc) {
        quic_free_ctx(ctx);
        goto l_cleanup;
    }

    /* Get remote IP/port for the transport struct */
    char remote_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    int remote_port_n = 0;
    if (AF_INET == remote_addr.ss_family) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&remote_addr;
        inet_ntop(AF_INET, &sin->sin_addr, remote_ip, sizeof(remote_ip));
        remote_port_n = ntohs(sin->sin_port);
    }

    t = quic_alloc_transport(ctx, 1, remote_ip, remote_port_n,
                               SKIN_UDP_QUIC__ops());
    if (NULL == t) {
        ctx->io_running = 0;
        pthread_join(ctx->io_thread, NULL);
        quic_free_ctx(ctx);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("QUIC: accepted connection from %s:%d", remote_ip, remote_port_n);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* ── listener_destroy ────────────────────────────────────────────────────────── */

static void quic_listener_destroy(skin_listener_t *sl) {
    skin_quic_listener_t *l = (skin_quic_listener_t *)sl;
    if (NULL == l) {
        return;
    }
    if (l->listen_fd >= 0) {
        close(l->listen_fd);
    }
#ifdef SKIN_QUIC_USE_MBEDTLS
    mbedtls_pk_free(&l->pkey);
#else
    ptls_openssl_dispose_sign_certificate(&l->sign_cert);
    EVP_PKEY_free(l->pkey);
#endif
    free(l->cert_der);
    free(l);
}

/* ── connect ─────────────────────────────────────────────────────────────────── */

static err_t quic_connect(const char *ip, int port, int connect_timeout_sec,
                            transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_quic_ctx_t *ctx = NULL;
    transport_t *t = NULL;

    VALIDATE_ARGS(ip, out_transport);
    *out_transport = NULL;

    ctx = quic_alloc_ctx();
    FAIL_IF(NULL == ctx, E__INVALID_ARG_NULL_POINTER);

    ctx->is_server = 0;

    /* Create UDP socket and connect to remote */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    ctx->udp_fd = fd;

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port   = htons((uint16_t)port);

    if (0 == inet_pton(AF_INET, ip, &remote.sin_addr)) {
        struct hostent *he = gethostbyname(ip);
        if (NULL == he) {
            quic_free_ctx(ctx);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        memcpy(&remote.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    if (0 != connect(fd, (struct sockaddr *)&remote, sizeof(remote))) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    /* Get local address */
    ctx->local_addrlen = sizeof(ctx->local_addr);
    getsockname(fd, (struct sockaddr *)&ctx->local_addr, &ctx->local_addrlen);

    memcpy(&ctx->remote_addr, &remote, sizeof(remote));
    ctx->remote_addrlen = sizeof(remote);

#ifdef SKIN_QUIC_USE_MBEDTLS
    psa_crypto_init();
#endif

    /* Set up client-side TLS context */
#ifdef SKIN_QUIC_USE_MBEDTLS
    ctx->key_ex_c[0] = &ptls_mbedtls_secp256r1;
    ctx->key_ex_c[1] = NULL;
    ctx->ciphers_c[0] = &ptls_mbedtls_aes128gcmsha256;
    ctx->ciphers_c[1] = &ptls_mbedtls_aes256gcmsha384;
    ctx->ciphers_c[2] = NULL;
    ctx->tls_ctx_client.random_bytes = ptls_mbedtls_random_bytes;
#else
    ctx->key_ex_c[0] = &ptls_openssl_secp256r1;
    ctx->key_ex_c[1] = NULL;
    ctx->ciphers_c[0] = &ptls_openssl_aes128gcmsha256;
    ctx->ciphers_c[1] = &ptls_openssl_aes256gcmsha384;
    ctx->ciphers_c[2] = NULL;
    ctx->tls_ctx_client.random_bytes = ptls_openssl_random_bytes;
#endif

    ctx->tls_ctx_client.get_time      = &ptls_get_time;
    ctx->tls_ctx_client.key_exchanges = ctx->key_ex_c;
    ctx->tls_ctx_client.cipher_suites = ctx->ciphers_c;
    /* no_certificate_verify: skip server cert validation (ganon uses node IDs) */
    ctx->tls_ctx_client.verify_certificate = NULL;

    ngtcp2_crypto_picotls_configure_client_context(&ctx->tls_ctx_client);

    ctx->conn_ref.get_conn  = quic_get_conn;
    ctx->conn_ref.user_data = ctx;

    ngtcp2_crypto_picotls_ctx_init(&ctx->cptls);
    ctx->cptls.handshake_properties.additional_extensions = ctx->exts;
    ctx->cptls.ptls = ptls_new(&ctx->tls_ctx_client, 0 /* client */);
    if (NULL == ctx->cptls.ptls) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    *ptls_get_data_ptr(ctx->cptls.ptls) = &ctx->conn_ref;
    ptls_set_server_name(ctx->cptls.ptls, ip, strlen(ip));

    /* Set ALPN in handshake properties (must be done before configure_client_session) */
    ctx->alpn_list[0] = ptls_iovec_init("ganon", 5);
    ctx->cptls.handshake_properties.client.negotiated_protocols.list  = ctx->alpn_list;
    ctx->cptls.handshake_properties.client.negotiated_protocols.count = 1;

    /* Generate DCID (server's connection ID) and SCID */
    ngtcp2_cid dcid, scid;
    quic_random(dcid.data, 8);
    dcid.datalen = 8;
    quic_random(scid.data, 8);
    scid.datalen = 8;

    ngtcp2_path path;
    ngtcp2_addr_init(&path.local,  (struct sockaddr *)&ctx->local_addr, ctx->local_addrlen);
    ngtcp2_addr_init(&path.remote, (struct sockaddr *)&remote, sizeof(remote));

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = quic_now();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = 8;
    params.initial_max_data         = 256 * 1024;
    params.initial_max_stream_data_bidi_local  = 128 * 1024;
    params.initial_max_stream_data_bidi_remote = 128 * 1024;



    int rv = ngtcp2_conn_client_new(&ctx->conn, &dcid, &scid, &path,
                                     NGTCP2_PROTO_VER_V1, &g_client_callbacks,
                                     &settings, &params, NULL, ctx);
    if (0 != rv) {
        LOG_ERROR("QUIC: ngtcp2_conn_client_new: %s", ngtcp2_strerror(rv));
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    /* Now configure the session (conn must exist) */
    if (0 != ngtcp2_crypto_picotls_configure_client_session(&ctx->cptls, ctx->conn)) {
        LOG_ERROR("QUIC: configure_client_session failed");
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    ngtcp2_conn_set_tls_native_handle(ctx->conn, &ctx->cptls);

    /* Handshake loop (synchronous, up to connect_timeout_sec) */
    {
        /* Send Initial packet */
        quic_flush_send(ctx);

        struct pollfd pfd = {ctx->udp_fd, POLLIN, 0};
        ngtcp2_tstamp start_ts = quic_now();
        ngtcp2_tstamp end_ts = start_ts + (ngtcp2_tstamp)connect_timeout_sec * NGTCP2_SECONDS;

        while (!ctx->handshake_done) {
            ngtcp2_tstamp now = quic_now();
            if (now >= end_ts) break;

            ngtcp2_tstamp expiry;
            pthread_mutex_lock(&ctx->conn_mutex);
            expiry = ngtcp2_conn_get_expiry(ctx->conn);
            pthread_mutex_unlock(&ctx->conn_mutex);

            int ms = 100;
            if (UINT64_MAX != expiry) {
                if (expiry <= now) {
                    ms = 0;
                } else {
                    uint64_t d = (expiry - now) / 1000000ULL;
                    ms = (int)(d < 100 ? d : 100);
                }
            }

            poll(&pfd, 1, ms);

            if (pfd.revents & POLLIN) {
                uint8_t buf[65536];
                struct sockaddr_storage addr;
                socklen_t al = sizeof(addr);
                ssize_t m = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0,
                                      (struct sockaddr *)&addr, &al);
                if (m > 0) {
                    ngtcp2_path path2;
                    ngtcp2_addr_init(&path2.local,
                                      (struct sockaddr *)&ctx->local_addr,
                                      ctx->local_addrlen);
                    ngtcp2_addr_init(&path2.remote,
                                      (struct sockaddr *)&addr, al);
                    ngtcp2_pkt_info pi = {0};
                    pthread_mutex_lock(&ctx->conn_mutex);
                    ngtcp2_conn_read_pkt(ctx->conn, &path2, &pi, buf, (size_t)m,
                                          quic_now());
                    pthread_mutex_unlock(&ctx->conn_mutex);
                }
            }

            /* Handle timer */
            now = quic_now();
            pthread_mutex_lock(&ctx->conn_mutex);
            if (ngtcp2_conn_get_expiry(ctx->conn) <= now) {
                ngtcp2_conn_handle_expiry(ctx->conn, now);
            }
            pthread_mutex_unlock(&ctx->conn_mutex);
            quic_flush_send(ctx);
        }

        if (!ctx->handshake_done) {
            LOG_WARN("QUIC: client handshake timed out to %s:%d", ip, port);
            quic_free_ctx(ctx);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }

        /* Wait for stream to be opened (client opens it after handshake) */
        int stream_wait = 50;
        while (--stream_wait > 0 && -1 == ctx->stream_id) {
            quic_flush_send(ctx);
            usleep(20000);
        }
        if (-1 == ctx->stream_id) {
            LOG_WARN("QUIC: stream not opened after handshake");
            quic_free_ctx(ctx);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
    }

    /* Create pipes and start I/O thread */
    if (0 != pipe(ctx->rx_pipe) || 0 != pipe(ctx->tx_pipe)) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    quic_set_nonblocking(ctx->rx_pipe[0]);
    quic_set_nonblocking(ctx->rx_pipe[1]);
    quic_set_nonblocking(ctx->tx_pipe[0]);
    quic_set_nonblocking(ctx->tx_pipe[1]);

    rc = quic_start_io_thread(ctx);
    if (E__SUCCESS != rc) {
        quic_free_ctx(ctx);
        goto l_cleanup;
    }

    t = quic_alloc_transport(ctx, 0, ip, port, SKIN_UDP_QUIC__ops());
    if (NULL == t) {
        ctx->io_running = 0;
        pthread_join(ctx->io_thread, NULL);
        quic_free_ctx(ctx);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("QUIC: connected to %s:%d", ip, port);
    *out_transport = t;

l_cleanup:
    return rc;
}

static ssize_t quic_blocking_read(int fd, void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = read(fd, (uint8_t *)buf + total, count - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n == 0) {
            return (ssize_t)total;
        } else {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                struct pollfd pfd = {fd, POLLIN, 0};
                poll(&pfd, 1, -1);
                continue;
            }
            return -1;
        }
    }
    return (ssize_t)total;
}

static ssize_t quic_blocking_write(int fd, const void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = write(fd, (const uint8_t *)buf + total, count - total);
        if (n > 0) {
            total += (size_t)n;
        } else {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                struct pollfd pfd = {fd, POLLOUT, 0};
                poll(&pfd, 1, -1);
                continue;
            }
            return -1;
        }
    }
    return (ssize_t)total;
}

/* ── send_msg ─────────────────────────────────────────────────────────────────── */

static err_t quic_send_msg(transport_t *t, const protocol_msg_t *msg,
                            const uint8_t *data) {
    err_t rc = E__SUCCESS;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)t->skin_ctx;

    VALIDATE_ARGS(t, msg);

    size_t payload_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0) {
        payload_len += msg->data_length;
    }

    uint8_t *frame = malloc(QUIC_PIPE_MSG_HDR + payload_len);
    FAIL_IF(NULL == frame, E__INVALID_ARG_NULL_POINTER);

    /* 4-byte length prefix */
    uint32_t net_len = htonl((uint32_t)payload_len);
    memcpy(frame, &net_len, 4);

    /* Serialize protocol message */
    size_t written = 0;
    rc = PROTOCOL__serialize(msg, data, frame + QUIC_PIPE_MSG_HDR,
                              payload_len, &written);
    if (E__SUCCESS != rc) {
        free(frame);
        goto l_cleanup;
    }

    /* Write to tx_pipe for the I/O thread to send */
    size_t total = QUIC_PIPE_MSG_HDR + written;
    ssize_t wr = quic_blocking_write(ctx->tx_pipe[1], frame, total);
    free(frame);

    if (wr != (ssize_t)total) {
        LOG_WARN("QUIC send_msg: short write to tx_pipe: %zd != %zu", wr, total);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

l_cleanup:
    return rc;
}

/* ── recv_msg ─────────────────────────────────────────────────────────────────── */

static err_t quic_recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)t->skin_ctx;
    uint8_t hdr[QUIC_PIPE_MSG_HDR];
    uint8_t *frame = NULL;

    VALIDATE_ARGS(t, msg, data);
    *data = NULL;

    /* Block reading the 4-byte length prefix from rx_pipe */
    ssize_t r = quic_blocking_read(ctx->rx_pipe[0], hdr, QUIC_PIPE_MSG_HDR);
    if (r != QUIC_PIPE_MSG_HDR) {
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    uint32_t frame_len = ((uint32_t)hdr[0] << 24) |
                         ((uint32_t)hdr[1] << 16) |
                         ((uint32_t)hdr[2] <<  8) |
                         ((uint32_t)hdr[3]);

    if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    frame = malloc(frame_len);
    FAIL_IF(NULL == frame, E__INVALID_ARG_NULL_POINTER);

    r = quic_blocking_read(ctx->rx_pipe[0], frame, frame_len);
    if (r != (ssize_t)frame_len) {
        free(frame);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    size_t data_len = 0;
    rc = PROTOCOL__unserialize(frame, frame_len, msg, data, &data_len);
    free(frame);

l_cleanup:
    return rc;
}

/* ── transport_destroy ────────────────────────────────────────────────────────── */

static void quic_transport_destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)t->skin_ctx;
    if (NULL == ctx) {
        return;
    }

    ctx->io_running = 0;

    /* Wake up the I/O thread by closing the tx_pipe write end */
    if (ctx->tx_pipe[1] >= 0) {
        close(ctx->tx_pipe[1]);
        ctx->tx_pipe[1] = -1;
    }

    if (0 != ctx->io_thread) {
        pthread_join(ctx->io_thread, NULL);
        ctx->io_thread = 0;
    }

    quic_free_ctx(ctx);
    t->skin_ctx = NULL;
    t->fd       = -1;
}

/* ── Vtable singleton ────────────────────────────────────────────────────────── */

static const skin_ops_t g_udp_quic_skin = {
    .skin_id            = SKIN_ID__QUIC,
    .name               = "udp-quic",
    .listener_create    = quic_listener_create,
    .listener_accept    = quic_listener_accept,
    .listener_destroy   = quic_listener_destroy,
    .connect            = quic_connect,
    .send_msg           = quic_send_msg,
    .recv_msg           = quic_recv_msg,
    .transport_destroy  = quic_transport_destroy,
};

const skin_ops_t *SKIN_UDP_QUIC__ops(void) {
    return &g_udp_quic_skin;
}

err_t SKIN_UDP_QUIC__register(void) {
    return SKIN__register(&g_udp_quic_skin);
}
