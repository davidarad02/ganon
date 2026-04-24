#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "routing.h"
#include "session.h"
#include "transport.h"
#include "tunnel.h"

static session_t g_session;
static uint32_t g_msg_seq_id = 0;

session_t *SESSION__get_session(void) {
    return &g_session;
}

uint32_t SESSION__get_next_msg_id(void) {
    return __atomic_add_fetch(&g_msg_seq_id, 1, __ATOMIC_SEQ_CST);
}

static err_t SESSION__send_node_init(IN transport_t *t) {
    err_t rc = E__SUCCESS;
    session_t *s = SESSION__get_session();
    protocol_msg_t msg;

    memset(&msg, 0, sizeof(msg));
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)s->node_id;
    msg.src_node_id = (uint32_t)s->node_id;
    msg.dst_node_id = 0;
    msg.message_id = SESSION__get_next_msg_id();
    msg.type = MSG__NODE_INIT;
    msg.data_length = 0;
    msg.ttl = 1;

    rc = TRANSPORT__send_msg(t, &msg, NULL);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    return rc;
}

static err_t SESSION__handle_node_init(IN session_t *s, IN transport_t *t, IN uint32_t orig_src, IN uint32_t src) {
    err_t rc = E__SUCCESS;

    LOG_DEBUG("Received NODE_INIT from node %u (orig_src=%u)", src, orig_src);

    if (orig_src == src) {
        transport_t *old_t = NETWORK__get_transport(s->net, src);
        if (NULL != old_t && old_t != t) {
            LOG_WARNING("Node %u reconnected, closing old session", src);
            // Mark old session as no longer owning this node_id to prevent other 
            // threads from trying to use the transport we are about to close.
            old_t->node_id = 0;
            close(old_t->fd);
            old_t->fd = -1;
        }
        rc = ROUTING__add_direct(&s->routing_table, src, t->fd);
        FAIL_IF(E__SUCCESS != rc, rc);
        TRANSPORT__set_node_id(t, src);
        LOG_INFO("Node %u connected (direct)", src);
        
        if (t->is_incoming) {
            SESSION__send_node_init(t);
        }
        
        // When a new neighbor connects, rediscover paths for our active routes
        // to allow the new neighbor to participate in load-balancing.
        ROUTING__rediscover_active_routes(&s->routing_table);
    }

l_cleanup:
    return rc;
}

static err_t SESSION__handle_connection_rejected(IN session_t *s, IN uint32_t src) {
    err_t rc = E__SUCCESS;
    (void)s;
    (void)src;
    LOG_DEBUG("Received CONNECTION_REJECTED from node %u", src);
    rc = E__SESSION__CONNECTION_REJECTED;
    goto l_cleanup;
l_cleanup:
    return rc;
}

static err_t SESSION__handle_connect_cmd(IN session_t *s, IN transport_t *t, IN const protocol_msg_t *msg, 
                                          IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    (void)t;
    (void)msg;
    
    if (data_len < sizeof(connect_cmd_payload_t)) {
        LOG_WARNING("CONNECT_CMD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }
    
    const connect_cmd_payload_t *p = (const connect_cmd_payload_t *)data;
    char ip[65];
    strncpy(ip, p->target_ip, 64);
    ip[64] = '\0';
    uint32_t port = ntohl(p->target_port);
    
    LOG_INFO("Received CONNECT_CMD from node %u: connect to %s:%u", 
             msg->orig_src_node_id, ip, port);
    
    /* Attempt to connect to the target */
    int status = CONNECT_STATUS_SUCCESS;
    uint32_t error_code = 0;
    
    err_t connect_rc = NETWORK__connect_to_peer(s->net, ip, (int)port, &status, &error_code);
    if (E__SUCCESS != connect_rc) {
        status = CONNECT_STATUS_ERROR;
        error_code = (uint32_t)connect_rc;
    }
    
    /* Send response back to originator */
    connect_response_payload_t resp;
    resp.status = htonl((uint32_t)status);
    resp.error_code = htonl(error_code);
    
    protocol_msg_t response_msg;
    memset(&response_msg, 0, sizeof(response_msg));
    memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
    response_msg.orig_src_node_id = (uint32_t)s->node_id;
    response_msg.src_node_id = (uint32_t)s->node_id;
    response_msg.dst_node_id = msg->orig_src_node_id;
    response_msg.message_id = SESSION__get_next_msg_id();
    response_msg.type = MSG__CONNECT_RESPONSE;
    response_msg.data_length = sizeof(resp);
    response_msg.ttl = DEFAULT_TTL;
    response_msg.channel_id = 0;
    
    ROUTING__route_message(&response_msg, (const uint8_t *)&resp, 0);
    
    LOG_INFO("CONNECT_CMD response sent to node %u: status=%d", 
             msg->orig_src_node_id, status);

l_cleanup:
    return rc;
}

static err_t SESSION__handle_disconnect_cmd(IN session_t *s, IN transport_t *t, IN const protocol_msg_t *msg,
                                             IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    (void)t;
    
    if (data_len < sizeof(disconnect_cmd_payload_t)) {
        LOG_WARNING("DISCONNECT_CMD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }
    
    const disconnect_cmd_payload_t *p = (const disconnect_cmd_payload_t *)data;
    uint32_t node_a = ntohl(p->node_a);
    uint32_t node_b = ntohl(p->node_b);
    
    LOG_INFO("Received DISCONNECT_CMD from node %u: disconnect %u from %u",
             msg->orig_src_node_id, node_a, node_b);
    
    /* Check if we are node_a (the initiator) */
    if ((uint32_t)s->node_id != node_a) {
        LOG_WARNING("DISCONNECT_CMD received but we are not node_a (%u != %u)", 
                    s->node_id, node_a);
        /* Forward to node_a if we have a route */
        /* For now, just report error */
        int status = DISCONNECT_STATUS_ERROR;
        uint32_t error_code = E__SESSION__INVALID_MESSAGE;
        
        disconnect_response_payload_t resp;
        resp.status = htonl((uint32_t)status);
        resp.error_code = htonl(error_code);
        
        protocol_msg_t response_msg;
        memset(&response_msg, 0, sizeof(response_msg));
        memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
        response_msg.orig_src_node_id = (uint32_t)s->node_id;
        response_msg.src_node_id = (uint32_t)s->node_id;
        response_msg.dst_node_id = msg->orig_src_node_id;
        response_msg.message_id = SESSION__get_next_msg_id();
        response_msg.type = MSG__DISCONNECT_RESPONSE;
        response_msg.data_length = sizeof(resp);
        response_msg.ttl = DEFAULT_TTL;
        response_msg.channel_id = 0;
        
        ROUTING__route_message(&response_msg, (const uint8_t *)&resp, 0);
        goto l_cleanup;
    }
    
    /* We are node_a, perform the disconnect */
    int status = DISCONNECT_STATUS_SUCCESS;
    uint32_t error_code = 0;
    
    err_t disconnect_rc = NETWORK__disconnect_from_peer(s->net, node_b, &status, &error_code);
    if (E__SUCCESS != disconnect_rc) {
        status = DISCONNECT_STATUS_ERROR;
        error_code = (uint32_t)disconnect_rc;
    }
    
    /* Send response back to originator */
    disconnect_response_payload_t resp;
    resp.status = htonl((uint32_t)status);
    resp.error_code = htonl(error_code);
    
    protocol_msg_t response_msg;
    memset(&response_msg, 0, sizeof(response_msg));
    memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
    response_msg.orig_src_node_id = (uint32_t)s->node_id;
    response_msg.src_node_id = (uint32_t)s->node_id;
    response_msg.dst_node_id = msg->orig_src_node_id;
    response_msg.message_id = SESSION__get_next_msg_id();
    response_msg.type = MSG__DISCONNECT_RESPONSE;
    response_msg.data_length = sizeof(resp);
    response_msg.ttl = DEFAULT_TTL;
    response_msg.channel_id = 0;
    
    ROUTING__route_message(&response_msg, (const uint8_t *)&resp, 0);
    
    LOG_INFO("DISCONNECT_CMD response sent to node %u: status=%d",
             msg->orig_src_node_id, status);

l_cleanup:
    return rc;
}

err_t SESSION__init(INOUT session_t *s, IN int node_id) {
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

void SESSION__destroy(IN session_t *s) {
    if (NULL == s) {
        return;
    }
    ROUTING__destroy(&s->routing_table);
}

void SESSION__set_network(IN session_t *s, IN network_t *net) {
    if (NULL == s) {
        return;
    }
    s->net = net;
}

network_t *SESSION__get_network(IN session_t *s) {
    if (NULL == s) {
        return NULL;
    }
    return s->net;
}

int SESSION__get_node_id(IN session_t *s) {
    if (NULL == s) {
        return -1;
    }
    return s->node_id;
}

routing_table_t *SESSION__get_routing_table(IN session_t *s) {
    if (NULL == s) {
        return NULL;
    }
    return &s->routing_table;
}

void SESSION__on_connected(IN transport_t *t) {
    session_t *s = SESSION__get_session();
    if (NULL == s || NULL == t) {
        return;
    }

    LOG_INFO("Connection established with %s:%d", t->client_ip, t->client_port);

    if (!t->is_incoming) {
        SESSION__send_node_init(t);
    }
}

void SESSION__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    session_t *s = SESSION__get_session();

    if (NULL == s || NULL == t || NULL == msg) {
        goto l_cleanup;
    }
    (void)data_len;
    (void)data;

    if (!PROTOCOL__validate_magic(msg->magic)) {
        LOG_WARNING("Invalid magic from %s:%d", t->client_ip, t->client_port);
        goto l_cleanup;
    }

    uint32_t orig_src = msg->orig_src_node_id;
    uint32_t src = msg->src_node_id;
    msg_type_t type = (msg_type_t)msg->type;

    switch (type) {
    case MSG__NODE_INIT:
        rc = SESSION__handle_node_init(s, t, orig_src, src);
        break;
    case MSG__CONNECTION_REJECTED:
        rc = SESSION__handle_connection_rejected(s, src);
        break;
    case MSG__USER_DATA:
        LOG_INFO("Received USER_DATA from node %u! Length: %zu", orig_src, data_len);
        break;
    case MSG__PING:
        {
            protocol_msg_t pong_msg;
            LOG_INFO("Received PING from node %u, sending PONG replica", orig_src);
            memset(&pong_msg, 0, sizeof(pong_msg));
            memcpy(pong_msg.magic, GANON_PROTOCOL_MAGIC, 4);
            pong_msg.orig_src_node_id = (uint32_t)s->node_id;
            pong_msg.src_node_id = (uint32_t)s->node_id;
            pong_msg.dst_node_id = orig_src;
            pong_msg.message_id = SESSION__get_next_msg_id();
            pong_msg.type = MSG__PONG;
            pong_msg.data_length = (uint32_t)data_len;
            pong_msg.ttl = DEFAULT_TTL;
            pong_msg.channel_id = msg->channel_id;

            ROUTING__route_message(&pong_msg, data, 0);
        }
        break;
    case MSG__PONG:
        LOG_INFO("Received PONG from node %u! Length: %zu", orig_src, data_len);
        break;
    case MSG__RREQ:
    case MSG__RREP:
    case MSG__RERR:
        break;
    case MSG__TUNNEL_OPEN:
    case MSG__TUNNEL_CONN_OPEN:
    case MSG__TUNNEL_CONN_ACK:
    case MSG__TUNNEL_DATA:
    case MSG__TUNNEL_CONN_CLOSE:
    case MSG__TUNNEL_CLOSE:
        TUNNEL__on_message(t, msg, data, data_len);
        break;
    case MSG__CONNECT_CMD:
        rc = SESSION__handle_connect_cmd(s, t, msg, data, data_len);
        break;
    case MSG__DISCONNECT_CMD:
        rc = SESSION__handle_disconnect_cmd(s, t, msg, data, data_len);
        break;
    case MSG__CONNECT_RESPONSE:
    case MSG__DISCONNECT_RESPONSE:
        /* These are handled by the waiting client or can be logged */
        LOG_INFO("Received %s from node %u", 
                 (type == MSG__CONNECT_RESPONSE) ? "CONNECT_RESPONSE" : "DISCONNECT_RESPONSE",
                 orig_src);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

    if (E__SESSION__CONNECTION_REJECTED == rc) {
        LOG_WARNING("Connection rejected for node %u", src);
    }

l_cleanup:
    (void)rc;
}

void SESSION__on_disconnected(IN transport_t *t) {
    session_t *s = SESSION__get_session();
    uint32_t node_id = TRANSPORT__get_node_id(t);

    if (NULL == s || 0 == node_id) {
        return;
    }
    
    LOG_INFO("Node %u disconnected", node_id);
    ROUTING__handle_disconnect(node_id);
    TUNNEL__handle_disconnect(node_id);
}
