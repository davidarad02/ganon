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

    VALIDATE_ARGS(t, buf, bytes_sent);

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
