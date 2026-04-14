#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "routing.h"
#include "session.h"
#include "transport.h"

static session_t g_session;

session_t *SESSION__get_session(void) {
    return &g_session;
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
                ((uint32_t *)peer_data)[peer_count] = htonl(rt->entries[i].node_id);
                peer_count++;
            }
        }
    }

    pthread_mutex_unlock(&rt->mutex);

    protocol_msg_t msg = {0};
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)s->node_id;
    msg.src_node_id = (uint32_t)s->node_id;
    msg.dst_node_id = dst_node_id;
    msg.message_id = 0;
    msg.type = MSG__PEER_INFO;
    msg.data_length = (uint32_t)peer_data_len;
    msg.ttl = DEFAULT_TTL;



    rc = TRANSPORT__send_to_node_id(s->net, dst_node_id, &msg, peer_data);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to send PEER_INFO to node %u", dst_node_id);
    }

    free(peer_data);

l_cleanup:
    return rc;
}

static err_t send_node_init(transport_t *t) {
    err_t rc = E__SUCCESS;
    session_t *s = SESSION__get_session();

    protocol_msg_t msg = {0};
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)s->node_id;
    msg.src_node_id = (uint32_t)s->node_id;
    msg.dst_node_id = 0;
    msg.message_id = 0;
    msg.type = MSG__NODE_INIT;
    msg.data_length = 0;
    msg.ttl = DEFAULT_TTL;



    rc = TRANSPORT__send_msg(t, &msg, NULL);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to send NODE_INIT");
    }

    return rc;
}

static void broadcast_peer_info(session_t *s, uint32_t *peer_list, size_t peer_count, uint32_t exclude_node_id) {
    if (NULL == s || NULL == peer_list || 0 == peer_count) {
        return;
    }

    protocol_msg_t msg = {0};
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)s->node_id;
    msg.src_node_id = (uint32_t)s->node_id;
    msg.dst_node_id = 0;
    msg.message_id = 0;
    msg.type = MSG__PEER_INFO;
    msg.data_length = (uint32_t)(peer_count * sizeof(uint32_t));
    msg.ttl = DEFAULT_TTL;

    uint8_t *peer_data = malloc(peer_count * sizeof(uint32_t));
    if (NULL == peer_data) {
        LOG_ERROR("Failed to allocate peer data for broadcast");
        return;
    }
    for (size_t i = 0; i < peer_count; i++) {
        ((uint32_t *)peer_data)[i] = htonl(peer_list[i]);
    }

    ROUTING__broadcast(&s->routing_table, exclude_node_id, (uint32_t)s->node_id, &msg, peer_data);

    free(peer_data);
}

static void broadcast_node_disconnect_with_via(session_t *s, uint32_t disconnected_node_id, uint32_t *via_nodes, size_t via_count) {
    protocol_msg_t msg = {0};
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = disconnected_node_id;
    msg.src_node_id = (uint32_t)s->node_id;
    msg.dst_node_id = 0;
    msg.message_id = 0;
    msg.type = MSG__NODE_DISCONNECT;
    msg.data_length = (uint32_t)(via_count * sizeof(uint32_t));
    msg.ttl = DEFAULT_TTL;

    uint8_t *data = NULL;
    if (via_count > 0 && NULL != via_nodes) {
        data = malloc(via_count * sizeof(uint32_t));
        if (NULL != data) {
            for (size_t i = 0; i < via_count; i++) {
                ((uint32_t *)data)[i] = htonl(via_nodes[i]);
            }
        }
    }

    ROUTING__broadcast(&s->routing_table, disconnected_node_id, (uint32_t)s->node_id, &msg, data);

    free(data);
}

static err_t handle_node_init(session_t *s, transport_t *t, uint32_t orig_src, uint32_t src) {
    err_t rc = E__SUCCESS;

    LOG_DEBUG("Received NODE_INIT from node %u (orig_src=%u)", src, orig_src);

    if (src == orig_src) {
        if (ROUTING__is_direct(&s->routing_table, src)) {
            LOG_WARNING("Node %u is already connected, rejecting", src);
            FAIL(E__SESSION__CONNECTION_REJECTED);
        }
        rc = ROUTING__add_direct(&s->routing_table, src, t->fd);
        TRANSPORT__set_node_id(t, src);
        LOG_INFO("Node %u connected (direct)", src);
    } else {
        LOG_DEBUG("NODE_INIT via relay (src=%u, orig_src=%u), adding as via_hop", src, orig_src);
        rc = ROUTING__add_via_hop(&s->routing_table, orig_src, src);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to add route to node %u via node %u", orig_src, src);
        } else {
            LOG_INFO("Learned route to node %u via node %u", orig_src, src);
            uint32_t peer_list[1] = { orig_src };
            broadcast_peer_info(s, peer_list, 1, src);
        }
    }

l_cleanup:
    return rc;
}

static err_t handle_peer_info(session_t *s, transport_t *t, uint32_t orig_src, uint32_t src, uint32_t data_len, uint8_t *data) {
    err_t rc = E__SUCCESS;

    (void)data_len;

    LOG_DEBUG("Received PEER_INFO from node %u (orig_src=%u, data_len=%u)", src, orig_src, data_len);

    if (orig_src == src) {
        ROUTING__add_direct(&s->routing_table, orig_src, t->fd);
        TRANSPORT__set_node_id(t, orig_src);
    }

    uint32_t *peer_list = (uint32_t *)data;
    size_t peer_count = data_len / sizeof(uint32_t);

    for (size_t i = 0; i < peer_count; i++) {
        uint32_t peer_id = ntohl(peer_list[i]);
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

static err_t handle_node_disconnect(session_t *s, uint32_t orig_src, uint32_t src, uint8_t *data, uint32_t data_len) {
    LOG_INFO("Node %u disconnected, removing from routing", orig_src);
    ROUTING__remove(&s->routing_table, orig_src);
    ROUTING__remove_via_node(&s->routing_table, orig_src);

    if (NULL != data && data_len > 0) {
        uint32_t *unreachable = (uint32_t *)data;
        size_t count = data_len / sizeof(uint32_t);
        for (size_t i = 0; i < count; i++) {
            uint32_t node_id = ntohl(unreachable[i]);
            if (node_id != (uint32_t)s->node_id) {
                ROUTING__remove(&s->routing_table, node_id);
                ROUTING__remove_via_node(&s->routing_table, src);
                LOG_INFO("Node %u is now unreachable via node %u", node_id, src);
            }
        }
    }

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

void SESSION__on_connected(transport_t *t) {
    session_t *s = SESSION__get_session();
    if (NULL == s || NULL == t) {
        return;
    }

    LOG_INFO("Connection established with %s:%d", t->client_ip, t->client_port);

    if (!t->is_incoming) {
        send_node_init(t);
    }
}

void SESSION__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len) {
    session_t *s = SESSION__get_session();
    if (NULL == s || NULL == t || NULL == msg) {
        return;
    }

    (void)data_len;

    if (!PROTOCOL__validate_magic(msg->magic)) {
        LOG_WARNING("Invalid magic from %s:%d", t->client_ip, t->client_port);
        return;
    }

    uint32_t orig_src = msg->orig_src_node_id;
    uint32_t src = msg->src_node_id;
    uint32_t data_length = msg->data_length;
    msg_type_t type = (msg_type_t)msg->type;



    err_t rc = E__SUCCESS;

    switch (type) {
    case MSG__NODE_INIT:
        rc = handle_node_init(s, t, orig_src, src);
        if (E__SUCCESS == rc && orig_src == src) {
            send_peer_info(s, src);
        }
        break;
    case MSG__PEER_INFO:
        rc = handle_peer_info(s, t, orig_src, src, data_length, (uint8_t *)data);
        break;
    case MSG__NODE_DISCONNECT:
        rc = handle_node_disconnect(s, orig_src, src, (uint8_t *)data, data_length);
        break;
    case MSG__CONNECTION_REJECTED:
        rc = handle_connection_rejected(s, src);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

    if (E__SESSION__CONNECTION_REJECTED == rc) {
        LOG_WARNING("Connection rejected for node %u", src);
    }
}

void SESSION__on_disconnected(transport_t *t) {
    session_t *s = SESSION__get_session();
    uint32_t node_id = TRANSPORT__get_node_id(t);
    if (NULL == s || 0 == node_id) {
        return;
    }
    LOG_INFO("Node %u disconnected", node_id);

    size_t via_count = 0;
    uint32_t *via_nodes = ROUTING__get_via_nodes(&s->routing_table, node_id, &via_count);

    ROUTING__remove(&s->routing_table, node_id);
    ROUTING__remove_via_node(&s->routing_table, node_id);
    broadcast_node_disconnect_with_via(s, node_id, via_nodes, via_count);

    free(via_nodes);
}
