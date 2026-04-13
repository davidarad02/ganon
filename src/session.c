#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include "common.h"
#include "logging.h"
#include "protocol.h"

#define PROTOCOL_HEADER_SIZE 20

static err_t SESSION__handle_ping(int fd, uint32_t node_id, uint32_t message_id, uint32_t data_length) {
    err_t rc = E__SUCCESS;

    LOG_DEBUG("Received PING from node %u (msg_id=%u, data_len=%u)", node_id, message_id, data_length);

    return rc;
}

static err_t SESSION__handle_message(int fd, protocol_msg_t *msg, uint8_t *data) {
    err_t rc = E__SUCCESS;

    FAIL_IF(NULL == msg, E__INVALID_ARG_NULL_POINTER);

    uint32_t node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->node_id);
    uint32_t message_id = PROTOCOL_FIELD_FROM_NETWORK(msg->message_id);
    msg_type_t type = msg->type;
    uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);

    LOG_INFO("Protocol: node_id=%u, msg_id=%u, type=%d, data_len=%u", node_id, message_id, type, data_length);

    switch (type) {
    case MSG__PING:
        rc = SESSION__handle_ping(fd, node_id, message_id, data_length);
        FAIL_IF(E__SUCCESS != rc, rc);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

l_cleanup:
    return rc;
}

err_t SESSION__protocol_loop(int fd) {
    err_t rc = E__SUCCESS;
    uint8_t header_buffer[PROTOCOL_HEADER_SIZE];
    ssize_t bytes_read;

    while (true) {
        bytes_read = recv(fd, header_buffer, sizeof(header_buffer), 0);
        if (0 > bytes_read) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("recv failed on fd %d: %s", fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        } else if (0 == bytes_read) {
            LOG_WARNING("Socket disconnected (fd=%d)", fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }

        if ((size_t)bytes_read < sizeof(header_buffer)) {
            LOG_WARNING("Incomplete protocol header on fd %d: got %zd, expected %zu", fd, bytes_read, sizeof(header_buffer));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }

        protocol_msg_t *msg = (protocol_msg_t *)header_buffer;
        if (0 != strncmp(msg->magic, GANON_PROTOCOL_MAGIC, 4)) {
            LOG_WARNING("Invalid magic on fd %d: expected %s, got %.4s", fd, GANON_PROTOCOL_MAGIC, msg->magic);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }

        uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
        uint8_t *data = NULL;
        if (data_length > 0) {
            data = malloc(data_length);
            if (NULL == data) {
                LOG_ERROR("Failed to allocate %u bytes for message data", data_length);
                FAIL(E__INVALID_ARG_NULL_POINTER);
            }

            size_t total_read = 0;
            while (total_read < data_length) {
                bytes_read = recv(fd, data + total_read, data_length - total_read, 0);
                if (0 > bytes_read) {
                    if (EAGAIN == errno || EWOULDBLOCK == errno) {
                        continue;
                    }
                    LOG_WARNING("recv failed reading data on fd %d: %s", fd, strerror(errno));
                    free(data);
                    FAIL(E__NET__SOCKET_CONNECT_FAILED);
                } else if (0 == bytes_read) {
                    LOG_WARNING("Socket disconnected while reading data (fd=%d)", fd);
                    free(data);
                    FAIL(E__NET__SOCKET_CONNECT_FAILED);
                }
                total_read += (size_t)bytes_read;
            }
        }

        rc = SESSION__handle_message(fd, msg, data);
        free(data);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

l_cleanup:
    return rc;
}
