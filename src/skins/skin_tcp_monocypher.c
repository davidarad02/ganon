/*
 * tcp-monocypher skin:
 *   TCP transport + X25519 key-exchange + BLAKE2b KDF + XChaCha20-Poly1305.
 *
 * Wire frame format (after handshake):
 *   [4  bytes] big-endian payload length (= nonce + mac + ciphertext)
 *   [24 bytes] nonce (16 zero bytes || 8-byte little-endian counter)
 *   [16 bytes] Poly1305 MAC
 *   [N  bytes] ciphertext of serialised protocol_msg_t + data
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "monocypher.h"
#include "protocol.h"
#include "skin.h"
#include "skins/skin_tcp_monocypher.h"
#include "transport.h"

#ifdef USE_EPOLL
#include <sys/epoll.h>
#include "network_epoll.h"
#endif

#ifdef USE_LIBSODIUM
#include <sodium.h>
#endif

/* ---- Constants ---------------------------------------------------------- */

#define TCPM_NONCE_SIZE     24
#define TCPM_MAC_SIZE       16
#define TCPM_FRAME_OVERHEAD 44   /* nonce + mac */

/* Stack buffer caps to avoid malloc on the hot path. */
#define TCPM_SEND_STACK_SIZE 131328
#define TCPM_RECV_STACK_SIZE 131400

/* ---- Per-connection opaque context -------------------------------------- */

typedef enum {
    TCPM_ENC__INIT = 0,
    TCPM_ENC__ESTABLISHED,
} tcpm_enc_state_t;

struct tcpm_outbuf {
    uint8_t *data;
    size_t   len;
    size_t   sent;
    struct tcpm_outbuf *next;
};

typedef struct {
    tcpm_enc_state_t enc_state;
    uint8_t enc_ephemeral_priv[32];
    uint8_t enc_ephemeral_pub[32];
    uint8_t enc_send_key[32];
    uint8_t enc_recv_key[32];
    uint64_t enc_send_nonce;
    uint64_t enc_recv_nonce;
    uint8_t enc_session_id[8];
    int enc_is_initiator;
    uint8_t enc_send_subkey[32];
    uint8_t enc_recv_subkey[32];

    /* Outbound queue for epoll mode. */
    struct tcpm_outbuf *out_head;
    struct tcpm_outbuf *out_tail;
    pthread_mutex_t out_mutex;
    int out_has_data;

    /* Partial-frame recv buffer for epoll mode. */
    uint8_t *recv_buf;
    size_t   recv_buf_len;
    size_t   recv_buf_cap;
} skin_tcpm_ctx_t;

/* ---- Per-listener state ------------------------------------------------- */

typedef struct {
    int      listen_fd;
    addr_t   addr;
} skin_tcpm_listener_t;

/* ---- Helpers: random, key-derivation, nonce ----------------------------- */

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
                        uint8_t send_key[32], uint8_t recv_key[32],
                        uint8_t session_id[8]) {
    uint8_t out[32];
    const char send_ctx = 'S';
    const char recv_ctx = 'R';
    const char sess_ctx = 'I';

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&send_ctx, 1);
    memcpy(send_key, out, 32);

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&recv_ctx, 1);
    memcpy(recv_key, out, 32);

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&sess_ctx, 1);
    memcpy(session_id, out, 8);

    crypto_wipe(out, sizeof(out));
}

static void build_nonce(uint8_t nonce[24], uint64_t counter) {
    memset(nonce, 0, 24);
    nonce[16] = (uint8_t)(counter);
    nonce[17] = (uint8_t)(counter >> 8);
    nonce[18] = (uint8_t)(counter >> 16);
    nonce[19] = (uint8_t)(counter >> 24);
    nonce[20] = (uint8_t)(counter >> 32);
    nonce[21] = (uint8_t)(counter >> 40);
    nonce[22] = (uint8_t)(counter >> 48);
    nonce[23] = (uint8_t)(counter >> 56);
}

/* ---- Raw TCP recv/send (blocking) --------------------------------------- */

static err_t tcpm_recv_all(int fd, uint8_t *buf, size_t len) {
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

static err_t tcpm_send_all(int fd, const uint8_t *buf, size_t len) {
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

/* ---- Transport allocation helpers --------------------------------------- */

static transport_t *tcpm_alloc_transport(int fd, int is_incoming,
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

static skin_tcpm_ctx_t *tcpm_ctx_alloc(void) {
    skin_tcpm_ctx_t *ctx = malloc(sizeof(skin_tcpm_ctx_t));
    if (NULL == ctx) {
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->out_mutex, NULL);
    return ctx;
}

static void tcpm_ctx_free(skin_tcpm_ctx_t *ctx) {
    if (NULL == ctx) {
        return;
    }
    crypto_wipe(ctx->enc_ephemeral_priv, 32);
    crypto_wipe(ctx->enc_send_key, 32);
    crypto_wipe(ctx->enc_recv_key, 32);

    pthread_mutex_lock(&ctx->out_mutex);
    struct tcpm_outbuf *ob = ctx->out_head;
    while (NULL != ob) {
        struct tcpm_outbuf *next = ob->next;
        FREE(ob->data);
        FREE(ob);
        ob = next;
    }
    ctx->out_head = NULL;
    ctx->out_tail = NULL;
    pthread_mutex_unlock(&ctx->out_mutex);
    pthread_mutex_destroy(&ctx->out_mutex);

    FREE(ctx->recv_buf);
    free(ctx);
}

/* ---- Handshake ---------------------------------------------------------- */

static err_t tcpm_do_handshake(transport_t *t, int is_initiator) {
    err_t rc = E__SUCCESS;
    skin_tcpm_ctx_t *ctx = (skin_tcpm_ctx_t *)t->skin_ctx;
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
        rc = tcpm_send_all(t->fd, ctx->enc_ephemeral_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
        rc = tcpm_recv_all(t->fd, peer_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
    } else {
        rc = tcpm_recv_all(t->fd, peer_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
        rc = tcpm_send_all(t->fd, ctx->enc_ephemeral_pub, 32);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

    crypto_x25519(shared, ctx->enc_ephemeral_priv, peer_pub);

    if (is_initiator) {
        derive_keys(shared, ctx->enc_send_key, ctx->enc_recv_key, ctx->enc_session_id);
    } else {
        derive_keys(shared, ctx->enc_recv_key, ctx->enc_send_key, ctx->enc_session_id);
    }

    ctx->enc_send_nonce = 0;
    ctx->enc_recv_nonce = 0;
    ctx->enc_state = TCPM_ENC__ESTABLISHED;

#ifndef USE_LIBSODIUM
    {
        uint8_t zero_nonce_prefix[16] = {0};
        crypto_chacha20_h(ctx->enc_send_subkey, ctx->enc_send_key, zero_nonce_prefix);
        crypto_chacha20_h(ctx->enc_recv_subkey, ctx->enc_recv_key, zero_nonce_prefix);
    }
#endif

    LOG_DEBUG("Encryption handshake complete on fd=%d (session_id=%02x%02x%02x%02x)",
              t->fd, ctx->enc_session_id[0], ctx->enc_session_id[1],
              ctx->enc_session_id[2], ctx->enc_session_id[3]);

l_cleanup:
    crypto_wipe(shared, sizeof(shared));
    return rc;
}

/* ---- Decrypt a single complete encrypted frame -------------------------- */

static err_t tcpm_decrypt_frame(transport_t *t, uint8_t *payload, size_t payload_len,
                                 protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    skin_tcpm_ctx_t *ctx = (skin_tcpm_ctx_t *)t->skin_ctx;
    uint8_t *plaintext = NULL;

    VALIDATE_ARGS(t, payload, msg, data);

    *data = NULL;

    if (payload_len < (TCPM_NONCE_SIZE + TCPM_MAC_SIZE)) {
        LOG_WARNING("Frame too small for nonce+mac: %zu on fd=%d", payload_len, t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    uint8_t *nonce      = payload;
    uint8_t *mac        = payload + TCPM_NONCE_SIZE;
    uint8_t *ciphertext = payload + TCPM_NONCE_SIZE + TCPM_MAC_SIZE;
    size_t ciphertext_len = payload_len - TCPM_NONCE_SIZE - TCPM_MAC_SIZE;

    uint64_t recv_counter = (uint64_t)nonce[16] |
                           ((uint64_t)nonce[17] << 8)  |
                           ((uint64_t)nonce[18] << 16) |
                           ((uint64_t)nonce[19] << 24) |
                           ((uint64_t)nonce[20] << 32) |
                           ((uint64_t)nonce[21] << 40) |
                           ((uint64_t)nonce[22] << 48) |
                           ((uint64_t)nonce[23] << 56);

    if (recv_counter != ctx->enc_recv_nonce) {
        LOG_WARNING("Replay detected on fd=%d: expected %llu, got %llu",
                    t->fd, (unsigned long long)ctx->enc_recv_nonce,
                    (unsigned long long)recv_counter);
        FAIL(E__CRYPTO__REPLAY_DETECTED);
    }
    ctx->enc_recv_nonce++;

    uint8_t plaintext_stack[TCPM_RECV_STACK_SIZE];
    int plaintext_heap = (ciphertext_len > TCPM_RECV_STACK_SIZE);
    if (plaintext_heap) {
        plaintext = malloc(ciphertext_len);
        FAIL_IF(NULL == plaintext, E__INVALID_ARG_NULL_POINTER);
    } else {
        plaintext = plaintext_stack;
    }

#ifdef USE_LIBSODIUM
    if (0 != crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
            plaintext, NULL, ciphertext, ciphertext_len, mac,
            NULL, 0, nonce, ctx->enc_recv_key)) {
        LOG_WARNING("Decryption failed on fd=%d", t->fd);
        if (plaintext_heap) FREE(plaintext);
        FAIL(E__CRYPTO__DECRYPT_FAILED);
    }
#else
    {
        crypto_aead_ctx aead;
        memcpy(aead.key, ctx->enc_recv_subkey, 32);
        memcpy(aead.nonce, nonce + 16, 8);
        aead.counter = 0;
        if (0 != crypto_aead_read(&aead, plaintext, mac,
                                  NULL, 0, ciphertext, ciphertext_len)) {
            LOG_WARNING("Decryption failed on fd=%d", t->fd);
            if (plaintext_heap) FREE(plaintext);
            FAIL(E__CRYPTO__DECRYPT_FAILED);
        }
    }
#endif

    size_t unserialize_data_len = 0;
    rc = PROTOCOL__unserialize(plaintext, ciphertext_len, msg, data, &unserialize_data_len);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to unserialize decrypted message from fd %d", t->fd);
        if (plaintext_heap) FREE(plaintext);
        goto l_cleanup;
    }

    LOG_TRACE("RECV msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, data_len=%u, ttl=%u, channel=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length, msg->ttl, msg->channel_id, t->fd);

    if (plaintext_heap) FREE(plaintext);

l_cleanup:
    return rc;
}

/* ---- Vtable implementations --------------------------------------------- */

/* listener_create */
static err_t tcpm_listener_create(const addr_t *addr,
                                   skin_listener_t **out_listener,
                                   int *out_listen_fd) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(addr, out_listener, out_listen_fd);

    *out_listener  = NULL;
    *out_listen_fd = -1;

    skin_tcpm_listener_t *l = malloc(sizeof(skin_tcpm_listener_t));
    FAIL_IF(NULL == l, E__INVALID_ARG_NULL_POINTER);

    l->addr = *addr;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > fd) {
        LOG_ERROR("Failed to create listen socket: %s", strerror(errno));
        FREE(l);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

    LOG_INFO("Listening on %s:%d (tcp-monocypher)", addr->ip, addr->port);

    l->listen_fd   = fd;
    *out_listen_fd = fd;
    *out_listener  = (skin_listener_t *)l;

l_cleanup:
    return rc;
}

/* listener_accept */
static err_t tcpm_listener_accept(skin_listener_t *sl,
                                   transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_tcpm_listener_t *l = (skin_tcpm_listener_t *)sl;
    transport_t *t = NULL;
    skin_tcpm_ctx_t *ctx = NULL;
    int client_fd = -1;

    VALIDATE_ARGS(l, out_transport);

    *out_transport = NULL;

    /* accept() — blocks until a connection arrives */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));

    client_fd = accept(l->listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (0 > client_fd) {
        if (EINTR == errno) {
            FAIL(E__NET__SOCKET_ACCEPT_FAILED);
        }
        LOG_ERROR("Accept failed: %s", strerror(errno));
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ctx = tcpm_ctx_alloc();
    if (NULL == ctx) {
        close(client_fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);

    t = tcpm_alloc_transport(client_fd, 1, ip_str, client_port, SKIN_TCPM__ops());
    if (NULL == t) {
        tcpm_ctx_free(ctx);
        close(client_fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    t->skin_ctx = ctx;

    LOG_INFO("Accepted connection from %s:%d (fd=%d)", ip_str, client_port, client_fd);

    rc = tcpm_do_handshake(t, 0 /* responder */);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Handshake failed for accepted connection fd=%d", client_fd);
        t->skin_ctx = NULL;   /* ctx freed below */
        TRANSPORT__free_base(t);
        tcpm_ctx_free(ctx);
        close(client_fd);
        goto l_cleanup;
    }

    *out_transport = t;

l_cleanup:
    return rc;
}

/* listener_destroy */
static void tcpm_listener_destroy(skin_listener_t *sl) {
    skin_tcpm_listener_t *l = (skin_tcpm_listener_t *)sl;
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
static err_t tcpm_connect(const char *ip, int port, int connect_timeout_sec,
                           transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    int fd = -1;
    transport_t *t = NULL;
    skin_tcpm_ctx_t *ctx = NULL;

    VALIDATE_ARGS(ip, out_transport);

    *out_transport = NULL;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > fd) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    /* Non-blocking connect with timeout */
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

    /* Restore blocking mode for the handshake */
    if (-1 == fcntl(fd, F_SETFL, flags)) {
        close(fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    ctx = tcpm_ctx_alloc();
    if (NULL == ctx) {
        close(fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    t = tcpm_alloc_transport(fd, 0, ip, port, SKIN_TCPM__ops());
    if (NULL == t) {
        tcpm_ctx_free(ctx);
        close(fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    t->skin_ctx = ctx;

    rc = tcpm_do_handshake(t, 1 /* initiator */);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Handshake failed connecting to %s:%d", ip, port);
        t->skin_ctx = NULL;
        TRANSPORT__free_base(t);
        tcpm_ctx_free(ctx);
        close(fd);
        goto l_cleanup;
    }

    LOG_INFO("Connected to %s:%d (fd=%d)", ip, port, fd);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* send_msg */
static err_t tcpm_send_msg(transport_t *t, const protocol_msg_t *msg,
                            const uint8_t *data) {
    err_t rc = E__SUCCESS;
    skin_tcpm_ctx_t *ctx = (skin_tcpm_ctx_t *)t->skin_ctx;
    uint8_t *frame = NULL;

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

    uint8_t  plain_stack[TCPM_SEND_STACK_SIZE];
    uint8_t *plain_buf;
    int      plain_heap = (plain_len > TCPM_SEND_STACK_SIZE);
    if (plain_heap) {
        plain_buf = malloc(plain_len);
        FAIL_IF(NULL == plain_buf, E__INVALID_ARG_NULL_POINTER);
    } else {
        plain_buf = plain_stack;
    }

    size_t bytes_written = 0;
    rc = PROTOCOL__serialize(msg, data, plain_buf, plain_len, &bytes_written);
    if (E__SUCCESS != rc) {
        if (plain_heap) FREE(plain_buf);
        goto l_cleanup;
    }

    size_t ciphertext_len = bytes_written;
    size_t payload_len    = TCPM_NONCE_SIZE + TCPM_MAC_SIZE + ciphertext_len;
    size_t total_len      = 4 + payload_len;

    frame = malloc(total_len);
    if (NULL == frame) {
        if (plain_heap) FREE(plain_buf);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    uint32_t net_len = htonl((uint32_t)payload_len);
    memcpy(frame, &net_len, 4);

    uint8_t *nonce      = frame + 4;
    uint8_t *mac        = frame + 4 + TCPM_NONCE_SIZE;
    uint8_t *ciphertext = frame + 4 + TCPM_NONCE_SIZE + TCPM_MAC_SIZE;

    build_nonce(nonce, ctx->enc_send_nonce);
    ctx->enc_send_nonce++;

#ifdef USE_LIBSODIUM
    crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
        ciphertext, mac, NULL,
        plain_buf, bytes_written,
        NULL, 0, NULL, nonce, ctx->enc_send_key);
#else
    {
        crypto_aead_ctx aead;
        memcpy(aead.key, ctx->enc_send_subkey, 32);
        memcpy(aead.nonce, nonce + 16, 8);
        aead.counter = 0;
        crypto_aead_write(&aead, ciphertext, mac, NULL, 0,
                          plain_buf, bytes_written);
    }
#endif

    if (plain_heap) FREE(plain_buf);

    if (t->is_nonblocking) {
        /* Epoll mode: hand frame to outbound queue. */
        pthread_mutex_lock(&ctx->out_mutex);
        struct tcpm_outbuf *ob = malloc(sizeof(struct tcpm_outbuf));
        if (NULL == ob) {
            pthread_mutex_unlock(&ctx->out_mutex);
            FREE(frame);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }
        ob->data = frame;
        ob->len  = total_len;
        ob->sent = 0;
        ob->next = NULL;
        if (NULL == ctx->out_tail) {
            ctx->out_head = ob;
            ctx->out_tail = ob;
        } else {
            ctx->out_tail->next = ob;
            ctx->out_tail = ob;
        }
        ctx->out_has_data = 1;
        frame = NULL;  /* ownership transferred */
#ifdef USE_EPOLL
        if (g_epoll_fd >= 0) {
            struct epoll_event ee;
            ee.events   = EPOLLIN | EPOLLOUT;
            ee.data.ptr = t;
            epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, t->fd, &ee);
        }
#endif
        pthread_mutex_unlock(&ctx->out_mutex);
    } else {
        rc = tcpm_send_all(t->fd, frame, total_len);
        FREE(frame);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

l_cleanup:
    if (NULL != frame) FREE(frame);
    return rc;
}

/* recv_msg */
static err_t tcpm_recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    uint8_t len_buf[4];
    uint32_t frame_len = 0;
    uint8_t *frame = NULL;

    VALIDATE_ARGS(t, msg, data);

    *data = NULL;

    rc = tcpm_recv_all(t->fd, len_buf, 4);
    FAIL_IF(E__SUCCESS != rc, rc);

    frame_len = ((uint32_t)len_buf[0] << 24) |
                ((uint32_t)len_buf[1] << 16) |
                ((uint32_t)len_buf[2] <<  8) |
                ((uint32_t)len_buf[3]);

    if (frame_len < (TCPM_NONCE_SIZE + TCPM_MAC_SIZE) || frame_len > 300000) {
        LOG_WARNING("Invalid encrypted frame length: %u on fd=%d", frame_len, t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    uint8_t frame_stack[TCPM_RECV_STACK_SIZE];
    int frame_heap = (frame_len > TCPM_RECV_STACK_SIZE);
    if (frame_heap) {
        frame = malloc(frame_len);
        FAIL_IF(NULL == frame, E__INVALID_ARG_NULL_POINTER);
    } else {
        frame = frame_stack;
    }

    rc = tcpm_recv_all(t->fd, frame, frame_len);
    if (E__SUCCESS != rc) {
        if (frame_heap) FREE(frame);
        goto l_cleanup;
    }

    rc = tcpm_decrypt_frame(t, frame, frame_len, msg, data);
    if (frame_heap) FREE(frame);

l_cleanup:
    return rc;
}

/* on_readable (epoll mode) */
static err_t tcpm_on_readable(transport_t *t, network_message_cb_t cb) {
    err_t rc = E__SUCCESS;
    skin_tcpm_ctx_t *ctx = (skin_tcpm_ctx_t *)t->skin_ctx;

    VALIDATE_ARGS(t, ctx);

    /* Grow recv buffer if needed */
    if (ctx->recv_buf_len + 4096 > ctx->recv_buf_cap) {
        size_t new_cap = ctx->recv_buf_cap ? ctx->recv_buf_cap * 2 : 16384;
        if (new_cap > 300000 + 44) {
            new_cap = 300000 + 44;
        }
        uint8_t *new_buf = realloc(ctx->recv_buf, new_cap);
        FAIL_IF(NULL == new_buf, E__INVALID_ARG_NULL_POINTER);
        ctx->recv_buf     = new_buf;
        ctx->recv_buf_cap = new_cap;
    }

    ssize_t n = recv(t->fd,
                     ctx->recv_buf + ctx->recv_buf_len,
                     ctx->recv_buf_cap - ctx->recv_buf_len,
                     0);
    if (0 > n) {
        if (EAGAIN == errno || EWOULDBLOCK == errno) {
            return E__SUCCESS;
        }
        LOG_WARNING("recv error on fd=%d: %s", t->fd, strerror(errno));
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    } else if (0 == n) {
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }
    ctx->recv_buf_len += (size_t)n;

    /* Process complete frames */
    while (ctx->recv_buf_len >= 4) {
        uint32_t payload_len = ((uint32_t)ctx->recv_buf[0] << 24) |
                               ((uint32_t)ctx->recv_buf[1] << 16) |
                               ((uint32_t)ctx->recv_buf[2] <<  8) |
                               ((uint32_t)ctx->recv_buf[3]);

        if (payload_len < (TCPM_NONCE_SIZE + TCPM_MAC_SIZE) || payload_len > 300000) {
            LOG_WARNING("Invalid encrypted frame length in epoll: %u on fd=%d",
                        payload_len, t->fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }

        size_t total = 4 + payload_len;
        if (ctx->recv_buf_len < total) {
            break;
        }

        protocol_msg_t msg;
        uint8_t *data = NULL;
        rc = tcpm_decrypt_frame(t, ctx->recv_buf + 4, payload_len, &msg, &data);
        if (E__SUCCESS != rc) {
            goto l_cleanup;
        }

        if (NULL != cb) {
            cb(t, &msg, data, msg.data_length);
        }
        free(data);

        memmove(ctx->recv_buf, ctx->recv_buf + total, ctx->recv_buf_len - total);
        ctx->recv_buf_len -= total;
    }

l_cleanup:
    return rc;
}

/* on_writable (epoll mode) */
static err_t tcpm_on_writable(transport_t *t, int *would_block) {
    skin_tcpm_ctx_t *ctx = (skin_tcpm_ctx_t *)t->skin_ctx;

    if (NULL != would_block) {
        *would_block = 0;
    }
    if (NULL == ctx) {
        return E__INVALID_ARG_NULL_POINTER;
    }

    pthread_mutex_lock(&ctx->out_mutex);
    while (NULL != ctx->out_head) {
        struct tcpm_outbuf *ob = ctx->out_head;
        ssize_t n = send(t->fd, ob->data + ob->sent, ob->len - ob->sent, MSG_NOSIGNAL);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                if (NULL != would_block) *would_block = 1;
                break;
            }
            LOG_WARNING("send failed on fd %d: %s", t->fd, strerror(errno));
            pthread_mutex_unlock(&ctx->out_mutex);
            return E__NET__SEND_FAILED;
        }
        ob->sent += (size_t)n;
        if (ob->sent >= ob->len) {
            ctx->out_head = ob->next;
            if (NULL == ctx->out_head) ctx->out_tail = NULL;
            FREE(ob->data);
            FREE(ob);
        }
    }
    if (NULL == ctx->out_head) {
        ctx->out_has_data = 0;
#ifdef USE_EPOLL
        if (t->is_nonblocking && g_epoll_fd >= 0) {
            struct epoll_event ee;
            ee.events   = EPOLLIN;
            ee.data.ptr = t;
            epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, t->fd, &ee);
        }
#endif
    }
    pthread_mutex_unlock(&ctx->out_mutex);

    return E__SUCCESS;
}

/* enqueue_outbuf (epoll mode) */
static err_t tcpm_enqueue_outbuf(transport_t *t, const protocol_msg_t *msg,
                                  const uint8_t *data) {
    /* For tcp-monocypher the enqueue path is folded into send_msg (which
     * detects is_nonblocking and enqueues there).  This entry point is
     * provided for completeness; callers should prefer send_msg. */
    return tcpm_send_msg(t, msg, data);
}

/* transport_destroy */
static void tcpm_transport_destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    skin_tcpm_ctx_t *ctx = (skin_tcpm_ctx_t *)t->skin_ctx;
    tcpm_ctx_free(ctx);
    t->skin_ctx = NULL;

    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
}

/* ---- Vtable singleton --------------------------------------------------- */

static const skin_ops_t g_tcpm_skin = {
    .skin_id            = SKIN_ID__TCP_MONOCYPHER,
    .name               = "tcp-monocypher",
    .listener_create    = tcpm_listener_create,
    .listener_accept    = tcpm_listener_accept,
    .listener_destroy   = tcpm_listener_destroy,
    .connect            = tcpm_connect,
    .send_msg           = tcpm_send_msg,
    .recv_msg           = tcpm_recv_msg,
    .on_readable        = tcpm_on_readable,
    .on_writable        = tcpm_on_writable,
    .enqueue_outbuf     = tcpm_enqueue_outbuf,
    .transport_destroy  = tcpm_transport_destroy,
};

const skin_ops_t *SKIN_TCPM__ops(void) {
    return &g_tcpm_skin;
}

err_t SKIN_TCPM__register(void) {
    return SKIN__register(&g_tcpm_skin);
}
