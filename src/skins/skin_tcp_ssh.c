/*
 * tcp-ssh skin:
 *   SSH transport over TCP using libssh.  The ganon protocol is carried
 *   inside an SSH "session" channel with subsystem name "ganon".
 *
 * Authentication:  "none" method — no passwords or keys required.
 * Host key:        Server generates an ephemeral Ed25519 key per listener
 *                  instance.  Client skips host key verification.
 * Channel policy:  Server accepts ONLY "session" channel opens followed by
 *                  a "ganon" subsystem request.  All other channel types and
 *                  channel requests (exec, shell, pty, …) are rejected.
 *
 * Wire frame format (identical to tcp-plain, carried over SSH channel):
 *   [4 bytes] big-endian payload length
 *   [N bytes] serialized protocol_msg_t + data
 */

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
#include <sys/types.h>
#include <unistd.h>

#include <libssh/libssh.h>
#include <libssh/server.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "protocol.h"
#include "skin.h"
#include "skins/skin_tcp_ssh.h"
#include "transport.h"

/* ---- Per-listener state ------------------------------------------------- */

typedef struct {
    ssh_bind bind;
    ssh_key  host_key;
    addr_t   addr;
} skin_tcp_ssh_listener_t;

/* ---- Per-connection context --------------------------------------------- */

typedef struct {
    ssh_session session;
    ssh_channel channel;
} skin_tcp_ssh_ctx_t;

/* ---- Blocking channel read/write helpers -------------------------------- */

static err_t skin_ssh_recv_all(ssh_channel ch, uint8_t *buf, uint32_t len) {
    err_t rc = E__SUCCESS;
    uint32_t total = 0;

    while (total < len) {
        int n = ssh_channel_read(ch, buf + total, len - total, 0);
        if (0 > n) {
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        if (0 == n) {
            /* EOF */
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total += (uint32_t)n;
    }

l_cleanup:
    return rc;
}

static err_t skin_ssh_send_all(ssh_channel ch, const uint8_t *buf, uint32_t len) {
    err_t rc = E__SUCCESS;
    uint32_t total = 0;

    while (total < len) {
        int n = ssh_channel_write(ch, buf + total, len - total);
        if (0 > n) {
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total += (uint32_t)n;
    }

l_cleanup:
    return rc;
}

/* ---- Server-side handshake (auth + channel negotiation) ----------------- */

static err_t skin_ssh_server_handshake(ssh_session session, ssh_channel *out_channel) {
    err_t rc = E__SUCCESS;
    ssh_message msg = NULL;
    ssh_channel channel = NULL;
    int authenticated = 0;
    int got_channel   = 0;
    int ready         = 0;

    if (SSH_OK != ssh_handle_key_exchange(session)) {
        LOG_WARNING("SSH key exchange failed: %s", ssh_get_error(session));
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    while (!ready) {
        msg = ssh_message_get(session);
        if (NULL == msg) {
            LOG_WARNING("SSH message_get returned NULL during handshake");
            FAIL(E__CRYPTO__HANDSHAKE_FAILED);
        }

        switch (ssh_message_type(msg)) {

        case SSH_REQUEST_SERVICE:
            ssh_message_reply_default(msg);
            break;

        case SSH_REQUEST_AUTH:
            if (SSH_AUTH_METHOD_NONE == ssh_message_subtype(msg)) {
                ssh_message_auth_reply_success(msg, 0);
                authenticated = 1;
            } else {
                /* Advertise only "none" so the client stops trying others. */
                ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_NONE);
                ssh_message_reply_default(msg);
            }
            break;

        case SSH_REQUEST_CHANNEL_OPEN:
            if (authenticated && SSH_CHANNEL_SESSION == ssh_message_subtype(msg)) {
                channel = ssh_message_channel_request_open_reply_accept(msg);
                if (NULL == channel) {
                    LOG_WARNING("Failed to accept channel open");
                    FAIL(E__CRYPTO__HANDSHAKE_FAILED);
                }
                got_channel = 1;
            } else {
                ssh_message_reply_default(msg);
            }
            break;

        case SSH_REQUEST_CHANNEL:
            if (got_channel && SSH_CHANNEL_REQUEST_SUBSYSTEM == ssh_message_subtype(msg)) {
                const char *subsys = ssh_message_channel_request_subsystem(msg);
                if (NULL != subsys && 0 == strcmp(subsys, "ganon")) {
                    ssh_message_channel_request_reply_success(msg);
                    ready = 1;
                } else {
                    LOG_WARNING("SSH: rejected subsystem '%s' (not 'ganon')",
                                NULL != subsys ? subsys : "(null)");
                    ssh_message_reply_default(msg);
                }
            } else {
                /* Deny exec, shell, pty, env, and all other channel requests. */
                ssh_message_reply_default(msg);
            }
            break;

        default:
            ssh_message_reply_default(msg);
            break;
        }

        ssh_message_free(msg);
        msg = NULL;
    }

    *out_channel = channel;
    channel = NULL;

l_cleanup:
    if (NULL != msg) {
        ssh_message_free(msg);
    }
    if (E__SUCCESS != rc && NULL != channel) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    return rc;
}

/* ---- Transport allocation helper ---------------------------------------- */

static transport_t *skin_ssh_alloc_transport(ssh_session session, ssh_channel channel,
                                         int is_incoming,
                                         const char *ip, int port) {
    skin_tcp_ssh_ctx_t *ctx = NULL;
    transport_t *t = NULL;
    int fd = (int)ssh_get_fd(session);

    t = TRANSPORT__alloc_base(fd, SKIN_TCP_SSH__ops());
    if (NULL == t) {
        return NULL;
    }

    ctx = malloc(sizeof(skin_tcp_ssh_ctx_t));
    if (NULL == ctx) {
        TRANSPORT__free_base(t);
        return NULL;
    }
    ctx->session = session;
    ctx->channel = channel;

    t->skin_ctx    = ctx;
    t->is_incoming = is_incoming;
    if (NULL != ip) {
        strncpy(t->client_ip, ip, INET_ADDRSTRLEN - 1);
        t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    }
    t->client_port = port;

    return t;
}

/* ---- listener_create ---------------------------------------------------- */

static err_t skin_ssh_listener_create(const addr_t *addr,
                                  skin_listener_t **out_listener,
                                  int *out_listen_fd) {
    err_t rc = E__SUCCESS;
    skin_tcp_ssh_listener_t *l = NULL;
    ssh_key host_key = NULL;

    VALIDATE_ARGS(addr, out_listener, out_listen_fd);

    *out_listener  = NULL;
    *out_listen_fd = -1;

    l = malloc(sizeof(skin_tcp_ssh_listener_t));
    if (NULL == l) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    memset(l, 0, sizeof(*l));

    /* Generate an ephemeral Ed25519 host key. */
    if (SSH_OK != ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &host_key)) {
        LOG_ERROR("SSH: failed to generate ephemeral host key");
        FREE(l);
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    l->bind = ssh_bind_new();
    if (NULL == l->bind) {
        LOG_ERROR("SSH: ssh_bind_new() failed");
        ssh_key_free(host_key);
        FREE(l);
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    ssh_bind_options_set(l->bind, SSH_BIND_OPTIONS_BINDADDR, addr->ip);
    int port = addr->port;
    ssh_bind_options_set(l->bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(l->bind, SSH_BIND_OPTIONS_IMPORT_KEY, host_key);

    /* Silent logging */
    int log_level = SSH_LOG_NOLOG;
    ssh_bind_options_set(l->bind, SSH_BIND_OPTIONS_LOG_VERBOSITY, &log_level);

    if (SSH_OK != ssh_bind_listen(l->bind)) {
        LOG_ERROR("SSH: ssh_bind_listen on %s:%d failed: %s",
                  addr->ip, addr->port, ssh_get_error(l->bind));
        ssh_key_free(host_key);
        ssh_bind_free(l->bind);
        FREE(l);
        FAIL(E__NET__SOCKET_LISTEN_FAILED);
    }

    l->host_key    = host_key;
    l->addr        = *addr;
    *out_listen_fd = (int)ssh_bind_get_fd(l->bind);
    *out_listener  = (skin_listener_t *)l;

    LOG_INFO("Listening on %s:%d (tcp-ssh)", addr->ip, addr->port);

l_cleanup:
    return rc;
}

/* ---- listener_accept ---------------------------------------------------- */

static err_t skin_ssh_listener_accept(skin_listener_t *sl, transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_tcp_ssh_listener_t *l = (skin_tcp_ssh_listener_t *)sl;
    ssh_session session = NULL;
    ssh_channel channel = NULL;

    VALIDATE_ARGS(l, out_transport);

    *out_transport = NULL;

    /* Non-blocking check: is a TCP connection waiting on the bind fd? */
    int bind_fd = (int)ssh_bind_get_fd(l->bind);
    struct timeval tv;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(bind_fd, &rfds);
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    if (1 != select(bind_fd + 1, &rfds, NULL, NULL, &tv)) {
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    session = ssh_new();
    if (NULL == session) {
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    if (SSH_OK != ssh_bind_accept(l->bind, session)) {
        LOG_WARNING("SSH: ssh_bind_accept failed: %s", ssh_get_error(l->bind));
        ssh_free(session);
        FAIL(E__NET__SOCKET_ACCEPT_FAILED);
    }

    /* Silence the session logger. */
    int log_level = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &log_level);

    /* Blocking: complete key exchange, auth, and channel setup. */
    rc = skin_ssh_server_handshake(session, &channel);
    if (E__SUCCESS != rc) {
        ssh_disconnect(session);
        ssh_free(session);
        goto l_cleanup;
    }

    /* Retrieve peer address for the transport struct. */
    char ip_str[INET_ADDRSTRLEN] = {0};
    int peer_port = 0;
    {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = (int)ssh_get_fd(session);
        if (0 == getpeername(fd, (struct sockaddr *)&peer, &peer_len)) {
            inet_ntop(AF_INET, &peer.sin_addr, ip_str, INET_ADDRSTRLEN);
            peer_port = ntohs(peer.sin_port);
        }
    }

    transport_t *t = skin_ssh_alloc_transport(session, channel, 1, ip_str, peer_port);
    if (NULL == t) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("SSH: accepted ganon connection from %s:%d", ip_str, peer_port);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* ---- listener_destroy --------------------------------------------------- */

static void skin_ssh_listener_destroy(skin_listener_t *sl) {
    skin_tcp_ssh_listener_t *l = (skin_tcp_ssh_listener_t *)sl;
    if (NULL == l) {
        return;
    }
    if (NULL != l->bind) {
        ssh_bind_free(l->bind);
        l->bind = NULL;
    }
    if (NULL != l->host_key) {
        ssh_key_free(l->host_key);
        l->host_key = NULL;
    }
    free(l);
}

/* ---- connect ------------------------------------------------------------ */

static err_t skin_ssh_connect(const char *ip, int port, int connect_timeout_sec,
                          transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    ssh_session session = NULL;
    ssh_channel channel = NULL;

    VALIDATE_ARGS(ip, out_transport);

    *out_transport = NULL;

    session = ssh_new();
    if (NULL == session) {
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }

    /* Connection options */
    ssh_options_set(session, SSH_OPTIONS_HOST, ip);
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);

    /* Do not read ~/.ssh/config or system config */
    int no = 0;
    ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &no);

    /* Disable host key verification */
    int strict = 0;
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);

    /* Silent logging */
    int log_level = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &log_level);

    /* Connection timeout */
    long timeout_us = (long)connect_timeout_sec * 1000000L;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT_USEC, &timeout_us);

    LOG_INFO("SSH: connecting to %s:%d...", ip, port);

    if (SSH_OK != ssh_connect(session)) {
        LOG_WARNING("SSH: connect to %s:%d failed: %s", ip, port,
                    ssh_get_error(session));
        ssh_free(session);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    /* "none" authentication */
    int auth = ssh_userauth_none(session, "ganon");
    if (SSH_AUTH_SUCCESS != auth) {
        LOG_WARNING("SSH: 'none' auth to %s:%d failed (code=%d)", ip, port, auth);
        ssh_disconnect(session);
        ssh_free(session);
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    /* Open a session channel */
    channel = ssh_channel_new(session);
    if (NULL == channel) {
        ssh_disconnect(session);
        ssh_free(session);
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    if (SSH_OK != ssh_channel_open_session(channel)) {
        LOG_WARNING("SSH: channel open to %s:%d failed: %s", ip, port,
                    ssh_get_error(session));
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    /* Request the "ganon" subsystem */
    if (SSH_OK != ssh_channel_request_subsystem(channel, "ganon")) {
        LOG_WARNING("SSH: subsystem 'ganon' request to %s:%d failed: %s",
                    ip, port, ssh_get_error(session));
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    transport_t *t = skin_ssh_alloc_transport(session, channel, 0, ip, port);
    if (NULL == t) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("SSH: connected to %s:%d", ip, port);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* ---- send_msg ----------------------------------------------------------- */

static err_t skin_ssh_send_msg(transport_t *t, const protocol_msg_t *msg,
                           const uint8_t *data) {
    err_t rc = E__SUCCESS;
    uint8_t *buf = NULL;
    skin_tcp_ssh_ctx_t *ctx = (skin_tcp_ssh_ctx_t *)t->skin_ctx;

    VALIDATE_ARGS(t, msg, ctx);

    LOG_TRACE("SSH SEND msg: orig_src=%u, src=%u, dst=%u, type=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->type, t->fd);

    size_t plain_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0) {
        plain_len += msg->data_length;
    }

    buf = malloc(plain_len);
    if (NULL == buf) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    size_t bytes_written = 0;
    rc = PROTOCOL__serialize(msg, data, buf, plain_len, &bytes_written);
    if (E__SUCCESS != rc) {
        FREE(buf);
        goto l_cleanup;
    }

    uint32_t net_len = htonl((uint32_t)bytes_written);
    rc = skin_ssh_send_all(ctx->channel, (const uint8_t *)&net_len, 4);
    if (E__SUCCESS != rc) {
        FREE(buf);
        goto l_cleanup;
    }

    rc = skin_ssh_send_all(ctx->channel, buf, (uint32_t)bytes_written);
    FREE(buf);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    if (NULL != buf) {
        FREE(buf);
    }
    return rc;
}

/* ---- recv_msg ----------------------------------------------------------- */

static err_t skin_ssh_recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    uint8_t len_buf[4];
    uint32_t frame_len = 0;
    uint8_t *frame = NULL;
    skin_tcp_ssh_ctx_t *ctx = (skin_tcp_ssh_ctx_t *)t->skin_ctx;

    VALIDATE_ARGS(t, msg, data, ctx);

    *data = NULL;

    rc = skin_ssh_recv_all(ctx->channel, len_buf, 4);
    FAIL_IF(E__SUCCESS != rc, rc);

    frame_len = ((uint32_t)len_buf[0] << 24) |
                ((uint32_t)len_buf[1] << 16) |
                ((uint32_t)len_buf[2] <<  8) |
                ((uint32_t)len_buf[3]);

    if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
        LOG_WARNING("SSH: invalid frame length %u", frame_len);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    frame = malloc(frame_len);
    if (NULL == frame) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    rc = skin_ssh_recv_all(ctx->channel, frame, frame_len);
    if (E__SUCCESS != rc) {
        FREE(frame);
        goto l_cleanup;
    }

    size_t unserialize_data_len = 0;
    rc = PROTOCOL__unserialize(frame, frame_len, msg, data, &unserialize_data_len);
    if (E__SUCCESS != rc) {
        LOG_WARNING("SSH: failed to unserialize message");
        FREE(frame);
        goto l_cleanup;
    }

    LOG_TRACE("SSH RECV msg: orig_src=%u, src=%u, dst=%u, type=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->type, t->fd);

    FREE(frame);

l_cleanup:
    if (NULL != frame) {
        FREE(frame);
    }
    return rc;
}

/* ---- transport_destroy -------------------------------------------------- */

static void skin_ssh_transport_destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    skin_tcp_ssh_ctx_t *ctx = (skin_tcp_ssh_ctx_t *)t->skin_ctx;
    if (NULL == ctx) {
        return;
    }

    if (NULL != ctx->channel) {
        ssh_channel_send_eof(ctx->channel);
        ssh_channel_close(ctx->channel);
        ssh_channel_free(ctx->channel);
        ctx->channel = NULL;
    }

    if (NULL != ctx->session) {
        /* ssh_disconnect tries to send a disconnect packet; it may fail
         * if the network layer already closed the underlying fd, which is
         * fine — we still call ssh_free to release all resources. */
        ssh_disconnect(ctx->session);
        ssh_free(ctx->session);
        ctx->session = NULL;
    }

    free(ctx);
    t->skin_ctx = NULL;

    /* Do NOT close t->fd here: the network layer owns that lifecycle and
     * may have already closed it before calling transport_destroy. */
    t->fd = -1;
}

/* ---- Vtable singleton --------------------------------------------------- */

static const skin_ops_t g_tcp_ssh_skin = {
    .skin_id            = SKIN_ID__SSH,
    .name               = "tcp-ssh",
    .listener_create    = skin_ssh_listener_create,
    .listener_accept    = skin_ssh_listener_accept,
    .listener_destroy   = skin_ssh_listener_destroy,
    .connect            = skin_ssh_connect,
    .send_msg           = skin_ssh_send_msg,
    .recv_msg           = skin_ssh_recv_msg,
    .transport_destroy  = skin_ssh_transport_destroy,
};

const skin_ops_t *SKIN_TCP_SSH__ops(void) {
    return &g_tcp_ssh_skin;
}

err_t SKIN_TCP_SSH__register(void) {
    return SKIN__register(&g_tcp_ssh_skin);
}
