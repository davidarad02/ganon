#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "logging.h"
#include "protocol.h"
#include "session.h"
#include "transport.h"

#define PROTOCOL_HEADER_SIZE 20

static err_t SESSION__handle_ping(uint32_t node_id, uint32_t message_id, uint32_t data_length) {
    err_t rc = E__SUCCESS;

    (void)node_id;
    (void)message_id;
    (void)data_length;

    LOG_DEBUG("Received PING from node %u (msg_id=%u, data_len=%u)", node_id, message_id, data_length);

    return rc;
}

static err_t SESSION__handle_message(uint32_t node_id, uint32_t message_id, msg_type_t type, uint32_t data_length) {
    err_t rc = E__SUCCESS;

    LOG_INFO("Protocol: node_id=%u, msg_id=%u, type=%d, data_len=%u", node_id, message_id, type, data_length);

    switch (type) {
    case MSG__PING:
        FAIL_IF(E__SUCCESS != SESSION__handle_ping(node_id, message_id, data_length), E__SESSION__HANDLE_PING_FAILED);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

l_cleanup:
    return rc;
}

err_t SESSION__process(transport_t *t) {
    err_t rc = E__SUCCESS;
    uint8_t *data = NULL;

    VALIDATE_ARGS(t);

    uint8_t header_buffer[PROTOCOL_HEADER_SIZE];
    ssize_t bytes_read = t->recv(t->fd, header_buffer, sizeof(header_buffer));
    if (0 > bytes_read) {
        LOG_WARNING("Failed to read header from fd %d", t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    } else if (0 == bytes_read) {
        LOG_WARNING("Socket disconnected (fd=%d)", t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    if ((size_t)bytes_read < sizeof(header_buffer)) {
        LOG_WARNING("Incomplete protocol header on fd %d: got %zd, expected %zu", t->fd, bytes_read, sizeof(header_buffer));
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    protocol_msg_t *msg = (protocol_msg_t *)header_buffer;
    if (0 != strncmp(msg->magic, GANON_PROTOCOL_MAGIC, 4)) {
        LOG_WARNING("Invalid magic on fd %d: expected %s, got %.4s", t->fd, GANON_PROTOCOL_MAGIC, msg->magic);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
    if (data_length > 0) {
        data = malloc(data_length);
        FAIL_IF(NULL == data, E__INVALID_ARG_NULL_POINTER);

        ssize_t recv_bytes;
        rc = TRANSPORT__recv_all(t, data, data_length, &recv_bytes);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to read data from fd %d", t->fd);
            FAIL(rc);
        }
    }

    uint32_t node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->node_id);
    uint32_t message_id = PROTOCOL_FIELD_FROM_NETWORK(msg->message_id);
    msg_type_t type = msg->type;

    FAIL_IF(E__SUCCESS != SESSION__handle_message(node_id, message_id, type, data_length), E__SESSION__HANDLE_MESSAGE_FAILED);

l_cleanup:
    FREE(data);
    return rc;
}
