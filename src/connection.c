#include <stdlib.h>
#include <unistd.h>

#include "common.h"
#include "connection.h"
#include "logging.h"

void CONNECTION__init(connection_t *conn, int fd) {
    if (NULL == conn) {
        return;
    }
    conn->node_id = 0;
    conn->fd = fd;
    conn->ctx = NULL;
}

void CONNECTION__set_node_id(connection_t *conn, uint32_t node_id) {
    if (NULL == conn) {
        return;
    }
    conn->node_id = node_id;
}

uint32_t CONNECTION__get_node_id(connection_t *conn) {
    if (NULL == conn) {
        return 0;
    }
    return conn->node_id;
}

int CONNECTION__get_fd(connection_t *conn) {
    if (NULL == conn) {
        return -1;
    }
    return conn->fd;
}

void CONNECTION__close(connection_t *conn) {
    if (NULL == conn) {
        return;
    }
    if (conn->fd >= 0) {
        shutdown(conn->fd, SHUT_RDWR);
        close(conn->fd);
        conn->fd = -1;
    }
}
