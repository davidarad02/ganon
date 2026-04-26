/*
 * Shared mbedTLS plumbing for tcp-tls13 and tcp-tls12 skins.
 *
 * Both skins are thin wrappers around these helpers: they differ only in
 * the min/max TLS version and whether ALPN is set.
 *
 * Wire frame format (inside the TLS record stream):
 *   [4 bytes] big-endian payload length
 *   [N bytes] PROTOCOL__serialize(msg) || data   (identical to tcp-plain)
 */

#include "skins/tls_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "protocol.h"
#include "transport.h"

/* ── Bio callbacks (blocking TCP I/O for mbedtls_ssl_set_bio) ─────────── */

static int tls_net_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0) {
        if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int tls_net_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0) {
        if (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (0 == n) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return (int)n;
}

/* ── Blocking write/read helpers ──────────────────────────────────────── */

static err_t tls_write_all(mbedtls_ssl_context *ssl, const uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;
    size_t total = 0;
    while (total < len) {
        int n = mbedtls_ssl_write(ssl, buf + total, len - total);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            continue;
        }
        if (n <= 0) {
            LOG_WARNING("mbedtls_ssl_write failed: -0x%04x", (unsigned int)-n);
            FAIL(E__TLS__SEND_FAILED);
        }
        total += (size_t)n;
    }
l_cleanup:
    return rc;
}

static err_t tls_read_all(mbedtls_ssl_context *ssl, uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;
    size_t total = 0;
    while (total < len) {
        int n = mbedtls_ssl_read(ssl, buf + total, len - total);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
            LOG_DEBUG("TLS peer closed connection");
            FAIL(E__TLS__CONN_CLOSED);
        }
        if (n < 0) {
            LOG_WARNING("mbedtls_ssl_read failed: -0x%04x", (unsigned int)-n);
            FAIL(E__TLS__RECV_FAILED);
        }
        total += (size_t)n;
    }
l_cleanup:
    return rc;
}

/* ── TCP socket helpers (identical to tcp-plain) ──────────────────────── */

static int tcp_create_listener(const addr_t *addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

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
            close(fd);
            return -1;
        }
    }

    if (0 != bind(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
        0 != listen(fd, SOMAXCONN)) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static int tcp_connect(const char *ip, int port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd); return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (0 == inet_pton(AF_INET, ip, &addr.sin_addr)) {
        struct hostent *he = gethostbyname(ip);
        if (!he) { close(fd); return -1; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    if (0 != connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        if (EINPROGRESS != errno) { close(fd); return -1; }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { close(fd); return -1; }
    }

    /* Restore blocking mode and set TCP_NODELAY. */
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return fd;
}

/* ── Cert generation ──────────────────────────────────────────────────── */

int tls_common_generate_cert(mbedtls_x509_crt        *cert,
                              mbedtls_pk_context      *pkey,
                              mbedtls_entropy_context *entropy,
                              uint8_t                **out_cert_der,
                              size_t                  *out_cert_der_len) {
    int ret = -1;
    mbedtls_x509write_cert   crt;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "ganon_tls_gen_cert";
    unsigned char cert_buf[4096];
    int cert_len;

    mbedtls_x509write_crt_init(&crt);
    mbedtls_pk_init(pkey);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, entropy,
                                     (const unsigned char *)pers,
                                     strlen(pers))) != 0)
        goto cleanup;

    if ((ret = mbedtls_pk_setup(pkey,
                                mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0)
        goto cleanup;
    if ((ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                   mbedtls_pk_ec(*pkey),
                                   mbedtls_ctr_drbg_random, &ctr_drbg)) != 0)
        goto cleanup;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    {
        unsigned char serial[] = { 0x01 };
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    }
    mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "21260101000000");
    mbedtls_x509write_crt_set_subject_name(&crt, "CN=ganon");
    mbedtls_x509write_crt_set_issuer_name(&crt,  "CN=ganon");
    mbedtls_x509write_crt_set_subject_key(&crt, pkey);
    mbedtls_x509write_crt_set_issuer_key(&crt,  pkey);

    cert_len = mbedtls_x509write_crt_der(&crt, cert_buf, sizeof(cert_buf),
                                         mbedtls_ctr_drbg_random, &ctr_drbg);
    if (cert_len < 0) { ret = cert_len; goto cleanup; }

    /* mbedtls_x509write_crt_der writes at END of buffer */
    const uint8_t *der_start = cert_buf + sizeof(cert_buf) - (size_t)cert_len;

    /* Parse into cert structure so mbedTLS SSL can use it */
    mbedtls_x509_crt_init(cert);
    ret = mbedtls_x509_crt_parse_der(cert, der_start, (size_t)cert_len);
    if (ret != 0) goto cleanup;

    /* Optionally return a malloc'd DER copy (for picotls) */
    if (out_cert_der && out_cert_der_len) {
        *out_cert_der = malloc((size_t)cert_len);
        if (!*out_cert_der) { ret = -1; goto cleanup; }
        memcpy(*out_cert_der, der_start, (size_t)cert_len);
        *out_cert_der_len = (size_t)cert_len;
    }

    ret = 0;

cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    return ret;
}

/* ── PSA init (idempotent) ────────────────────────────────────────────── */

static void tls_ensure_psa_init(void) {
    static int psa_initialised = 0;
    if (!psa_initialised) {
        psa_crypto_init();
        psa_initialised = 1;
    }
}

/* ── Per-connection context ──────────────────────────────────────────── */

static tls_common_ctx_t *tls_ctx_alloc(int fd) {
    tls_common_ctx_t *ctx = calloc(1, sizeof(tls_common_ctx_t));
    if (!ctx) return NULL;
    ctx->fd = fd;
    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    return ctx;
}

static void tls_ctx_free(tls_common_ctx_t *ctx) {
    if (!ctx) return;
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    free(ctx);
}

/* ── listener_create ─────────────────────────────────────────────────── */

err_t tls_common_listener_create(const addr_t              *addr,
                                  mbedtls_ssl_protocol_version min_ver,
                                  mbedtls_ssl_protocol_version max_ver,
                                  const char                *alpn,
                                  const skin_ops_t          *skin,
                                  skin_listener_t          **out_listener,
                                  int                       *out_listen_fd) {
    err_t rc = E__SUCCESS;
    tls_common_listener_t *l = NULL;
    int fd = -1;

    VALIDATE_ARGS(addr, skin, out_listener, out_listen_fd);

    tls_ensure_psa_init();

    *out_listener  = NULL;
    *out_listen_fd = -1;

    l = calloc(1, sizeof(tls_common_listener_t));
    if (!l) FAIL(E__TLS__ALLOC_FAILED);

    mbedtls_entropy_init(&l->entropy);

    if (tls_common_generate_cert(&l->cert, &l->pkey, &l->entropy, NULL, NULL) != 0) {
        LOG_ERROR("TLS: cert generation failed");
        FAIL(E__TLS__INIT_FAILED);
    }

    fd = tcp_create_listener(addr);
    if (fd < 0) {
        LOG_ERROR("TLS: failed to create listener on %s:%d: %s",
                  addr->ip, addr->port, strerror(errno));
        FAIL(E__NET__SOCKET_BIND_FAILED);
    }

    l->listen_fd = fd;
    l->addr      = *addr;
    l->min_ver   = min_ver;
    l->max_ver   = max_ver;
    l->skin      = skin;

    if (alpn && alpn[0]) {
        strncpy(l->alpn_str, alpn, sizeof(l->alpn_str) - 1);
        l->alpn_list[0] = l->alpn_str;
        l->alpn_list[1] = NULL;
    }

    LOG_INFO("Listening on %s:%d (%s)", addr->ip, addr->port, skin->name);

    *out_listen_fd = fd;
    *out_listener  = (skin_listener_t *)l;

l_cleanup:
    if (E__SUCCESS != rc) {
        if (fd >= 0) close(fd);
        if (l) {
            mbedtls_x509_crt_free(&l->cert);
            mbedtls_pk_free(&l->pkey);
            mbedtls_entropy_free(&l->entropy);
            free(l);
        }
    }
    return rc;
}

/* ── listener_accept ─────────────────────────────────────────────────── */

err_t tls_common_listener_accept(skin_listener_t *sl,
                                  transport_t    **out_transport) {
    err_t rc = E__SUCCESS;
    tls_common_listener_t *l = (tls_common_listener_t *)sl;
    tls_common_ctx_t *ctx = NULL;
    transport_t *t = NULL;
    int client_fd = -1;

    VALIDATE_ARGS(l, out_transport);
    *out_transport = NULL;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));

    client_fd = accept(l->listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    /* Make the accepted socket blocking; clear the nonblock flag from listen fd. */
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ctx = tls_ctx_alloc(client_fd);
    if (!ctx) {
        close(client_fd);
        FAIL(E__TLS__ALLOC_FAILED);
    }

    /* Seed per-connection ctr_drbg from its own entropy context */
    if (mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                               NULL, 0) != 0) {
        LOG_ERROR("TLS: ctr_drbg seed failed");
        tls_ctx_free(ctx);
        close(client_fd);
        FAIL(E__TLS__INIT_FAILED);
    }

    /* SSL config for this server connection */
    if (mbedtls_ssl_config_defaults(&ctx->cfg,
                                    MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        tls_ctx_free(ctx);
        close(client_fd);
        FAIL(E__TLS__INIT_FAILED);
    }
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, l->min_ver);
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, l->max_ver);
    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_NONE);

    if (mbedtls_ssl_conf_own_cert(&ctx->cfg, &l->cert, &l->pkey) != 0) {
        tls_ctx_free(ctx);
        close(client_fd);
        FAIL(E__TLS__INIT_FAILED);
    }

    if (l->alpn_list[0]) {
        mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, l->alpn_list);
    }

    if (mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg) != 0) {
        tls_ctx_free(ctx);
        close(client_fd);
        FAIL(E__TLS__INIT_FAILED);
    }

    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->fd, tls_net_send, tls_net_recv, NULL);

    int hs_ret;
    while ((hs_ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (hs_ret != MBEDTLS_ERR_SSL_WANT_READ &&
            hs_ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOG_WARNING("TLS server handshake failed: -0x%04x", (unsigned int)-hs_ret);
            tls_ctx_free(ctx);
            close(client_fd);
            FAIL(E__TLS__HANDSHAKE_FAILED);
        }
    }

    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);

    LOG_INFO("TLS %s accepted from %s:%d", l->skin->name, ip_str, client_port);

    t = TRANSPORT__alloc_base(client_fd, l->skin);
    if (!t) {
        tls_ctx_free(ctx);
        close(client_fd);
        FAIL(E__TLS__ALLOC_FAILED);
    }
    t->is_incoming = 1;
    snprintf(t->client_ip, INET_ADDRSTRLEN, "%s", ip_str);
    t->client_port = client_port;
    t->skin_ctx    = ctx;

    *out_transport = t;

l_cleanup:
    return rc;
}

/* ── listener_destroy ────────────────────────────────────────────────── */

void tls_common_listener_destroy(skin_listener_t *sl) {
    tls_common_listener_t *l = (tls_common_listener_t *)sl;
    if (!l) return;
    if (l->listen_fd >= 0) {
        close(l->listen_fd);
        l->listen_fd = -1;
    }
    mbedtls_x509_crt_free(&l->cert);
    mbedtls_pk_free(&l->pkey);
    mbedtls_entropy_free(&l->entropy);
    free(l);
}

/* ── connect ─────────────────────────────────────────────────────────── */

err_t tls_common_connect(const char               *ip,
                          int                       port,
                          int                       connect_timeout_sec,
                          mbedtls_ssl_protocol_version min_ver,
                          mbedtls_ssl_protocol_version max_ver,
                          const char               *alpn,
                          const skin_ops_t         *skin,
                          transport_t             **out_transport) {
    err_t rc = E__SUCCESS;
    tls_common_ctx_t *ctx = NULL;
    transport_t *t = NULL;
    int fd = -1;

    VALIDATE_ARGS(ip, skin, out_transport);
    *out_transport = NULL;

    tls_ensure_psa_init();

    fd = tcp_connect(ip, port, connect_timeout_sec);
    if (fd < 0) {
        LOG_WARNING("TLS: TCP connect to %s:%d failed", ip, port);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    ctx = tls_ctx_alloc(fd);
    if (!ctx) {
        close(fd);
        FAIL(E__TLS__ALLOC_FAILED);
    }

    if (mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                               NULL, 0) != 0) {
        tls_ctx_free(ctx);
        close(fd);
        FAIL(E__TLS__INIT_FAILED);
    }

    if (mbedtls_ssl_config_defaults(&ctx->cfg,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        tls_ctx_free(ctx);
        close(fd);
        FAIL(E__TLS__INIT_FAILED);
    }
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, min_ver);
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, max_ver);
    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_NONE);

    if (alpn && alpn[0]) {
        strncpy(ctx->alpn_str, alpn, sizeof(ctx->alpn_str) - 1);
        ctx->alpn_list[0] = ctx->alpn_str;
        ctx->alpn_list[1] = NULL;
        mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, ctx->alpn_list);
    }

    if (mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg) != 0 ||
        mbedtls_ssl_set_hostname(&ctx->ssl, ip) != 0) {
        tls_ctx_free(ctx);
        close(fd);
        FAIL(E__TLS__INIT_FAILED);
    }

    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->fd, tls_net_send, tls_net_recv, NULL);

    int hs_ret;
    while ((hs_ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (hs_ret != MBEDTLS_ERR_SSL_WANT_READ &&
            hs_ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOG_WARNING("TLS client handshake failed: -0x%04x", (unsigned int)-hs_ret);
            tls_ctx_free(ctx);
            close(fd);
            FAIL(E__TLS__HANDSHAKE_FAILED);
        }
    }

    LOG_INFO("TLS %s connected to %s:%d", skin->name, ip, port);

    t = TRANSPORT__alloc_base(fd, skin);
    if (!t) {
        tls_ctx_free(ctx);
        close(fd);
        FAIL(E__TLS__ALLOC_FAILED);
    }
    t->is_incoming = 0;
    strncpy(t->client_ip, ip, INET_ADDRSTRLEN - 1);
    t->client_port = port;
    t->skin_ctx    = ctx;

    *out_transport = t;

l_cleanup:
    return rc;
}

/* ── send_msg ────────────────────────────────────────────────────────── */

err_t tls_common_send_msg(transport_t           *t,
                           const protocol_msg_t  *msg,
                           const uint8_t         *data) {
    err_t rc = E__SUCCESS;
    tls_common_ctx_t *ctx = (tls_common_ctx_t *)t->skin_ctx;
    uint8_t *buf = NULL;

    VALIDATE_ARGS(t, msg);

    size_t payload_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0) {
        payload_len += msg->data_length;
    }

    buf = malloc(payload_len);
    if (!buf) FAIL(E__TLS__ALLOC_FAILED);

    size_t bytes_written = 0;
    rc = PROTOCOL__serialize(msg, data, buf, payload_len, &bytes_written);
    FAIL_IF(E__SUCCESS != rc, rc);

    uint32_t hdr = htonl((uint32_t)bytes_written);
    rc = tls_write_all(&ctx->ssl, (const uint8_t *)&hdr, sizeof(hdr));
    FAIL_IF(E__SUCCESS != rc, rc);

    rc = tls_write_all(&ctx->ssl, buf, bytes_written);

l_cleanup:
    FREE(buf);
    return rc;
}

/* ── recv_msg ────────────────────────────────────────────────────────── */

err_t tls_common_recv_msg(transport_t      *t,
                           protocol_msg_t   *msg,
                           uint8_t         **out_data) {
    err_t rc = E__SUCCESS;
    tls_common_ctx_t *ctx = (tls_common_ctx_t *)t->skin_ctx;
    uint8_t *buf = NULL;
    uint8_t len_buf[4];
    uint32_t frame_len = 0;

    VALIDATE_ARGS(t, msg, out_data);
    *out_data = NULL;

    rc = tls_read_all(&ctx->ssl, len_buf, 4);
    FAIL_IF(E__SUCCESS != rc, rc);

    frame_len = ((uint32_t)len_buf[0] << 24) |
                ((uint32_t)len_buf[1] << 16) |
                ((uint32_t)len_buf[2] <<  8) |
                ((uint32_t)len_buf[3]);

    if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
        LOG_WARNING("TLS recv: bad frame length %u", frame_len);
        FAIL(E__TLS__BAD_FRAME_LEN);
    }

    buf = malloc(frame_len);
    if (!buf) FAIL(E__TLS__ALLOC_FAILED);

    rc = tls_read_all(&ctx->ssl, buf, frame_len);
    FAIL_IF(E__SUCCESS != rc, rc);

    size_t data_len = 0;
    rc = PROTOCOL__unserialize(buf, frame_len, msg, out_data, &data_len);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    FREE(buf);
    return rc;
}

/* ── transport_destroy ───────────────────────────────────────────────── */

void tls_common_transport_destroy(transport_t *t) {
    tls_common_ctx_t *ctx = (tls_common_ctx_t *)t->skin_ctx;
    if (!ctx) return;

    /* Best-effort close_notify */
    mbedtls_ssl_close_notify(&ctx->ssl);

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    tls_ctx_free(ctx);
    t->skin_ctx = NULL;
    t->fd       = -1;
}
