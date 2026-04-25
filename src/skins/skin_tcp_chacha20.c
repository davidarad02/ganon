/*
 * tcp-chacha20 skin:
 *   TCP transport + X25519 key-exchange + BLAKE2b KDF + ChaCha20 stream cipher.
 *
 *   Uses the same ephemeral X25519 handshake as tcp-monocypher, but instead
 *   of XChaCha20-Poly1305 AEAD it uses ChaCha20 as a raw stream cipher
 *   (no authentication).  This provides per-connection obfuscation at
 *   roughly 2-3x the throughput of the AEAD skin because the Poly1305 MAC
 *   computation and 44-byte frame overhead are eliminated.
 *
 * Wire frame format (after handshake):
 *   [4 bytes] big-endian payload length
 *   [8 bytes] nonce (little-endian message counter)
 *   [N bytes] ChaCha20-obfuscated serialized protocol_msg_t + data
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "monocypher.h"
#include "protocol.h"
#include "skin.h"
#include "skins/skin_tcp_chacha20.h"
#include "transport.h"

#ifdef USE_LIBSODIUM
#include <sodium.h>
#endif

/* ---- Per-connection opaque context -------------------------------------- */

typedef struct {
    uint8_t enc_ephemeral_priv[32];
    uint8_t enc_ephemeral_pub[32];
    uint8_t enc_send_key[32];
    uint8_t enc_recv_key[32];
    int     enc_is_initiator;
    uint64_t enc_send_counter;
    uint64_t enc_recv_counter;
} skin_tcp_chacha20_ctx_t;

/* ---- Per-listener state ------------------------------------------------- */

typedef struct {
    int    listen_fd;
    addr_t addr;
} skin_tcp_chacha20_listener_t;

/* ---- Helpers: random, key-derivation ------------------------------------ */

static int get_random_bytes(uint8_t *buf, size_t len) {
#if defined(__linux__) && defined(SYS_getrandom)
    ssize_t n = getrandom(buf, len, 0);
    if (n == (ssize_t)len) {
        return 0;
    }
#endif
    static int urandom_fd = -1;
    if (urandom_fd < 0) {
        urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (urandom_fd < 0) {
            return -1;
        }
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(urandom_fd, buf + total, len - total);
        if (n < 0) {
            if (EINTR == errno) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static void derive_keys(const uint8_t shared[32],
                        uint8_t send_key[32], uint8_t recv_key[32]) {
    uint8_t out[32];
    const char send_ctx = 'S';
    const char recv_ctx = 'R';

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&send_ctx, 1);
    memcpy(send_key, out, 32);

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&recv_ctx, 1);
    memcpy(recv_key, out, 32);

    crypto_wipe(out, sizeof(out));
}

/* ---- Raw TCP recv/send (blocking) --------------------------------------- */

static err_t ch20_recv_all(int fd, uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) continue;
            LOG_WARNING("recv failed on fd %d: %s", fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        } else if (0 == n) {
            LOG_WARNING("Socket disconnected (fd=%d)", fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total += (size_t)n;
    }
l_cleanup:
    return rc;
}

static err_t ch20_send_all(int fd, const uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) continue;
            LOG_WARNING("send failed on fd %d: %s", fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total += (size_t)n;
    }
l_cleanup:
    return rc;
}

/* ---- ChaCha20 stream cipher --------------------------------------------- */

static void ch20_crypt(uint8_t *buf, size_t len,
                        const uint8_t key[32], uint64_t counter) {
    uint8_t nonce[8];
    nonce[0] = (uint8_t)(counter);
    nonce[1] = (uint8_t)(counter >> 8);
    nonce[2] = (uint8_t)(counter >> 16);
    nonce[3] = (uint8_t)(counter >> 24);
    nonce[4] = (uint8_t)(counter >> 32);
    nonce[5] = (uint8_t)(counter >> 40);
    nonce[6] = (uint8_t)(counter >> 48);
    nonce[7] = (uint8_t)(counter >> 56);

#ifdef USE_LIBSODIUM
    crypto_stream_chacha20_xor(buf, buf, len, nonce, key);
#else
    crypto_chacha20_djb(buf, buf, len, key, nonce, 0);
#endif
}

/* ---- Transport allocation helpers --------------------------------------- */

static transport_t *ch20_alloc_transport(int fd, int is_incoming,
                                          const char *ip, int port,
                                          const skin_ops_t *skin) {
    transport_t *t = TRANSPORT__alloc_base(fd, skin);
    if (NULL == t) {
        return NULL;
    }
    t->is_incoming = is_incoming;
    if (NULL != ip) {
        strncpy(t->client_ip, ip, INET_ADDRSTRLEN - 1);
        t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    }
    t->client_port = port;
    return t;
}

static skin_tcp_chacha20_ctx_t *ch20_ctx_alloc(void) {
    skin_tcp_chacha20_ctx_t *ctx = malloc(sizeof(skin_tcp_chacha20_ctx_t));
    if (NULL == ctx) {
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    return ctx;
}

static void ch20_ctx_free(skin_tcp_chacha20_ctx_t *ctx) {
    if (NULL == ctx) {
        return;
    }
    crypto_wipe(ctx->enc_ephemeral_priv, 32);
    crypto_wipe(ctx->enc_send_key, 32);
    crypto_wipe(ctx->enc_recv_key, 32);
    free(ctx);
}

/* ---- Handshake ---------------------------------------------------------- */

static err_t ch20_do_handshake(transport_t *t, int is_initiator) {
    err_t rc = E__SUCCESS;
    skin_tcp_chacha20_ctx_t *ctx = (skin_tcp_chacha20_ctx_t *)t->skin_ctx;
    uint8_t peer_pub[32];
    uint8_t shared[32];

    VALIDATE_ARGS(ctx);

    if (0 != get_random_bytes(ctx->enc_ephemeral_priv, 32)) {
        LOG_ERROR("Failed to generate random bytes for keypair");
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    crypto_x25519_public_key(ctx->enc_ephemeral_pub, ctx->enc_ephemeral_priv);
    ctx->enc_is_initiator = is_initiator;

    if (is_initiator) {
        rc = ch20_send_all(t->fd, ctx->enc_ephemeral_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
        rc = ch20_recv_all(t->fd, peer_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
    } else {
        rc = ch20_recv_all(t->fd, peer_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
        rc = ch20_send_all(t->fd, ctx->enc_ephemeral_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

    crypto_x25519(shared, ctx->enc_ephemeral_priv, peer_pub);

    if (is_initiator) {
        derive_keys(shared, ctx->enc_send_key, ctx->enc_recv_key);
    } else {
        derive_keys(shared, ctx->enc_recv_key, ctx->enc_send_key);
    }

    ctx->enc_send_counter = 0;
    ctx->enc_recv_counter = 0;

    LOG_DEBUG("ChaCha20 handshake complete on fd=%d", t->fd);

l_cleanup:
    crypto_wipe(shared, sizeof(shared));
    return rc;
}

/* ---- listener_create ---------------------------------------------------- */

static err_t ch20_listener_create(const addr_t *addr,
                                   skin_listener_t **out_listener,
                                   int *out_listen_fd) {
    err_t rc = E__SUCCESS;
    skin_tcp_chacha20_listener_t *l = NULL;
    int fd = -1;

    VALIDATE_ARGS(addr, out_listener, out_listen_fd);

    *out_listener  = NULL;
    *out_listen_fd = -1;

    l = malloc(sizeof(skin_tcp_chacha20_listener_t));
    if (NULL == l) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    memset(l, 0, sizeof(*l));

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > fd) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        FREE(l);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)addr->port);

    if (0 == strcmp(addr->ip, "0.0.0.0") || 0 == strcmp(addr->ip, "*")) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (0 == inet_pton(AF_INET, addr->ip, &sa.sin_addr)) {
            LOG_ERROR("Invalid listen IP: %s", addr->ip);
            close(fd);
            FREE(l);
            FAIL(E__NET__SOCKET_BIND_FAILED);
        }
    }

    if (0 != bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
        LOG_ERROR("Failed to bind to %s:%d: %s", addr->ip, addr->port, strerror(errno));
        close(fd);
        FREE(l);
        FAIL(E__NET__SOCKET_BIND_FAILED);
    }

    if (0 != listen(fd, SOMAXCONN)) {
        LOG_ERROR("Failed to listen on %s:%d: %s", addr->ip, addr->port, strerror(errno));
        close(fd);
        FREE(l);
        FAIL(E__NET__SOCKET_LISTEN_FAILED);
    }

    int nb_flags = fcntl(fd, F_GETFL, 0);
    if (nb_flags >= 0) {
        fcntl(fd, F_SETFL, nb_flags | O_NONBLOCK);
    }

    LOG_INFO("Listening on %s:%d (tcp-chacha20)", addr->ip, addr->port);

    l->listen_fd   = fd;
    *out_listen_fd = fd;
    *out_listener  = (skin_listener_t *)l;

l_cleanup:
    return rc;
}

/* listener_accept */
static err_t ch20_listener_accept(skin_listener_t *sl,
                                   transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_tcp_chacha20_listener_t *l = (skin_tcp_chacha20_listener_t *)sl;
    transport_t *t = NULL;
    skin_tcp_chacha20_ctx_t *ctx = NULL;
    int client_fd = -1;

    VALIDATE_ARGS(l, out_transport);

    *out_transport = NULL;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));

    client_fd = accept(l->listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (0 > client_fd) {
        if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno) {
            FAIL(E__NET__SOCKET_ACCEPT_FAILED);
        }
        LOG_ERROR("Accept failed: %s", strerror(errno));
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ctx = ch20_ctx_alloc();
    if (NULL == ctx) {
        close(client_fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);

    t = ch20_alloc_transport(client_fd, 1, ip_str, client_port,
                              SKIN_TCP_CHACHA20__ops());
    if (NULL == t) {
        ch20_ctx_free(ctx);
        close(client_fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    t->skin_ctx = ctx;

    LOG_INFO("Accepted connection from %s:%d (fd=%d)", ip_str, client_port, client_fd);

    rc = ch20_do_handshake(t, 0 /* responder */);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Handshake failed for accepted connection fd=%d", client_fd);
        t->skin_ctx = NULL;
        TRANSPORT__free_base(t);
        ch20_ctx_free(ctx);
        close(client_fd);
        goto l_cleanup;
    }

    *out_transport = t;

l_cleanup:
    return rc;
}

/* listener_destroy */
static void ch20_listener_destroy(skin_listener_t *sl) {
    skin_tcp_chacha20_listener_t *l = (skin_tcp_chacha20_listener_t *)sl;
    if (NULL == l) {
        return;
    }
    if (l->listen_fd >= 0) {
        close(l->listen_fd);
        l->listen_fd = -1;
    }
    free(l);
}

/* connect */
static err_t ch20_connect(const char *ip, int port, int connect_timeout_sec,
                           transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    int fd = -1;
    transport_t *t = NULL;
    skin_tcp_chacha20_ctx_t *ctx = NULL;

    VALIDATE_ARGS(ip, out_transport);

    *out_transport = NULL;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > fd) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (-1 == flags || -1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        close(fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (0 == inet_pton(AF_INET, ip, &addr.sin_addr)) {
        struct hostent *he = gethostbyname(ip);
        if (NULL == he) {
            LOG_ERROR("Failed to resolve host: %s", ip);
            close(fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    LOG_INFO("Connecting to %s:%d...", ip, port);

    if (0 != connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        if (EINPROGRESS == errno) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);
            struct timeval tv;
            tv.tv_sec  = connect_timeout_sec;
            tv.tv_usec = 0;
            int sel = select(fd + 1, NULL, &write_fds, NULL, &tv);
            if (0 >= sel) {
                LOG_WARNING("Connect to %s:%d timed out after %ds", ip, port, connect_timeout_sec);
                close(fd);
                FAIL(E__NET__SOCKET_CONNECT_FAILED);
            }
            int err = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
            if (0 != err) {
                LOG_WARNING("Connect to %s:%d failed: %s", ip, port, strerror(err));
                close(fd);
                FAIL(E__NET__SOCKET_CONNECT_FAILED);
            }
        } else {
            LOG_WARNING("Failed to connect to %s:%d: %s", ip, port, strerror(errno));
            close(fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (-1 == fcntl(fd, F_SETFL, flags)) {
        close(fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    ctx = ch20_ctx_alloc();
    if (NULL == ctx) {
        close(fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    t = ch20_alloc_transport(fd, 0, ip, port, SKIN_TCP_CHACHA20__ops());
    if (NULL == t) {
        ch20_ctx_free(ctx);
        close(fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    t->skin_ctx = ctx;

    rc = ch20_do_handshake(t, 1 /* initiator */);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Handshake failed connecting to %s:%d", ip, port);
        t->skin_ctx = NULL;
        TRANSPORT__free_base(t);
        ch20_ctx_free(ctx);
        close(fd);
        goto l_cleanup;
    }

    LOG_INFO("Connected to %s:%d (fd=%d)", ip, port, fd);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* send_msg */
static err_t ch20_send_msg(transport_t *t, const protocol_msg_t *msg,
                            const uint8_t *data) {
    err_t rc = E__SUCCESS;
    skin_tcp_chacha20_ctx_t *ctx = (skin_tcp_chacha20_ctx_t *)t->skin_ctx;
    uint8_t *obf = NULL;

    VALIDATE_ARGS(t, msg, ctx);

    LOG_TRACE("SEND msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, "
              "data_len=%u, ttl=%u, channel=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length,
              msg->ttl, msg->channel_id, t->fd);

    size_t plain_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0) {
        plain_len += msg->data_length;
    }

    obf = malloc(plain_len);
    if (NULL == obf) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    size_t bytes_written = 0;
    rc = PROTOCOL__serialize(msg, data, obf, plain_len, &bytes_written);
    if (E__SUCCESS != rc) {
        FREE(obf);
        goto l_cleanup;
    }

    uint64_t counter = ctx->enc_send_counter++;
    ch20_crypt(obf, bytes_written, ctx->enc_send_key, counter);

    uint32_t net_len = htonl((uint32_t)bytes_written);
    rc = ch20_send_all(t->fd, (uint8_t *)&net_len, 4);
    if (E__SUCCESS != rc) {
        FREE(obf);
        goto l_cleanup;
    }

    rc = ch20_send_all(t->fd, (uint8_t *)&counter, 8);
    if (E__SUCCESS != rc) {
        FREE(obf);
        goto l_cleanup;
    }

    rc = ch20_send_all(t->fd, obf, bytes_written);
    FREE(obf);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    if (NULL != obf) FREE(obf);
    return rc;
}

/* recv_msg */
static err_t ch20_recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    skin_tcp_chacha20_ctx_t *ctx = (skin_tcp_chacha20_ctx_t *)t->skin_ctx;
    uint8_t len_buf[4];
    uint8_t nonce_buf[8];
    uint32_t frame_len = 0;
    uint8_t *frame = NULL;

    VALIDATE_ARGS(t, msg, data, ctx);

    *data = NULL;

    rc = ch20_recv_all(t->fd, len_buf, 4);
    FAIL_IF(E__SUCCESS != rc, rc);

    frame_len = ((uint32_t)len_buf[0] << 24) |
                ((uint32_t)len_buf[1] << 16) |
                ((uint32_t)len_buf[2] <<  8) |
                ((uint32_t)len_buf[3]);

    if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
        LOG_WARNING("Invalid frame length: %u on fd=%d", frame_len, t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    rc = ch20_recv_all(t->fd, nonce_buf, 8);
    FAIL_IF(E__SUCCESS != rc, rc);

    uint64_t counter = ((uint64_t)nonce_buf[0]) |
                       ((uint64_t)nonce_buf[1] << 8) |
                       ((uint64_t)nonce_buf[2] << 16) |
                       ((uint64_t)nonce_buf[3] << 24) |
                       ((uint64_t)nonce_buf[4] << 32) |
                       ((uint64_t)nonce_buf[5] << 40) |
                       ((uint64_t)nonce_buf[6] << 48) |
                       ((uint64_t)nonce_buf[7] << 56);

    frame = malloc(frame_len);
    if (NULL == frame) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    rc = ch20_recv_all(t->fd, frame, frame_len);
    FAIL_IF(E__SUCCESS != rc, rc);

    ch20_crypt(frame, frame_len, ctx->enc_recv_key, counter);

    size_t unserialize_data_len = 0;
    rc = PROTOCOL__unserialize(frame, frame_len, msg, data, &unserialize_data_len);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to unserialize message from fd %d", t->fd);
        FREE(frame);
        goto l_cleanup;
    }

    LOG_TRACE("RECV msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, "
              "data_len=%u, ttl=%u, channel=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length,
              msg->ttl, msg->channel_id, t->fd);

    FREE(frame);

l_cleanup:
    if (NULL != frame) FREE(frame);
    return rc;
}

/* transport_destroy */
static void ch20_transport_destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    skin_tcp_chacha20_ctx_t *ctx = (skin_tcp_chacha20_ctx_t *)t->skin_ctx;
    ch20_ctx_free(ctx);
    t->skin_ctx = NULL;

    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
}

/* ---- Vtable singleton --------------------------------------------------- */

static const skin_ops_t g_tcp_chacha20_skin = {
    .skin_id            = SKIN_ID__TCP_CHACHA20,
    .name               = "tcp-chacha20",
    .listener_create    = ch20_listener_create,
    .listener_accept    = ch20_listener_accept,
    .listener_destroy   = ch20_listener_destroy,
    .connect            = ch20_connect,
    .send_msg           = ch20_send_msg,
    .recv_msg           = ch20_recv_msg,
    .transport_destroy  = ch20_transport_destroy,
};

const skin_ops_t *SKIN_TCP_CHACHA20__ops(void) {
    return &g_tcp_chacha20_skin;
}

err_t SKIN_TCP_CHACHA20__register(void) {
    return SKIN__register(&g_tcp_chacha20_skin);
}
