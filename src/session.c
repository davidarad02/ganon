#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "logging.h"
#include "protocol.h"
#include "routing.h"
#include "session.h"
#include "transport.h"

static err_t SESSION__handle_node_init(routing_table_t *rt, int fd, uint32_t orig_src_node_id, uint32_t src_node_id, uint32_t message_id, uint32_t ttl, uint32_t data_length, uint8_t *data, uint32_t *out_node_id) {
    err_t rc = E__SUCCESS;

    (void)orig_src_node_id;
    (void)message_id;
    (void)ttl;
    (void)data_length;
    (void)data;

    LOG_DEBUG("Received NODE_INIT from node %u (orig_src=%u, msg_id=%u, ttl=%u, data_len=%u)", src_node_id, orig_src_node_id, message_id, ttl, data_length);

    rc = ROUTING__add_direct(rt, src_node_id, fd);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to add node %u to routing table", src_node_id);
        goto l_cleanup;
    }

    if (NULL != out_node_id) {
        *out_node_id = src_node_id;
    }

l_cleanup:
    return rc;
}

static err_t SESSION__handle_peer_info(routing_table_t *rt, uint32_t orig_src_node_id, uint32_t src_node_id, uint32_t message_id, uint32_t ttl, uint32_t data_length, uint8_t *data, uint32_t *out_node_id, uint32_t **out_peer_list, size_t *out_peer_count) {
    err_t rc = E__SUCCESS;

    (void)orig_src_node_id;
    (void)message_id;
    (void)ttl;

    LOG_DEBUG("Received PEER_INFO from node %u (orig_src=%u, msg_id=%u, ttl=%u, data_len=%u)", src_node_id, orig_src_node_id, message_id, ttl, data_length);

    uint32_t *peer_list = (uint32_t *)data;
    size_t peer_count = data_length / sizeof(uint32_t);

    uint32_t *learned_peers = NULL;
    size_t learned_count = 0;

    if (peer_count > 0) {
        learned_peers = malloc(peer_count * sizeof(uint32_t));
        if (NULL == learned_peers) {
            LOG_WARNING("Failed to allocate learned peers list");
        }
    }

    LOG_DEBUG("Peer list from node %u contains %zu peers:", src_node_id, peer_count);
    for (size_t i = 0; i < peer_count; i++) {
        uint32_t peer_id = PROTOCOL_FIELD_FROM_NETWORK(peer_list[i]);
        LOG_DEBUG("  - peer %u: node %u", i, peer_id);
        rc = ROUTING__add_via_hop(rt, peer_id, src_node_id);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to add route to node %u via node %u", peer_id, src_node_id);
        } else if (NULL != learned_peers) {
            learned_peers[learned_count++] = peer_id;
        }
    }

    (void)out_node_id;

    if (NULL != out_peer_list) {
        *out_peer_list = learned_peers;
    } else if (NULL != learned_peers) {
        free(learned_peers);
    }
    if (NULL != out_peer_count) {
        *out_peer_count = learned_count;
    }

    return rc;
}

static err_t SESSION__handle_message(routing_table_t *rt, int fd, uint32_t orig_src_node_id, uint32_t src_node_id, uint32_t dst_node_id, uint32_t message_id, msg_type_t type, uint32_t ttl, uint32_t data_length, uint8_t *data, uint32_t *out_node_id, uint32_t **out_peer_list, size_t *out_peer_count) {
    err_t rc = E__SUCCESS;

    (void)orig_src_node_id;
    (void)dst_node_id;

    LOG_INFO("Protocol: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%d, ttl=%u, data_len=%u", orig_src_node_id, src_node_id, dst_node_id, message_id, type, ttl, data_length);

    switch (type) {
    case MSG__NODE_INIT:
        rc = SESSION__handle_node_init(rt, fd, orig_src_node_id, src_node_id, message_id, ttl, data_length, data, out_node_id);
        FAIL_IF(E__SUCCESS != rc, E__SESSION__HANDLE_NODE_INIT_FAILED);
        break;
    case MSG__PEER_INFO:
        rc = SESSION__handle_peer_info(rt, orig_src_node_id, src_node_id, message_id, ttl, data_length, data, out_node_id, out_peer_list, out_peer_count);
        FAIL_IF(E__SUCCESS != rc, E__SESSION__HANDLE_MESSAGE_FAILED);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

l_cleanup:
    return rc;
}

err_t SESSION__process(routing_table_t *rt, int fd, transport_t *t, uint32_t *peer_node_id, uint8_t *out_header, size_t header_len, uint32_t **out_peer_list, size_t *out_peer_count, uint8_t **out_data, size_t *out_data_len) {
    err_t rc = E__SUCCESS;
    uint8_t *data = NULL;
    uint32_t discovered_node_id = 0;
    uint32_t *learned_peers = NULL;
    size_t learned_count = 0;

    VALIDATE_ARGS(rt, t);

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

    uint32_t orig_src_node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->orig_src_node_id);
    uint32_t src_node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->src_node_id);
    uint32_t dst_node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->dst_node_id);
    uint32_t message_id = PROTOCOL_FIELD_FROM_NETWORK(msg->message_id);
    uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(msg->ttl);
    msg_type_t type = msg->type;

    LOG_DEBUG("Received packet: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%d, ttl=%u, data_len=%u, fd=%d", orig_src_node_id, src_node_id, dst_node_id, message_id, type, ttl, data_length, fd);

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

    rc = SESSION__handle_message(rt, fd, orig_src_node_id, src_node_id, dst_node_id, message_id, type, ttl, data_length, data, &discovered_node_id, &learned_peers, &learned_count);
    FAIL_IF(E__SUCCESS != rc, E__SESSION__HANDLE_MESSAGE_FAILED);

    if (NULL != peer_node_id && 0 != discovered_node_id) {
        *peer_node_id = discovered_node_id;
    }

    if (NULL != out_header && header_len >= PROTOCOL_HEADER_SIZE) {
        memcpy(out_header, header_buffer, PROTOCOL_HEADER_SIZE);
    }

    if (NULL != out_peer_list && NULL != learned_peers) {
        *out_peer_list = learned_peers;
        learned_peers = NULL;
    }
    if (NULL != out_peer_count) {
        *out_peer_count = learned_count;
    }

    if (NULL != out_data && NULL != data) {
        *out_data = data;
        data = NULL;
    }
    if (NULL != out_data_len) {
        *out_data_len = data_length;
    }

l_cleanup:
    FREE(data);
    FREE(learned_peers);
    return rc;
}
