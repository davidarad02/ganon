#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "transport.h"

ssize_t TRANSPORT__recv(int fd, uint8_t *buf, size_t len) {
    return recv(fd, buf, len, 0);
}

ssize_t TRANSPORT__send(int fd, const uint8_t *buf, size_t len) {
    return send(fd, buf, len, 0);
}

transport_t *TRANSPORT__create(int fd) {
    transport_t *t = malloc(sizeof(transport_t));
    if (NULL == t) {
        LOG_ERROR("Failed to allocate transport");
        return NULL;
    }

    t->fd = fd;
    t->is_incoming = 0;
    t->client_ip[0] = '\0';
    t->client_port = 0;
    t->node_id = 0;
    t->ctx = NULL;
    t->recv = TRANSPORT__recv;
    t->send = TRANSPORT__send;

    return t;
}

void TRANSPORT__destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
    FREE(t);
}

int TRANSPORT__get_fd(transport_t *t) {
    if (NULL == t) {
        return -1;
    }
    return t->fd;
}

uint32_t TRANSPORT__get_node_id(transport_t *t) {
    if (NULL == t) {
        return 0;
    }
    return t->node_id;
}

void TRANSPORT__set_node_id(transport_t *t, uint32_t node_id) {
    if (NULL == t) {
        return;
    }
    t->node_id = node_id;
}

err_t TRANSPORT__recv_all(transport_t *t, uint8_t *buf, size_t len, ssize_t *bytes_read) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(t, buf, bytes_read);

    *bytes_read = 0;

    size_t total_read = 0;
    while (total_read < len) {
        ssize_t n = t->recv(t->fd, buf + total_read, len - total_read);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("recv failed on fd %d: %s", t->fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        } else if (0 == n) {
            LOG_WARNING("Socket disconnected (fd=%d)", t->fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total_read += (size_t)n;
    }

    *bytes_read = (ssize_t)total_read;

l_cleanup:
    return rc;
}

err_t TRANSPORT__send_all(transport_t *t, const uint8_t *buf, size_t len, ssize_t *bytes_sent) {
    err_t rc = E__SUCCESS;

    if (NULL == t || NULL == buf || NULL == bytes_sent) {
        rc = E__INVALID_ARG_NULL_POINTER;
        goto l_cleanup;
    }

    *bytes_sent = 0;

    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t n = t->send(t->fd, buf + total_sent, len - total_sent);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("send failed on fd %d: %s", t->fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total_sent += (size_t)n;
    }

    *bytes_sent = (ssize_t)total_sent;

l_cleanup:
    return rc;
}

err_t TRANSPORT__recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(t, msg, data);

    *data = NULL;

    uint8_t header[PROTOCOL_HEADER_SIZE];
    ssize_t bytes_read = 0;
    rc = TRANSPORT__recv_all(t, header, PROTOCOL_HEADER_SIZE, &bytes_read);
    if (E__SUCCESS != rc) {
        goto l_cleanup;
    }

    if ((size_t)bytes_read < PROTOCOL_HEADER_SIZE) {
        LOG_WARNING("Incomplete protocol header on fd %d: got %zd, expected %zu", t->fd, bytes_read, PROTOCOL_HEADER_SIZE);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    size_t data_len = 0;
    rc = PROTOCOL__unserialize(header, PROTOCOL_HEADER_SIZE, msg, data, &data_len);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to unserialize message from fd %d", t->fd);
        goto l_cleanup;
    }

    LOG_TRACE("RECV msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, data_len=%u, ttl=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length, msg->ttl, t->fd);

l_cleanup:
    return rc;
}

err_t TRANSPORT__send_msg(transport_t *t, const protocol_msg_t *msg, const uint8_t *data) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(t, msg);

    LOG_TRACE("SEND msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, data_len=%u, ttl=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length, msg->ttl, t->fd);

    uint8_t buf[PROTOCOL_HEADER_SIZE];
    size_t bytes_written = 0;
    rc = PROTOCOL__serialize(msg, data, buf, sizeof(buf), &bytes_written);
    if (E__SUCCESS != rc) {
        goto l_cleanup;
    }

    rc = TRANSPORT__send_all(t, buf, bytes_written, NULL);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    return rc;
}

err_t TRANSPORT__send_to_node_id(network_t *net, uint32_t node_id, const protocol_msg_t *msg, const uint8_t *data) {
    if (NULL == net || NULL == msg) {
        return E__INVALID_ARG_NULL_POINTER;
    }

    transport_t *t = NETWORK__get_transport(net, node_id);
    if (NULL == t) {
        LOG_WARNING("TRANSPORT__send_to_node_id: no transport for node_id=%u", node_id);
        return E__NET__SOCKET_CONNECT_FAILED;
    }

    return TRANSPORT__send_msg(t, msg, data);
}
