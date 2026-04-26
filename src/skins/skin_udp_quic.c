/*
 * udp-quic skin: QUIC transport using ngtcp2 + picotls (mbedTLS crypto).
 *
 * Clean re-implementation that fixes the critical bugs in the original
 * udp-quic skin:
 *
 *   - exts[1].type initialised to UINT16_MAX immediately so picotls never
 *     walks off the end of the two-entry array.
 *   - configure_client_session called AFTER ngtcp2_conn_client_new (it needs
 *     a live conn to encode local transport params).
 *   - TLS context fields stored in the listener struct, not compound literals.
 *   - I/O thread joined before freeing context.
 *
 * Wire format: identical to tcp-plain — [4-byte BE length][protocol frame].
 * ALPN: "ganon".
 * Skin id: SKIN_ID__QUIC (7).
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
#include <picotls/mbedtls.h>
#include <psa/crypto.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "protocol.h"
#include "skin.h"
#include "skins/skin_udp_quic.h"
#include "transport.h"
#include "skin_udp_quic_internal.h"
#include "quic_tls.h"
#include "quic_ngtcp2_crypto.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

#define QUIC_ALPN          "\x08ganon"   /* 1-byte length + 8 chars */
#define QUIC_ALPN_LEN      9
#define QUIC_ALPN_STR      "ganon"
#define QUIC_ALPN_STRLEN   8
#define QUIC_MAX_PKT       1500
#define QUIC_RX_BUF_INIT   65536
#define QUIC_PIPE_HDR      4            /* uint32_t frame-length prefix in pipe */

/* ── Timestamp ──────────────────────────────────────────────────────────── */

static ngtcp2_tstamp quic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * NGTCP2_SECONDS +
           (ngtcp2_tstamp)ts.tv_nsec;
}

/* ── Randomness ─────────────────────────────────────────────────────────── */

static void quic_random(void *buf, size_t len) {
    ptls_mbedtls_random_bytes(buf, len);
}

/* ── Misc helpers ───────────────────────────────────────────────────────── */

static void quic_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ── Forward declarations ───────────────────────────────────────────────── */

static ngtcp2_conn *quic_get_conn(ngtcp2_crypto_conn_ref *ref);
static void        *quic_io_thread(void *arg);
static err_t        quic_flush_send(skin_quic_ctx_t *ctx);
static err_t        quic_write_stream(skin_quic_ctx_t *ctx,
                                       const uint8_t *data, size_t datalen);
static err_t        quic_service_tx(skin_quic_ctx_t *ctx);
static int          quic_drain_stream_buf(skin_quic_ctx_t *ctx);
static err_t        quic_start_io_thread(skin_quic_ctx_t *ctx);

/* ── ngtcp2 callbacks ───────────────────────────────────────────────────── */

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
    (void)conn; (void)user_data;
    quic_random(cid->data, cidlen);
    cid->datalen = cidlen;
    quic_random(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

static int quic_drain_stream_buf(skin_quic_ctx_t *ctx) {
    while (ctx->stream_buf_len >= QUIC_PIPE_HDR) {
        uint32_t frame_len = ((uint32_t)ctx->stream_buf[0] << 24) |
                             ((uint32_t)ctx->stream_buf[1] << 16) |
                             ((uint32_t)ctx->stream_buf[2] <<  8) |
                             ((uint32_t)ctx->stream_buf[3]);

        if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
            LOG_ERROR("QUIC: invalid frame length %u", frame_len);
            return -1;
        }
        if (ctx->stream_buf_len < QUIC_PIPE_HDR + frame_len)
            break;  /* incomplete frame */

        size_t total   = QUIC_PIPE_HDR + frame_len;
        ssize_t written = write(ctx->rx_pipe[1], ctx->stream_buf, total);
        if (written < 0) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) break;
            LOG_ERROR("QUIC: rx_pipe write failed: %s", strerror(errno));
            return -1;
        }
        if ((size_t)written < total) {
            ctx->stream_buf_len -= (size_t)written;
            memmove(ctx->stream_buf, ctx->stream_buf + (size_t)written,
                    ctx->stream_buf_len);
            break;
        }
        ctx->stream_buf_len -= total;
        if (ctx->stream_buf_len > 0)
            memmove(ctx->stream_buf, ctx->stream_buf + total,
                    ctx->stream_buf_len);
    }
    return 0;
}

static int quic_drain_tx_pipe_buf(skin_quic_ctx_t *ctx) {
    while (ctx->tx_pipe_buf_len >= QUIC_PIPE_HDR) {
        uint32_t frame_len = ((uint32_t)ctx->tx_pipe_buf[0] << 24) |
                             ((uint32_t)ctx->tx_pipe_buf[1] << 16) |
                             ((uint32_t)ctx->tx_pipe_buf[2] <<  8) |
                             ((uint32_t)ctx->tx_pipe_buf[3]);

        if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
            LOG_ERROR("QUIC: invalid tx frame length %u", frame_len);
            return -1;
        }
        if (ctx->tx_pipe_buf_len < QUIC_PIPE_HDR + frame_len) break;

        size_t total = QUIC_PIPE_HDR + frame_len;
        LOG_DEBUG("QUIC: drain_tx_pipe_buf: frame len=%u stream_id=%lld", frame_len, (long long)ctx->stream_id);
        if (ctx->stream_id != -1)
            quic_write_stream(ctx, ctx->tx_pipe_buf, total);

        ctx->tx_pipe_buf_len -= total;
        if (ctx->tx_pipe_buf_len > 0)
            memmove(ctx->tx_pipe_buf, ctx->tx_pipe_buf + total,
                    ctx->tx_pipe_buf_len);
    }
    return 0;
}

static int quic_recv_stream_data_cb(ngtcp2_conn *conn,
                                      uint32_t flags, int64_t stream_id,
                                      uint64_t offset,
                                      const uint8_t *data, size_t datalen,
                                      void *user_data, void *stream_user_data) {
    (void)flags; (void)offset; (void)stream_user_data;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;

    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
    ngtcp2_conn_extend_max_offset(conn, datalen);

    if (0 == datalen) return 0;

    if (ctx->stream_buf_len + datalen > ctx->stream_buf_cap) {
        size_t new_cap = ctx->stream_buf_cap + datalen + QUIC_RX_BUF_INIT;
        uint8_t *nb = realloc(ctx->stream_buf, new_cap);
        if (NULL == nb) return NGTCP2_ERR_CALLBACK_FAILURE;
        ctx->stream_buf     = nb;
        ctx->stream_buf_cap = new_cap;
    }
    memcpy(ctx->stream_buf + ctx->stream_buf_len, data, datalen);
    ctx->stream_buf_len += datalen;

    if (0 != quic_drain_stream_buf(ctx))
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

static int quic_stream_open_cb(ngtcp2_conn *conn, int64_t stream_id,
                                  void *user_data) {
    (void)conn;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;
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
    if (!ctx->is_server && -1 == ctx->stream_id) {
        int64_t stream_id;
        if (0 == ngtcp2_conn_open_bidi_stream(conn, &stream_id, NULL)) {
            ctx->stream_id = stream_id;
            LOG_DEBUG("QUIC: client opened stream %lld", (long long)stream_id);
        }
    }
    return 0;
}

static int quic_acked_stream_data_offset_cb(ngtcp2_conn *conn,
                                              int64_t stream_id,
                                              uint64_t offset,
                                              uint64_t datalen,
                                              void *user_data,
                                              void *stream_user_data) {
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)user_data;
    (void)conn; (void)stream_id; (void)stream_user_data;

    uint64_t acked_upto = offset + datalen;

    quic_pending_data_t **curr = &ctx->pending_data;
    while (*curr) {
        quic_pending_data_t *p = *curr;
        if (p->offset + p->len <= acked_upto) {
            *curr = p->next;
            if (p == ctx->pending_data_tail) ctx->pending_data_tail = NULL;
            free(p->buf);
            free(p);
        } else {
            curr = &((*curr)->next);
        }
    }
    if (acked_upto > ctx->stream_acked_offset)
        ctx->stream_acked_offset = acked_upto;
    return 0;
}

static ngtcp2_callbacks g_client_cbs2 = {
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

static ngtcp2_callbacks g_server_cbs2 = {
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

/* ── Send helpers ───────────────────────────────────────────────────────── */

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
        if (0 == n) break;
        if (0 > n) {
            if (NGTCP2_ERR_WRITE_MORE == (int)n) continue;
            LOG_WARN("QUIC: write_pkt: %s", ngtcp2_strerror((int)n));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        ssize_t sent = send(ctx->udp_fd, pkt, (size_t)n, 0);
        if (0 > sent && EAGAIN != errno && EWOULDBLOCK != errno) {
            LOG_WARN("QUIC: sendto: %s", strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
    }

l_cleanup:
    return rc;
}

static err_t quic_write_stream(skin_quic_ctx_t *ctx,
                                 const uint8_t *data, size_t datalen) {
    err_t rc = E__SUCCESS;

    quic_pending_data_t *p = malloc(sizeof(*p));
    FAIL_IF(NULL == p, E__QUIC__ALLOC_FAILED);
    p->offset = ctx->stream_queued_offset;
    p->len    = datalen;
    p->buf    = malloc(datalen);
    if (NULL == p->buf) { free(p); FAIL(E__QUIC__ALLOC_FAILED); }
    memcpy(p->buf, data, datalen);
    p->next   = NULL;

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
        quic_pending_data_t *p = ctx->pending_data;
        while (p && p->offset + p->len <= ctx->stream_sent_offset)
            p = p->next;
        if (!p) break;

        LOG_DEBUG("QUIC: service_tx: pending data len=%zu sent_off=%llu queued_off=%llu stream_id=%lld",
                  p->len - (size_t)(ctx->stream_sent_offset - p->offset),
                  (unsigned long long)ctx->stream_sent_offset,
                  (unsigned long long)ctx->stream_queued_offset,
                  (long long)ctx->stream_id);

        size_t stream_offset = (size_t)(ctx->stream_sent_offset - p->offset);
        ngtcp2_vec vec = { p->buf + stream_offset, p->len - stream_offset };
        ngtcp2_ssize pdatalen = 0;
        ngtcp2_tstamp ts = quic_now();

        pthread_mutex_lock(&ctx->conn_mutex);
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(ctx->conn, &ps.path, &pi,
                                                    pkt, sizeof(pkt),
                                                    &pdatalen,
                                                    NGTCP2_WRITE_STREAM_FLAG_NONE,
                                                    ctx->stream_id,
                                                    &vec, 1, ts);
        pthread_mutex_unlock(&ctx->conn_mutex);

        LOG_DEBUG("QUIC: service_tx: writev_stream returned n=%zd pdatalen=%zd", (ssize_t)n, (ssize_t)pdatalen);

        if (pdatalen > 0)
            ctx->stream_sent_offset += (uint64_t)pdatalen;

        if (0 > n) {
            if (NGTCP2_ERR_STREAM_DATA_BLOCKED == (int)n ||
                NGTCP2_ERR_STREAM_NOT_FOUND    == (int)n) {
                LOG_DEBUG("QUIC: service_tx: stream blocked or not found: %s", ngtcp2_strerror((int)n));
                break;
            }
            LOG_WARN("QUIC: writev_stream: %s", ngtcp2_strerror((int)n));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        if (0 == n) {
            LOG_DEBUG("QUIC: service_tx: writev_stream returned 0");
            break;
        }

        ssize_t sent = send(ctx->udp_fd, pkt, (size_t)n, 0);
        if (0 > sent && EAGAIN != errno && EWOULDBLOCK != errno) {
            LOG_WARN("QUIC: stream sendto: %s", strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
    }

    quic_flush_send(ctx);

l_cleanup:
    return rc;
}

/* ── I/O thread ─────────────────────────────────────────────────────────── */

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
        fds[2].events = (ctx->stream_buf_len >= QUIC_PIPE_HDR) ? POLLOUT : 0;

        poll(fds, 3, timeout_ms);

        now = quic_now();

        if (fds[2].revents & POLLOUT)
            quic_drain_stream_buf(ctx);

        /* Timer */
        pthread_mutex_lock(&ctx->conn_mutex);
        if (ngtcp2_conn_get_expiry(ctx->conn) <= now)
            ngtcp2_conn_handle_expiry(ctx->conn, now);
        pthread_mutex_unlock(&ctx->conn_mutex);
        quic_flush_send(ctx);
        quic_service_tx(ctx);

        /* Incoming UDP */
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
                        LOG_WARN("QUIC: read_pkt: %s", ngtcp2_strerror(rv));
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

        /* TX pipe → QUIC stream */
        if (fds[1].revents & POLLIN) {
            uint8_t buf[16384];
            for (;;) {
                ssize_t n = read(ctx->tx_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (ctx->tx_pipe_buf_len + (size_t)n > ctx->tx_pipe_buf_cap) {
                        size_t new_cap = ctx->tx_pipe_buf_cap + (size_t)n + QUIC_RX_BUF_INIT;
                        uint8_t *nb = realloc(ctx->tx_pipe_buf, new_cap);
                        if (nb) {
                            ctx->tx_pipe_buf     = nb;
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

/* ── Context allocation / free ──────────────────────────────────────────── */

static skin_quic_ctx_t *quic_alloc_ctx(void) {
    skin_quic_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) return NULL;

    ctx->stream_id       = -1;
    ctx->rx_pipe[0] = ctx->rx_pipe[1] = -1;
    ctx->tx_pipe[0] = ctx->tx_pipe[1] = -1;
    ctx->udp_fd          = -1;

    /* Pre-initialise the extension sentinel.  This is the critical fix: without
     * it picotls walks past exts[1] into the rest of the struct, finds a large
     * data.len from some other field, and crashes in push_additional_extensions. */
    ctx->exts[1].type = UINT16_MAX;

    ctx->stream_buf_cap = QUIC_RX_BUF_INIT;
    ctx->stream_buf     = malloc(ctx->stream_buf_cap);
    if (NULL == ctx->stream_buf) { free(ctx); return NULL; }

    ctx->tx_pipe_buf_cap = QUIC_RX_BUF_INIT;
    ctx->tx_pipe_buf     = malloc(ctx->tx_pipe_buf_cap);
    if (NULL == ctx->tx_pipe_buf) { free(ctx->stream_buf); free(ctx); return NULL; }

    pthread_mutex_init(&ctx->conn_mutex, NULL);
    return ctx;
}

static void quic_free_ctx(skin_quic_ctx_t *ctx) {
    if (NULL == ctx) return;

    ctx->io_running = 0;

    if (ctx->rx_pipe[0] >= 0) close(ctx->rx_pipe[0]);
    if (ctx->rx_pipe[1] >= 0) close(ctx->rx_pipe[1]);
    if (ctx->tx_pipe[0] >= 0) close(ctx->tx_pipe[0]);
    if (ctx->tx_pipe[1] >= 0) close(ctx->tx_pipe[1]);
    if (ctx->udp_fd >= 0)     close(ctx->udp_fd);

    if (NULL != ctx->conn) {
        quic_crypto_free_conn_data(ctx->conn);
        ngtcp2_crypto_picotls_deconfigure_session(&ctx->cptls);
        if (NULL != ctx->cptls.ptls) ptls_free(ctx->cptls.ptls);
        ngtcp2_conn_del(ctx->conn);
    }

    quic_pending_data_t *p = ctx->pending_data;
    while (p) {
        quic_pending_data_t *next = p->next;
        free(p->buf);
        free(p);
        p = next;
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
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }
l_cleanup:
    return rc;
}

static transport_t *quic_alloc_transport(skin_quic_ctx_t *ctx,
                                           int is_incoming,
                                           const char *ip, int port,
                                           const skin_ops_t *skin) {
    transport_t *t = TRANSPORT__alloc_base(ctx->udp_fd, skin);
    if (NULL == t) return NULL;
    t->is_incoming = is_incoming;
    if (NULL != ip) {
        strncpy(t->client_ip, ip, INET_ADDRSTRLEN - 1);
        t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    }
    t->client_port = port;
    t->skin_ctx    = ctx;
    return t;
}

/* ── UDP socket helper ──────────────────────────────────────────────────── */

static int quic_udp_bind(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "*"))
        sa.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, ip, &sa.sin_addr);

    if (0 != bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Handshake loop (used by both accept and connect) ───────────────────── */

static err_t quic_run_handshake(skin_quic_ctx_t *ctx,
                                  struct sockaddr_storage *local_addr,
                                  socklen_t local_addrlen,
                                  int timeout_sec) {
    err_t rc = E__SUCCESS;
    ngtcp2_tstamp now = quic_now();
    ngtcp2_tstamp end_ts = now + (ngtcp2_tstamp)timeout_sec * NGTCP2_SECONDS;

    struct pollfd pfd = { ctx->udp_fd, POLLIN, 0 };

    while (!ctx->handshake_done) {
        now = quic_now();
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
            socklen_t addrlen = sizeof(addr);
            ssize_t m = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0,
                                  (struct sockaddr *)&addr, &addrlen);
            if (m > 0) {
                ngtcp2_path path2;
                ngtcp2_addr_init(&path2.local,
                                  (struct sockaddr *)local_addr, local_addrlen);
                ngtcp2_addr_init(&path2.remote,
                                  (struct sockaddr *)&addr, addrlen);
                ngtcp2_pkt_info pi2 = {0};
                pthread_mutex_lock(&ctx->conn_mutex);
                ngtcp2_conn_read_pkt(ctx->conn, &path2, &pi2, buf, (size_t)m,
                                      quic_now());
                pthread_mutex_unlock(&ctx->conn_mutex);
                quic_flush_send(ctx);
            }
        } else {
            now = quic_now();
            pthread_mutex_lock(&ctx->conn_mutex);
            if (ngtcp2_conn_get_expiry(ctx->conn) <= now)
                ngtcp2_conn_handle_expiry(ctx->conn, now);
            pthread_mutex_unlock(&ctx->conn_mutex);
            quic_flush_send(ctx);
        }
    }

    FAIL_IF(!ctx->handshake_done, E__QUIC__HANDSHAKE_FAILED);

l_cleanup:
    return rc;
}

/* ── listener_create ─────────────────────────────────────────────────────── */

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
    FAIL_IF(NULL == l, E__QUIC__ALLOC_FAILED);
    l->addr = *addr;

    psa_crypto_init();

    if (0 != quic_tls_listener_init(&l->tls_ctx, &l->sign_cert, &l->pkey,
                                      l->key_ex, l->ciphers,
                                      &l->cert_der, &l->cert_der_len,
                                      l->cert_iov)) {
        LOG_ERROR("QUIC: TLS listener init failed");
        free(l);
        FAIL(E__QUIC__INIT_FAILED);
    }

    fd = quic_udp_bind(addr->ip, addr->port);
    if (fd < 0) {
        LOG_ERROR("QUIC: bind %s:%d failed: %s", addr->ip, addr->port,
                  strerror(errno));
        mbedtls_pk_free(&l->pkey);
        free(l->cert_der);
        free(l);
        FAIL(E__NET__SOCKET_BIND_FAILED);
    }
    quic_set_nonblocking(fd);

    l->listen_fd   = fd;
    *out_listen_fd = fd;
    *out_listener  = (skin_listener_t *)l;

    LOG_INFO("QUIC: listener on %s:%d (udp)", addr->ip, addr->port);

l_cleanup:
    return rc;
}

/* ── listener_accept ─────────────────────────────────────────────────────── */

static err_t quic_listener_accept(skin_listener_t *sl,
                                    transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_quic_listener_t *l = (skin_quic_listener_t *)sl;
    skin_quic_ctx_t *ctx    = NULL;
    transport_t *t           = NULL;

    VALIDATE_ARGS(l, out_transport);
    *out_transport = NULL;

    uint8_t pkt[65536];
    struct sockaddr_storage remote_addr;
    socklen_t remote_addrlen = sizeof(remote_addr);
    ssize_t n = recvfrom(l->listen_fd, pkt, sizeof(pkt), MSG_DONTWAIT,
                          (struct sockaddr *)&remote_addr, &remote_addrlen);
    if (n < 0) FAIL(E__NET__SOCKET_ACCEPT_FAILED);

    LOG_INFO("QUIC: listener received %zd bytes", n);

    /* Require QUIC long-header (high bit set) */
    if ((pkt[0] & 0x80) == 0) {
        LOG_WARN("QUIC: ignoring non-long-header packet");
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    struct sockaddr_storage local_addr;
    socklen_t local_addrlen = sizeof(local_addr);
    getsockname(l->listen_fd, (struct sockaddr *)&local_addr, &local_addrlen);

    int conn_fd = quic_udp_bind(l->addr.ip, l->addr.port);
    if (conn_fd < 0) {
        LOG_ERROR("QUIC: per-conn socket failed: %s", strerror(errno));
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    if (0 != connect(conn_fd, (struct sockaddr *)&remote_addr, remote_addrlen)) {
        LOG_WARN("QUIC: connect to remote failed: %s", strerror(errno));
        close(conn_fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }
    quic_set_nonblocking(conn_fd);

    ctx = quic_alloc_ctx();
    if (NULL == ctx) { close(conn_fd); FAIL(E__QUIC__ALLOC_FAILED); }

    ctx->udp_fd         = conn_fd;
    ctx->is_server      = 1;
    ctx->is_incoming    = 1;
    ctx->remote_addr    = remote_addr;
    ctx->remote_addrlen = remote_addrlen;
    ctx->local_addr     = local_addr;
    ctx->local_addrlen  = local_addrlen;

    ctx->conn_ref.get_conn  = quic_get_conn;
    ctx->conn_ref.user_data = ctx;

    /* Create picotls session for this connection. */
    ngtcp2_crypto_picotls_ctx_init(&ctx->cptls);

    /* Set additional_extensions BEFORE configure_server_session and BEFORE
     * ptls_new.  exts[1].type == UINT16_MAX is already set in alloc_ctx so
     * picotls has a valid (empty) extension list from the very first callback. */
    ctx->cptls.handshake_properties.additional_extensions = ctx->exts;

    ctx->cptls.ptls = ptls_new(&l->tls_ctx, 1 /* is_server */);
    if (NULL == ctx->cptls.ptls) {
        LOG_ERROR("QUIC: ptls_new failed");
        quic_free_ctx(ctx);
        FAIL(E__QUIC__INIT_FAILED);
    }
    *ptls_get_data_ptr(ctx->cptls.ptls) = &ctx->conn_ref;

    if (0 != ngtcp2_crypto_picotls_configure_server_session(&ctx->cptls)) {
        LOG_ERROR("QUIC: configure_server_session failed");
        quic_free_ctx(ctx);
        FAIL(E__QUIC__INIT_FAILED);
    }

    /* Decode version + CIDs from Initial packet */
    ngtcp2_version_cid vcid;
    int rv = ngtcp2_pkt_decode_version_cid(&vcid, pkt, (size_t)n, 8);
    if (0 != rv) {
        LOG_WARN("QUIC: decode_version_cid: %s", ngtcp2_strerror(rv));
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    ngtcp2_cid scid;
    quic_random(scid.data, 8);
    scid.datalen = 8;

    ngtcp2_cid dcid;
    memcpy(dcid.data, vcid.scid, vcid.scidlen);
    dcid.datalen = vcid.scidlen;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = quic_now();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi              = 8;
    params.initial_max_streams_uni               = 0;
    params.initial_max_data                      = 256 * 1024;
    params.initial_max_stream_data_bidi_local    = 128 * 1024;
    params.initial_max_stream_data_bidi_remote   = 128 * 1024;
    params.original_dcid_present = 1;
    memcpy(params.original_dcid.data, vcid.dcid, vcid.dcidlen);
    params.original_dcid.datalen = vcid.dcidlen;

    ngtcp2_path path;
    ngtcp2_addr_init(&path.local,  (struct sockaddr *)&local_addr, local_addrlen);
    ngtcp2_addr_init(&path.remote, (struct sockaddr *)&remote_addr, remote_addrlen);

    rv = ngtcp2_conn_server_new(&ctx->conn, &dcid, &scid, &path,
                                 vcid.version, &g_server_cbs2,
                                 &settings, &params, NULL, ctx);
    if (0 != rv) {
        LOG_ERROR("QUIC: conn_server_new: %s", ngtcp2_strerror(rv));
        quic_free_ctx(ctx);
        FAIL(E__QUIC__INIT_FAILED);
    }
    ngtcp2_conn_set_tls_native_handle(ctx->conn, &ctx->cptls);

    /* Feed the Initial packet */
    {
        ngtcp2_pkt_info pi = {0};
        pthread_mutex_lock(&ctx->conn_mutex);
        rv = ngtcp2_conn_read_pkt(ctx->conn, &path, &pi, pkt, (size_t)n,
                                   quic_now());
        pthread_mutex_unlock(&ctx->conn_mutex);
        if (0 != rv && NGTCP2_ERR_RETRY != rv)
            LOG_WARN("QUIC: read_pkt (Initial): %s", ngtcp2_strerror(rv));
        quic_flush_send(ctx);
    }

    /* Synchronous handshake (5 s timeout) */
    rc = quic_run_handshake(ctx, &local_addr, local_addrlen, 5);
    if (E__SUCCESS != rc) {
        LOG_WARN("QUIC: server handshake failed");
        quic_free_ctx(ctx);
        goto l_cleanup;
    }

    /* Pipes + I/O thread */
    if (0 != pipe(ctx->rx_pipe) || 0 != pipe(ctx->tx_pipe)) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    quic_set_nonblocking(ctx->rx_pipe[0]);
    quic_set_nonblocking(ctx->rx_pipe[1]);
    quic_set_nonblocking(ctx->tx_pipe[0]);
    quic_set_nonblocking(ctx->tx_pipe[1]);

    rc = quic_start_io_thread(ctx);
    if (E__SUCCESS != rc) { quic_free_ctx(ctx); goto l_cleanup; }

    char remote_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    int  remote_port_n = 0;
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
        FAIL(E__QUIC__ALLOC_FAILED);
    }

    LOG_INFO("QUIC: accepted connection from %s:%d", remote_ip, remote_port_n);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* ── listener_destroy ─────────────────────────────────────────────────────── */

static void quic_listener_destroy(skin_listener_t *sl) {
    skin_quic_listener_t *l = (skin_quic_listener_t *)sl;
    if (NULL == l) return;
    if (l->listen_fd >= 0) close(l->listen_fd);
    mbedtls_pk_free(&l->pkey);
    free(l->cert_der);
    free(l);
}

/* ── connect ──────────────────────────────────────────────────────────────── */

static err_t quic_connect(const char *ip, int port, int connect_timeout_sec,
                            transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_quic_ctx_t *ctx = NULL;
    transport_t *t        = NULL;

    VALIDATE_ARGS(ip, out_transport);
    *out_transport = NULL;

    ctx = quic_alloc_ctx();
    FAIL_IF(NULL == ctx, E__QUIC__ALLOC_FAILED);

    ctx->is_server = 0;

    /* UDP socket connected to remote */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { quic_free_ctx(ctx); FAIL(E__NET__SOCKET_CREATE_FAILED); }
    ctx->udp_fd = fd;

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port   = htons((uint16_t)port);
    if (0 == inet_pton(AF_INET, ip, &remote.sin_addr)) {
        struct hostent *he = gethostbyname(ip);
        if (NULL == he) { quic_free_ctx(ctx); FAIL(E__NET__SOCKET_CONNECT_FAILED); }
        memcpy(&remote.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }
    if (0 != connect(fd, (struct sockaddr *)&remote, sizeof(remote))) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }
    quic_set_nonblocking(fd);

    ctx->local_addrlen = sizeof(ctx->local_addr);
    getsockname(fd, (struct sockaddr *)&ctx->local_addr, &ctx->local_addrlen);
    memcpy(&ctx->remote_addr, &remote, sizeof(remote));
    ctx->remote_addrlen = sizeof(remote);

    psa_crypto_init();

    /* Client TLS context */
    ctx->key_ex_c[0]  = &ptls_mbedtls_secp256r1;
    ctx->key_ex_c[1]  = NULL;
    ctx->ciphers_c[0] = &ptls_mbedtls_aes128gcmsha256;
    ctx->ciphers_c[1] = &ptls_mbedtls_aes256gcmsha384;
    ctx->ciphers_c[2] = NULL;

    memset(&ctx->tls_ctx_client, 0, sizeof(ctx->tls_ctx_client));
    ctx->tls_ctx_client.random_bytes       = ptls_mbedtls_random_bytes;
    ctx->tls_ctx_client.get_time           = &ptls_get_time;
    ctx->tls_ctx_client.key_exchanges      = ctx->key_ex_c;
    ctx->tls_ctx_client.cipher_suites      = ctx->ciphers_c;
    ctx->tls_ctx_client.verify_certificate = NULL; /* ganon auth at protocol layer */

    ngtcp2_crypto_picotls_configure_client_context(&ctx->tls_ctx_client);

    ctx->conn_ref.get_conn  = quic_get_conn;
    ctx->conn_ref.user_data = ctx;

    ngtcp2_crypto_picotls_ctx_init(&ctx->cptls);

    /* additional_extensions set BEFORE ptls_new (sentinel already in exts[1]) */
    ctx->cptls.handshake_properties.additional_extensions = ctx->exts;

    ctx->cptls.ptls = ptls_new(&ctx->tls_ctx_client, 0 /* client */);
    if (NULL == ctx->cptls.ptls) {
        quic_free_ctx(ctx);
        FAIL(E__QUIC__INIT_FAILED);
    }
    *ptls_get_data_ptr(ctx->cptls.ptls) = &ctx->conn_ref;
    ptls_set_server_name(ctx->cptls.ptls, ip, strlen(ip));

    /* ALPN "ganon" */
    ctx->alpn_list[0] = ptls_iovec_init(QUIC_ALPN_STR, QUIC_ALPN_STRLEN);
    ctx->cptls.handshake_properties.client.negotiated_protocols.list  = ctx->alpn_list;
    ctx->cptls.handshake_properties.client.negotiated_protocols.count = 1;

    /* QUIC CIDs */
    ngtcp2_cid dcid, scid;
    quic_random(dcid.data, 8); dcid.datalen = 8;
    quic_random(scid.data, 8); scid.datalen = 8;

    ngtcp2_path path;
    ngtcp2_addr_init(&path.local,  (struct sockaddr *)&ctx->local_addr,
                     ctx->local_addrlen);
    ngtcp2_addr_init(&path.remote, (struct sockaddr *)&remote, sizeof(remote));

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = quic_now();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi            = 8;
    params.initial_max_data                    = 256 * 1024;
    params.initial_max_stream_data_bidi_local  = 128 * 1024;
    params.initial_max_stream_data_bidi_remote = 128 * 1024;

    int rv = ngtcp2_conn_client_new(&ctx->conn, &dcid, &scid, &path,
                                     NGTCP2_PROTO_VER_V1, &g_client_cbs2,
                                     &settings, &params, NULL, ctx);
    if (0 != rv) {
        LOG_ERROR("QUIC: conn_client_new: %s", ngtcp2_strerror(rv));
        quic_free_ctx(ctx);
        FAIL(E__QUIC__INIT_FAILED);
    }

    /* configure_client_session MUST be called after conn_client_new — it calls
     * ngtcp2_conn_encode_local_transport_params which needs a live conn. */
    if (0 != ngtcp2_crypto_picotls_configure_client_session(&ctx->cptls,
                                                             ctx->conn)) {
        LOG_ERROR("QUIC: configure_client_session failed");
        quic_free_ctx(ctx);
        FAIL(E__QUIC__INIT_FAILED);
    }

    ngtcp2_conn_set_tls_native_handle(ctx->conn, &ctx->cptls);

    /* Kick off handshake */
    quic_flush_send(ctx);

    rc = quic_run_handshake(ctx, &ctx->local_addr, ctx->local_addrlen,
                              connect_timeout_sec);
    if (E__SUCCESS != rc) {
        LOG_WARN("QUIC: client handshake to %s:%d failed", ip, port);
        quic_free_ctx(ctx);
        goto l_cleanup;
    }

    /* Wait briefly for the stream to open (client triggers it via
     * extend_max_local_streams_bidi callback). */
    {
        int wait = 50;
        while (--wait > 0 && -1 == ctx->stream_id) {
            quic_flush_send(ctx);
            usleep(20000);
        }
        if (-1 == ctx->stream_id) {
            LOG_WARN("QUIC: stream not opened after handshake");
            quic_free_ctx(ctx);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
    }

    /* Pipes + I/O thread */
    if (0 != pipe(ctx->rx_pipe) || 0 != pipe(ctx->tx_pipe)) {
        quic_free_ctx(ctx);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    quic_set_nonblocking(ctx->rx_pipe[0]);
    quic_set_nonblocking(ctx->rx_pipe[1]);
    quic_set_nonblocking(ctx->tx_pipe[0]);
    quic_set_nonblocking(ctx->tx_pipe[1]);

    rc = quic_start_io_thread(ctx);
    if (E__SUCCESS != rc) { quic_free_ctx(ctx); goto l_cleanup; }

    t = quic_alloc_transport(ctx, 0, ip, port, SKIN_UDP_QUIC__ops());
    if (NULL == t) {
        ctx->io_running = 0;
        pthread_join(ctx->io_thread, NULL);
        quic_free_ctx(ctx);
        FAIL(E__QUIC__ALLOC_FAILED);
    }

    LOG_INFO("QUIC: connected to %s:%d", ip, port);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* ── Blocking pipe I/O ──────────────────────────────────────────────────── */

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
                struct pollfd pfd = { fd, POLLIN, 0 };
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
                struct pollfd pfd = { fd, POLLOUT, 0 };
                poll(&pfd, 1, -1);
                continue;
            }
            return -1;
        }
    }
    return (ssize_t)total;
}

/* ── send_msg ────────────────────────────────────────────────────────────── */

static err_t quic_send_msg(transport_t *t, const protocol_msg_t *msg,
                             const uint8_t *data) {
    err_t rc = E__SUCCESS;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)t->skin_ctx;

    VALIDATE_ARGS(t, msg);

    size_t payload_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0)
        payload_len += msg->data_length;

    uint8_t *frame = malloc(QUIC_PIPE_HDR + payload_len);
    FAIL_IF(NULL == frame, E__QUIC__ALLOC_FAILED);

    uint32_t net_len = htonl((uint32_t)payload_len);
    memcpy(frame, &net_len, 4);

    size_t written = 0;
    rc = PROTOCOL__serialize(msg, data, frame + QUIC_PIPE_HDR,
                              payload_len, &written);
    if (E__SUCCESS != rc) { free(frame); goto l_cleanup; }

    size_t total = QUIC_PIPE_HDR + written;
    LOG_DEBUG("QUIC: send_msg: writing %zu bytes to tx_pipe[1] (msg_type=%u)", total, msg->type);
    ssize_t wr   = quic_blocking_write(ctx->tx_pipe[1], frame, total);
    free(frame);

    if (wr != (ssize_t)total) {
        LOG_WARN("QUIC: send_msg: short write %zd != %zu", wr, total);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

l_cleanup:
    return rc;
}

/* ── recv_msg ────────────────────────────────────────────────────────────── */

static err_t quic_recv_msg(transport_t *t, protocol_msg_t *msg,
                             uint8_t **data) {
    err_t rc = E__SUCCESS;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)t->skin_ctx;
    uint8_t hdr[QUIC_PIPE_HDR];
    uint8_t *frame = NULL;

    VALIDATE_ARGS(t, msg, data);
    *data = NULL;

    ssize_t r = quic_blocking_read(ctx->rx_pipe[0], hdr, QUIC_PIPE_HDR);
    if (r != QUIC_PIPE_HDR) FAIL(E__NET__SOCKET_CONNECT_FAILED);

    uint32_t frame_len = ((uint32_t)hdr[0] << 24) |
                         ((uint32_t)hdr[1] << 16) |
                         ((uint32_t)hdr[2] <<  8) |
                         ((uint32_t)hdr[3]);

    if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000)
        FAIL(E__QUIC__BAD_FRAME_LEN);

    frame = malloc(frame_len);
    FAIL_IF(NULL == frame, E__QUIC__ALLOC_FAILED);

    r = quic_blocking_read(ctx->rx_pipe[0], frame, frame_len);
    if (r != (ssize_t)frame_len) { free(frame); FAIL(E__NET__SOCKET_CONNECT_FAILED); }

    size_t data_len = 0;
    rc = PROTOCOL__unserialize(frame, frame_len, msg, data, &data_len);
    free(frame);

l_cleanup:
    return rc;
}

/* ── transport_destroy ───────────────────────────────────────────────────── */

static void quic_transport_destroy(transport_t *t) {
    if (NULL == t) return;
    skin_quic_ctx_t *ctx = (skin_quic_ctx_t *)t->skin_ctx;
    if (NULL == ctx) return;

    ctx->io_running = 0;

    /* Closing the write-end wakes the I/O thread's poll on tx_pipe[0]. */
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

/* ── Vtable ──────────────────────────────────────────────────────────────── */

static const skin_ops_t g_udp_quic_skin = {
    .skin_id           = SKIN_ID__QUIC,
    .name              = "udp-quic",
    .listener_create   = quic_listener_create,
    .listener_accept   = quic_listener_accept,
    .listener_destroy  = quic_listener_destroy,
    .connect           = quic_connect,
    .send_msg          = quic_send_msg,
    .recv_msg          = quic_recv_msg,
    .transport_destroy = quic_transport_destroy,
};

const skin_ops_t *SKIN_UDP_QUIC__ops(void) {
    return &g_udp_quic_skin;
}

err_t SKIN_UDP_QUIC__register(void) {
    return SKIN__register(&g_udp_quic_skin);
}
