#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "logging.h"
#include "protocol.h"

bool PROTOCOL__validate_magic(const uint8_t *buf) {
    if (NULL == buf) {
        return false;
    }
    return 0 == strncmp((const char *)buf, GANON_PROTOCOL_MAGIC, 4);
}

err_t PROTOCOL__unserialize(const uint8_t *buf, size_t len, protocol_msg_t *msg, uint8_t **data, size_t *data_len) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(buf, msg, data, data_len);

    if (len < PROTOCOL_HEADER_SIZE) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    if (!PROTOCOL__validate_magic(buf)) {
        LOG_WARNING("Invalid magic: expected %.4s, got %.4s", GANON_PROTOCOL_MAGIC, buf);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    memcpy(msg, buf, sizeof(*msg));

    msg->orig_src_node_id = ntohl(msg->orig_src_node_id);
    msg->src_node_id = ntohl(msg->src_node_id);
    msg->dst_node_id = ntohl(msg->dst_node_id);
    msg->message_id = ntohl(msg->message_id);
    msg->type = ntohl(msg->type);
    msg->data_length = ntohl(msg->data_length);
    msg->ttl = ntohl(msg->ttl);

    *data = NULL;
    *data_len = 0;

    if (msg->data_length > 0) {
        size_t total_len = PROTOCOL_HEADER_SIZE + msg->data_length;
        if (len < total_len) {
            LOG_WARNING("Incomplete message: expected %zu bytes, got %zu", total_len, len);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        *data = malloc(msg->data_length);
        if (NULL == *data) {
            LOG_ERROR("Failed to allocate data buffer of size %u", msg->data_length);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }
        memcpy(*data, buf + PROTOCOL_HEADER_SIZE, msg->data_length);
        *data_len = msg->data_length;
    }

l_cleanup:
    return rc;
}

err_t PROTOCOL__serialize(const protocol_msg_t *msg, const uint8_t *data, uint8_t *buf, size_t buf_len, size_t *bytes_written) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(msg, buf, bytes_written);

    if (buf_len < PROTOCOL_HEADER_SIZE) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    protocol_msg_t net_msg = *msg;
    net_msg.orig_src_node_id = htonl(net_msg.orig_src_node_id);
    net_msg.src_node_id = htonl(net_msg.src_node_id);
    net_msg.dst_node_id = htonl(net_msg.dst_node_id);
    net_msg.message_id = htonl(net_msg.message_id);
    net_msg.type = htonl(net_msg.type);
    net_msg.data_length = htonl(net_msg.data_length);
    net_msg.ttl = htonl(net_msg.ttl);

    memcpy(buf, &net_msg, sizeof(net_msg));
    *bytes_written = PROTOCOL_HEADER_SIZE;

    if (NULL != data && msg->data_length > 0) {
        if (buf_len < PROTOCOL_HEADER_SIZE + msg->data_length) {
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }
        memcpy(buf + PROTOCOL_HEADER_SIZE, data, msg->data_length);
        *bytes_written += msg->data_length;
    }

l_cleanup:
    return rc;
}
