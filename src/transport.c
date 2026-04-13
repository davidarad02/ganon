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
    if (NULL != t) {
        free(t);
    }
}

ssize_t TRANSPORT__recv_all(transport_t *t, uint8_t *buf, size_t len) {
    if (NULL == t || NULL == buf) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < len) {
        ssize_t bytes_read = t->recv(t->fd, buf + total_read, len - total_read);
        if (0 > bytes_read) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("recv failed on fd %d: %s", t->fd, strerror(errno));
            return -1;
        } else if (0 == bytes_read) {
            LOG_WARNING("Socket disconnected (fd=%d)", t->fd);
            return -1;
        }
        total_read += (size_t)bytes_read;
    }

    return (ssize_t)total_read;
}

ssize_t TRANSPORT__send_all(transport_t *t, const uint8_t *buf, size_t len) {
    if (NULL == t || NULL == buf) {
        return -1;
    }

    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t bytes_sent = t->send(t->fd, buf + total_sent, len - total_sent);
        if (0 > bytes_sent) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("send failed on fd %d: %s", t->fd, strerror(errno));
            return -1;
        }
        total_sent += (size_t)bytes_sent;
    }

    return (ssize_t)total_sent;
}
