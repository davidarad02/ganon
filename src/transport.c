#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "err.h"
#include "logging.h"
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
    t->recv = TRANSPORT__recv;
    t->send = TRANSPORT__send;

    return t;
}

void TRANSPORT__destroy(transport_t *t) {
    FREE(t);
}

int TRANSPORT__get_fd(transport_t *t) {
    if (NULL == t) {
        return -1;
    }
    return t->fd;
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

    memcpy(msg, header, sizeof(*msg));

    if (0 != strncmp(msg->magic, GANON_PROTOCOL_MAGIC, 4)) {
        LOG_WARNING("Invalid magic on fd %d: expected %s, got %.4s", t->fd, GANON_PROTOCOL_MAGIC, msg->magic);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
    if (data_length > 0) {
        *data = malloc(data_length);
        if (NULL == *data) {
            LOG_ERROR("Failed to allocate data buffer of size %u", data_length);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }

        rc = TRANSPORT__recv_all(t, *data, data_length, &bytes_read);
        if (E__SUCCESS != rc) {
            goto l_cleanup;
        }
    }

l_cleanup:
    if (E__SUCCESS != rc && NULL != *data) {
        free(*data);
        *data = NULL;
    }
    return rc;
}

err_t TRANSPORT__send_msg(transport_t *t, const protocol_msg_t *msg, const uint8_t *data) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(t, msg);

    rc = TRANSPORT__send_all(t, (const uint8_t *)msg, PROTOCOL_HEADER_SIZE, NULL);
    FAIL_IF(E__SUCCESS != rc, rc);

    uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
    if (NULL != data && data_length > 0) {
        rc = TRANSPORT__send_all(t, data, data_length, NULL);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

l_cleanup:
    return rc;
}
