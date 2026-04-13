#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "connection.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "routing.h"
#include "session.h"

static void log_received_packet(const protocol_msg_t *msg, uint32_t data_len) {
    (void)msg;
    (void)data_len;
}

static void log_sent_packet(const protocol_msg_t *msg, uint32_t data_len) {
    (void)msg;
    (void)data_len;
}

static err_t send_peer_info(session_t *s, uint32_t dst_node_id) {
    err_t rc = E__SUCCESS;

    routing_table_t *rt = &s->routing_table;

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    size_t peer_count = 0;
    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id != dst_node_id) {
            peer_count++;
        }
    }

    uint8_t *peer_data = NULL;
    size_t peer_data_len = peer_count * sizeof(uint32_t);
    if (peer_count > 0) {
        peer_data = malloc(peer_data_len);
        if (NULL == peer_data) {
            LOG_ERROR("Failed to allocate peer data");
            pthread_mutex_unlock(&rt->mutex);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }

        peer_count = 0;
        for (size_t i = 0; i < rt->entry_count; i++) {
            if (rt->entries[i].node_id != dst_node_id) {
                ((uint32_t *)peer_data)[peer_count] = PROTOCOL_FIELD_TO_NETWORK(rt->entries[i].node_id);
                peer_count++;
            }
        }
    }

    pthread_mutex_unlock(&rt->mutex);

    uint8_t header[PROTOCOL_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    protocol_msg_t *msg = (protocol_msg_t *)header;
    memcpy(msg->magic, GANON_PROTOCOL_MAGIC, 4);
    msg->orig_src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->dst_node_id = PROTOCOL_FIELD_TO_NETWORK(dst_node_id);
    msg->message_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->type = PROTOCOL_FIELD_TO_NETWORK((uint32_t)MSG__PEER_INFO);
    msg->data_length = PROTOCOL_FIELD_TO_NETWORK((uint32_t)peer_data_len);
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(DEFAULT_TTL);

    size_t total_len = PROTOCOL_HEADER_SIZE + peer_data_len;
    uint8_t *pkt = malloc(total_len);
    if (NULL == pkt) {
        LOG_ERROR("Failed to allocate packet");
        FREE(peer_data);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    memcpy(pkt, header, PROTOCOL_HEADER_SIZE);
    if (NULL != peer_data && peer_data_len > 0) {
        memcpy(pkt + PROTOCOL_HEADER_SIZE, peer_data, peer_data_len);
    }

    NETWORK__send_to(s->net, dst_node_id, pkt, total_len);
    log_sent_packet(msg, (uint32_t)peer_data_len);

    free(pkt);
    free(peer_data);

l_cleanup:
    return rc;
}

static err_t send_node_init(session_t *s, uint32_t dst_node_id) {
    err_t rc = E__SUCCESS;

    uint8_t header[PROTOCOL_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    protocol_msg_t *msg = (protocol_msg_t *)header;
    memcpy(msg->magic, GANON_PROTOCOL_MAGIC, 4);
    msg->orig_src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->dst_node_id = PROTOCOL_FIELD_TO_NETWORK(dst_node_id);
    msg->message_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->type = PROTOCOL_FIELD_TO_NETWORK((uint32_t)MSG__NODE_INIT);
    msg->data_length = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(DEFAULT_TTL);

    NETWORK__send_to(s->net, dst_node_id, header, PROTOCOL_HEADER_SIZE);
    log_sent_packet(msg, 0);

    return rc;
}

static err_t send_to_node(session_t *s, uint32_t dst_node_id, const uint8_t *buf, size_t len) {
    if (NULL == s || NULL == buf) {
        return E__INVALID_ARG_NULL_POINTER;
    }
    return NETWORK__send_to(s->net, dst_node_id, buf, len);
}

static void broadcast_to_others(session_t *s, uint32_t exclude_node_id, const protocol_msg_t *msg, const uint8_t *data, size_t data_len) {
    routing_table_t *rt = &s->routing_table;

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        return;
    }

    uint8_t header[PROTOCOL_HEADER_SIZE];
    memcpy(header, msg, PROTOCOL_HEADER_SIZE);
    protocol_msg_t *hdr = (protocol_msg_t *)header;
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(hdr->ttl);
    if (ttl > 0) {
        hdr->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
        hdr->ttl = PROTOCOL_FIELD_TO_NETWORK(ttl - 1);
    }

    size_t total_len = PROTOCOL_HEADER_SIZE + data_len;
    uint8_t *pkt = malloc(total_len);
    if (NULL == pkt) {
        LOG_ERROR("Failed to allocate broadcast packet");
        pthread_mutex_unlock(&rt->mutex);
        return;
    }
    memcpy(pkt, header, PROTOCOL_HEADER_SIZE);
    if (NULL != data && data_len > 0) {
        memcpy(pkt + PROTOCOL_HEADER_SIZE, data, data_len);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        uint32_t node_id = rt->entries[i].node_id;
        if (node_id != exclude_node_id && rt->entries[i].route_type == ROUTE__DIRECT) {
            LOG_DEBUG("Broadcast to node %u (ttl=%u)", node_id, ttl - 1);
            send_to_node(s, node_id, pkt, total_len);
        }
    }

    free(pkt);
    pthread_mutex_unlock(&rt->mutex);
}

static void broadcast_peer_info(session_t *s, uint32_t *peer_list, size_t peer_count, uint32_t exclude_node_id) {
    if (NULL == s || NULL == peer_list || 0 == peer_count) {
        return;
    }

    uint8_t header[PROTOCOL_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    protocol_msg_t *msg = (protocol_msg_t *)header;
    memcpy(msg->magic, GANON_PROTOCOL_MAGIC, 4);
    msg->orig_src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->dst_node_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->message_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->type = PROTOCOL_FIELD_TO_NETWORK((uint32_t)MSG__PEER_INFO);
    msg->data_length = PROTOCOL_FIELD_TO_NETWORK((uint32_t)(peer_count * sizeof(uint32_t)));
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(DEFAULT_TTL);

    uint8_t *peer_data = malloc(peer_count * sizeof(uint32_t));
    if (NULL == peer_data) {
        LOG_ERROR("Failed to allocate peer data for broadcast");
        return;
    }
    for (size_t i = 0; i < peer_count; i++) {
        ((uint32_t *)peer_data)[i] = peer_list[i];
    }

    broadcast_to_others(s, exclude_node_id, msg, peer_data, peer_count * sizeof(uint32_t));
    free(peer_data);
}

static void broadcast_node_disconnect(session_t *s, uint32_t disconnected_node_id) {
    uint8_t header[PROTOCOL_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    protocol_msg_t *msg = (protocol_msg_t *)header;
    memcpy(msg->magic, GANON_PROTOCOL_MAGIC, 4);
    msg->orig_src_node_id = PROTOCOL_FIELD_TO_NETWORK(disconnected_node_id);
    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    msg->dst_node_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->message_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->type = PROTOCOL_FIELD_TO_NETWORK((uint32_t)MSG__NODE_DISCONNECT);
    msg->data_length = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(DEFAULT_TTL);

    broadcast_to_others(s, disconnected_node_id, msg, NULL, 0);
}

static err_t forward_message(session_t *s, const protocol_msg_t *msg, const uint8_t *data, size_t data_len) {
    err_t rc = E__SUCCESS;

    uint32_t dst_node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->dst_node_id);
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(msg->ttl);

    if (dst_node_id == (uint32_t)s->node_id) {
        return E__SUCCESS;
    }

    if (ttl == 0) {
        LOG_DEBUG("TTL 0, dropping forwarded message to node %u", dst_node_id);
        return E__SUCCESS;
    }

    routing_table_t *rt = &s->routing_table;
    route_entry_t entry;
    rc = ROUTING__get_route(rt, dst_node_id, &entry);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Cannot forward: no route to node %u", dst_node_id);
        return rc;
    }

    uint8_t header[PROTOCOL_HEADER_SIZE];
    memcpy(header, msg, PROTOCOL_HEADER_SIZE);
    protocol_msg_t *hdr = (protocol_msg_t *)header;
    hdr->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)s->node_id);
    hdr->ttl = PROTOCOL_FIELD_TO_NETWORK(ttl > 0 ? ttl - 1 : 0);

    size_t total_len = PROTOCOL_HEADER_SIZE + data_len;
    uint8_t *pkt = malloc(total_len);
    if (NULL == pkt) {
        LOG_ERROR("Failed to allocate forward packet");
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    memcpy(pkt, header, PROTOCOL_HEADER_SIZE);
    if (NULL != data && data_len > 0) {
        memcpy(pkt + PROTOCOL_HEADER_SIZE, data, data_len);
    }

    rc = send_to_node(s, entry.next_hop_node_id, pkt, total_len);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to forward message to node %u", dst_node_id);
    } else {
        LOG_DEBUG("Forwarded message to node %u (ttl=%u)", dst_node_id, ttl - 1);
    }

    free(pkt);

l_cleanup:
    return rc;
}

static err_t handle_node_init(session_t *s, connection_t *conn, uint32_t orig_src, uint32_t src, uint32_t ttl, uint32_t data_len) {
    err_t rc = E__SUCCESS;

    (void)ttl;
    (void)data_len;

    LOG_DEBUG("Received NODE_INIT from node %u (orig_src=%u, ttl=%u, data_len=%u)", src, orig_src, ttl, data_len);

    if (src == orig_src) {
        if (ROUTING__is_direct(&s->routing_table, src)) {
            LOG_WARNING("Node %u is already connected, rejecting", src);
            FAIL(E__SESSION__CONNECTION_REJECTED);
        }
        rc = ROUTING__add_direct(&s->routing_table, src, conn->fd);
        CONNECTION__set_node_id(conn, src);
        LOG_INFO("Node %u connected (direct)", src);
    } else {
        LOG_DEBUG("NODE_INIT via relay (src=%u, orig_src=%u), adding as via_hop", src, orig_src);
        rc = ROUTING__add_via_hop(&s->routing_table, orig_src, src);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to add route to node %u via node %u", orig_src, src);
        } else {
            LOG_INFO("Learned route to node %u via node %u", orig_src, src);
            uint32_t peer_list[1] = { PROTOCOL_FIELD_TO_NETWORK(orig_src) };
            broadcast_peer_info(s, peer_list, 1, src);
        }
    }

l_cleanup:
    return rc;
}

static err_t handle_peer_info(session_t *s, uint32_t orig_src, uint32_t src, uint32_t data_len, uint8_t *data) {
    err_t rc = E__SUCCESS;

    (void)orig_src;
    (void)src;
    (void)data_len;
    (void)data;

    LOG_DEBUG("Received PEER_INFO from node %u (orig_src=%u, data_len=%u)", src, orig_src, data_len);

    uint32_t *peer_list = (uint32_t *)data;
    size_t peer_count = data_len / sizeof(uint32_t);

    for (size_t i = 0; i < peer_count; i++) {
        uint32_t peer_id = PROTOCOL_FIELD_FROM_NETWORK(peer_list[i]);
        if (peer_id == (uint32_t)s->node_id) {
            continue;
        }
        rc = ROUTING__add_via_hop(&s->routing_table, peer_id, src);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to add route to node %u via node %u", peer_id, src);
        } else {
            LOG_DEBUG("Learned route to node %u via node %u", peer_id, src);
        }
    }

    if (peer_count > 0) {
        broadcast_peer_info(s, peer_list, peer_count, src);
    }

    return rc;
}

static err_t handle_node_disconnect(session_t *s, uint32_t orig_src) {
    LOG_INFO("Node %u disconnected, removing from routing", orig_src);
    ROUTING__remove(&s->routing_table, orig_src);
    ROUTING__remove_via_node(&s->routing_table, orig_src);
    return E__SUCCESS;
}

static err_t handle_connection_rejected(session_t *s, uint32_t src) {
    (void)s;
    (void)src;
    LOG_DEBUG("Received CONNECTION_REJECTED from node %u", src);
    return E__SESSION__CONNECTION_REJECTED;
}

err_t SESSION__init(session_t *s, int node_id) {
    err_t rc = E__SUCCESS;

    if (NULL == s) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    memset(s, 0, sizeof(session_t));
    s->node_id = node_id;
    s->net = NULL;

    rc = ROUTING__init(&s->routing_table);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Failed to initialize routing table");
        FAIL(rc);
    }

l_cleanup:
    return rc;
}

void SESSION__destroy(session_t *s) {
    if (NULL == s) {
        return;
    }
    ROUTING__destroy(&s->routing_table);
}

void SESSION__set_network(session_t *s, network_t *net) {
    if (NULL == s) {
        return;
    }
    s->net = net;
}

network_t *SESSION__get_network(session_t *s) {
    if (NULL == s) {
        return NULL;
    }
    return s->net;
}

int SESSION__get_node_id(session_t *s) {
    if (NULL == s) {
        return -1;
    }
    return s->node_id;
}

routing_table_t *SESSION__get_routing_table(session_t *s) {
    if (NULL == s) {
        return NULL;
    }
    return &s->routing_table;
}

void SESSION__on_connected(session_t *s, connection_t *conn) {
    if (NULL == s || NULL == conn) {
        return;
    }

    LOG_INFO("Connection established with %s:%d", conn->client_ip, conn->client_port);

    if (!conn->is_incoming) {
        send_node_init(s, 0);
    }
}

void SESSION__on_message(session_t *s, connection_t *conn, const uint8_t *buf, size_t len) {
    if (NULL == s || NULL == conn || NULL == buf || len < PROTOCOL_HEADER_SIZE) {
        return;
    }

    protocol_msg_t *msg = (protocol_msg_t *)buf;
    uint8_t *data = NULL;
    size_t data_len = 0;

    if (0 != strncmp(msg->magic, GANON_PROTOCOL_MAGIC, 4)) {
        LOG_WARNING("Invalid magic from %s:%d", conn->client_ip, conn->client_port);
        return;
    }

    uint32_t orig_src = PROTOCOL_FIELD_FROM_NETWORK(msg->orig_src_node_id);
    uint32_t src = PROTOCOL_FIELD_FROM_NETWORK(msg->src_node_id);
    uint32_t dst = PROTOCOL_FIELD_FROM_NETWORK(msg->dst_node_id);
    uint32_t data_length = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(msg->ttl);
    msg_type_t type = (msg_type_t)PROTOCOL_FIELD_FROM_NETWORK(msg->type);

    log_received_packet(msg, data_length);

    if (data_length > 0 && len >= PROTOCOL_HEADER_SIZE + data_length) {
        data_len = data_length;
        data = (uint8_t *)buf + PROTOCOL_HEADER_SIZE;
    }

    err_t rc = E__SUCCESS;

    switch (type) {
    case MSG__NODE_INIT:
        rc = handle_node_init(s, conn, orig_src, src, ttl, data_length);
        if (E__SUCCESS == rc && orig_src == src) {
            send_peer_info(s, src);
        }
        break;
    case MSG__PEER_INFO:
        rc = handle_peer_info(s, orig_src, src, data_length, data);
        break;
    case MSG__NODE_DISCONNECT:
        rc = handle_node_disconnect(s, orig_src);
        broadcast_node_disconnect(s, orig_src);
        break;
    case MSG__CONNECTION_REJECTED:
        rc = handle_connection_rejected(s, src);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

    if (dst == 0) {
        uint32_t exclude_node_id = (src == (uint32_t)s->node_id) ? orig_src : src;
        protocol_msg_t forward_msg;
        memcpy(&forward_msg, msg, sizeof(forward_msg));
        broadcast_to_others(s, exclude_node_id, &forward_msg, data, data_len);
    } else if (dst != (uint32_t)s->node_id) {
        forward_message(s, msg, data, data_len);
    }

    if (E__SESSION__CONNECTION_REJECTED == rc) {
        LOG_WARNING("Connection rejected for node %u", src);
    }
}

void SESSION__on_disconnected(session_t *s, uint32_t node_id) {
    if (NULL == s || 0 == node_id) {
        return;
    }
    LOG_INFO("Node %u disconnected", node_id);
    ROUTING__remove(&s->routing_table, node_id);
    ROUTING__remove_via_node(&s->routing_table, node_id);
    broadcast_node_disconnect(s, node_id);
}
