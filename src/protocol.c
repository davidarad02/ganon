#include <arpa/inet.h>
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

err_t PROTOCOL__parse_header(const uint8_t *buf, protocol_msg_t *msg) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(buf, msg);

    if (!PROTOCOL__validate_magic(buf)) {
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    memcpy(msg, buf, sizeof(*msg));

    PROTOCOL__msg_ntoh(msg);

l_cleanup:
    return rc;
}

err_t PROTOCOL__serialize(const protocol_msg_t *msg, uint8_t *buf, size_t buf_len, size_t *bytes_written) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(msg, buf, bytes_written);

    if (buf_len < PROTOCOL_HEADER_SIZE) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    protocol_msg_t net_msg = *msg;
    PROTOCOL__msg_hton(&net_msg);

    memcpy(buf, &net_msg, sizeof(net_msg));
    *bytes_written = PROTOCOL_HEADER_SIZE;

l_cleanup:
    return rc;
}

void PROTOCOL__msg_ntoh(protocol_msg_t *msg) {
    if (NULL == msg) {
        return;
    }
    msg->orig_src_node_id = ntohl(msg->orig_src_node_id);
    msg->src_node_id = ntohl(msg->src_node_id);
    msg->dst_node_id = ntohl(msg->dst_node_id);
    msg->message_id = ntohl(msg->message_id);
    msg->type = ntohl(msg->type);
    msg->data_length = ntohl(msg->data_length);
    msg->ttl = ntohl(msg->ttl);
}

void PROTOCOL__msg_hton(protocol_msg_t *msg) {
    if (NULL == msg) {
        return;
    }
    msg->orig_src_node_id = htonl(msg->orig_src_node_id);
    msg->src_node_id = htonl(msg->src_node_id);
    msg->dst_node_id = htonl(msg->dst_node_id);
    msg->message_id = htonl(msg->message_id);
    msg->type = htonl(msg->type);
    msg->data_length = htonl(msg->data_length);
    msg->ttl = htonl(msg->ttl);
}
