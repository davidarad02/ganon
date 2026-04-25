/*
 * tcp-plain skin:
 *   TCP transport with no encryption — length-prefixed raw protocol frames.
 *
 * Wire frame format:
 *   [4 bytes] big-endian payload length (= sizeof(protocol_msg_t) + data_len)
 *   [N bytes] plaintext serialized protocol_msg_t + data
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
#include "protocol.h"
#include "skin.h"
#include "skins/skin_tcp_plain.h"
#include "transport.h"

/* ---- Per-listener state ------------------------------------------------- */

typedef struct {
    int    listen_fd;
    addr_t addr;
} skin_tcp_plain_listener_t;

/* ---- Raw TCP recv/send (blocking) --------------------------------------- */

static err_t plain_recv_all(int fd, uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
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

static err_t plain_send_all(int fd, const uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("send failed on fd %d: %s", fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total += (size_t)n;
    }
l_cleanup:
    return rc;
}

/* ---- Transport allocation helper ---------------------------------------- */

static transport_t *plain_alloc_transport(int fd, int is_incoming,
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

/* ---- listener_create ---------------------------------------------------- */

static err_t plain_listener_create(const addr_t *addr,
                                    skin_listener_t **out_listener,
                                    int *out_listen_fd) {
    err_t rc = E__SUCCESS;
    skin_tcp_plain_listener_t *l = NULL;
    int fd = -1;

    VALIDATE_ARGS(addr, out_listener, out_listen_fd);

    *out_listener  = NULL;
    *out_listen_fd = -1;

    l = malloc(sizeof(skin_tcp_plain_listener_t));
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

    /* Non-blocking accept so the accept thread can poll the running flag. */
    int nb_flags = fcntl(fd, F_GETFL, 0);
    if (nb_flags >= 0) {
        fcntl(fd, F_SETFL, nb_flags | O_NONBLOCK);
    }

    LOG_INFO("Listening on %s:%d (tcp-plain)", addr->ip, addr->port);

    l->listen_fd   = fd;
    *out_listen_fd = fd;
    *out_listener  = (skin_listener_t *)l;

l_cleanup:
    return rc;
}

/* listener_accept */
static err_t plain_listener_accept(skin_listener_t *sl,
                                    transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    skin_tcp_plain_listener_t *l = (skin_tcp_plain_listener_t *)sl;
    transport_t *t = NULL;
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

    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);

    t = plain_alloc_transport(client_fd, 1, ip_str, client_port,
                               SKIN_TCP_PLAIN__ops());
    if (NULL == t) {
        close(client_fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("Accepted connection from %s:%d (fd=%d)", ip_str, client_port, client_fd);

    *out_transport = t;

l_cleanup:
    return rc;
}

/* listener_destroy */
static void plain_listener_destroy(skin_listener_t *sl) {
    skin_tcp_plain_listener_t *l = (skin_tcp_plain_listener_t *)sl;
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
static err_t plain_connect(const char *ip, int port, int connect_timeout_sec,
                            transport_t **out_transport) {
    err_t rc = E__SUCCESS;
    int fd = -1;
    transport_t *t = NULL;

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

    /* Restore blocking mode for normal operation */
    if (-1 == fcntl(fd, F_SETFL, flags)) {
        close(fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    t = plain_alloc_transport(fd, 0, ip, port, SKIN_TCP_PLAIN__ops());
    if (NULL == t) {
        close(fd);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("Connected to %s:%d (fd=%d)", ip, port, fd);
    *out_transport = t;

l_cleanup:
    return rc;
}

/* send_msg */
static err_t plain_send_msg(transport_t *t, const protocol_msg_t *msg,
                             const uint8_t *data) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(t, msg);

    LOG_TRACE("SEND msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, "
              "data_len=%u, ttl=%u, channel=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length,
              msg->ttl, msg->channel_id, t->fd);

    size_t plain_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0) {
        plain_len += msg->data_length;
    }

    uint8_t *buf = malloc(plain_len);
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
    rc = plain_send_all(t->fd, (uint8_t *)&net_len, 4);
    if (E__SUCCESS != rc) {
        FREE(buf);
        goto l_cleanup;
    }

    rc = plain_send_all(t->fd, buf, bytes_written);
    FREE(buf);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    return rc;
}

/* recv_msg */
static err_t plain_recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    uint8_t len_buf[4];
    uint32_t frame_len = 0;
    uint8_t *frame = NULL;

    VALIDATE_ARGS(t, msg, data);

    *data = NULL;

    rc = plain_recv_all(t->fd, len_buf, 4);
    FAIL_IF(E__SUCCESS != rc, rc);

    frame_len = ((uint32_t)len_buf[0] << 24) |
                ((uint32_t)len_buf[1] << 16) |
                ((uint32_t)len_buf[2] <<  8) |
                ((uint32_t)len_buf[3]);

    if (frame_len < PROTOCOL_HEADER_SIZE || frame_len > 300000) {
        LOG_WARNING("Invalid frame length: %u on fd=%d", frame_len, t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    frame = malloc(frame_len);
    if (NULL == frame) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    rc = plain_recv_all(t->fd, frame, frame_len);
    FAIL_IF(E__SUCCESS != rc, rc);

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
    if (NULL != frame) {
        FREE(frame);
    }
    return rc;
}

/* transport_destroy */
static void plain_transport_destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
}

/* ---- Vtable singleton --------------------------------------------------- */

static const skin_ops_t g_tcp_plain_skin = {
    .skin_id            = SKIN_ID__TCP_PLAIN,
    .name               = "tcp-plain",
    .listener_create    = plain_listener_create,
    .listener_accept    = plain_listener_accept,
    .listener_destroy   = plain_listener_destroy,
    .connect            = plain_connect,
    .send_msg           = plain_send_msg,
    .recv_msg           = plain_recv_msg,
    .transport_destroy  = plain_transport_destroy,
};

const skin_ops_t *SKIN_TCP_PLAIN__ops(void) {
    return &g_tcp_plain_skin;
}

err_t SKIN_TCP_PLAIN__register(void) {
    return SKIN__register(&g_tcp_plain_skin);
}
