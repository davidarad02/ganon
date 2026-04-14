#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "routing.h"
#include "transport.h"

static routing_table_t *g_rt = NULL;
static routing_message_cb_t g_session_cb = NULL;
int g_node_id = 0;

void ROUTING__init_globals(IN routing_table_t *rt, IN routing_message_cb_t session_cb) {
    g_rt = rt;
    g_session_cb = session_cb;
}

static err_t ROUTING__send_to_node_id(IN uint32_t node_id, IN const protocol_msg_t *msg, IN const uint8_t *data) {
    err_t rc = E__SUCCESS;
    transport_t *t = NETWORK__get_transport(&g_network, node_id);
    if (NULL == t) {
        LOG_WARNING("No transport for node %u", node_id);
        FAIL(E__ROUTING__NODE_NOT_FOUND);
    }
    protocol_msg_t fwd_msg = *msg;
    fwd_msg.src_node_id = (uint32_t)g_node_id;
    if (fwd_msg.ttl > 0) {
        fwd_msg.ttl--;
    }
    rc = TRANSPORT__send_msg(t, &fwd_msg, data);
    FAIL_IF(E__SUCCESS != rc, rc);
l_cleanup:
    return rc;
}

void ROUTING__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len) {
    if (NULL == g_rt || NULL == msg) {
        return;
    }

    (void)data_len;

    uint32_t dst = msg->dst_node_id;
    uint32_t src = msg->src_node_id;
    uint32_t ttl = msg->ttl;

    if (ttl == 0) {
        LOG_DEBUG("Dropping message with TTL 0");
        return;
    }

    if (dst == (uint32_t)g_node_id) {
        if (NULL != g_session_cb) {
            g_session_cb(t, msg, data, data_len);
        }
        return;
    }

    if (dst == 0) {
        if (NULL != g_session_cb) {
            g_session_cb(t, msg, data, data_len);
        }
        ROUTING__broadcast(g_rt, src, (uint32_t)g_node_id, msg, data);
        return;
    }

    if (dst != (uint32_t)g_node_id && dst != 0) {
        route_entry_t entry;
        if (E__SUCCESS == ROUTING__get_route(g_rt, dst, &entry)) {
            ROUTING__send_to_node_id(entry.next_hop_node_id, msg, data);
        }
    }
}

static void ROUTING__log_table(routing_table_t *rt) {
    if (NULL == rt) {
        return;
    }
    LOG_DEBUG("Routing table (%zu entries):", rt->entry_count);
    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].route_type == ROUTE__DIRECT) {
            LOG_DEBUG("  -> node %u: direct (fd=%d)", rt->entries[i].node_id, rt->entries[i].fd);
        } else {
            LOG_DEBUG("  -> node %u: via node %u", rt->entries[i].node_id, rt->entries[i].next_hop_node_id);
        }
    }
}

err_t ROUTING__init(OUT routing_table_t *rt) {
    err_t rc = E__SUCCESS;

    if (NULL == rt) {
        FAIL(E__ROUTING__INVALID_ARG);
    }

    memset(rt, 0, sizeof(routing_table_t));
    if (0 != pthread_mutex_init(&rt->mutex, NULL)) {
        LOG_ERROR("Failed to initialize routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

l_cleanup:
    return rc;
}

void ROUTING__destroy(IN routing_table_t *rt) {
    if (NULL != rt) {
        pthread_mutex_destroy(&rt->mutex);
        memset(rt, 0, sizeof(routing_table_t));
    }
}

err_t ROUTING__add_direct(IN routing_table_t *rt, IN uint32_t node_id, IN int fd) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(rt);

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            rt->entries[i].next_hop_node_id = node_id;
            rt->entries[i].route_type = ROUTE__DIRECT;
            rt->entries[i].fd = fd;
            pthread_mutex_unlock(&rt->mutex);
            LOG_DEBUG("Routing: updated route to node %u (direct, fd=%d)", node_id, fd);
            goto l_cleanup;
        }
    }

    if (rt->entry_count >= ROUTING_TABLE_MAX_ENTRIES) {
        LOG_ERROR("Routing table full, cannot add node %u", node_id);
        pthread_mutex_unlock(&rt->mutex);
        FAIL(E__ROUTING__TABLE_FULL);
    }

    rt->entries[rt->entry_count].node_id = node_id;
    rt->entries[rt->entry_count].next_hop_node_id = node_id;
    rt->entries[rt->entry_count].route_type = ROUTE__DIRECT;
    rt->entries[rt->entry_count].fd = fd;
    rt->entry_count++;

    LOG_DEBUG("Routing: added route to node %u (direct, fd=%d)", node_id, fd);
    ROUTING__log_table(rt);

    pthread_mutex_unlock(&rt->mutex);

l_cleanup:
    return rc;
}

err_t ROUTING__add_via_hop(IN routing_table_t *rt, IN uint32_t node_id, IN uint32_t next_hop_node_id) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(rt);

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            rt->entries[i].next_hop_node_id = next_hop_node_id;
            rt->entries[i].route_type = ROUTE__VIA_HOP;
            rt->entries[i].fd = -1;
            pthread_mutex_unlock(&rt->mutex);
            LOG_INFO("Updated route to node %u via node %u", node_id, next_hop_node_id);
            ROUTING__log_table(rt);
            goto l_cleanup;
        }
    }

    if (rt->entry_count >= ROUTING_TABLE_MAX_ENTRIES) {
        LOG_ERROR("Routing table full, cannot add node %u", node_id);
        pthread_mutex_unlock(&rt->mutex);
        FAIL(E__ROUTING__TABLE_FULL);
    }

    rt->entries[rt->entry_count].node_id = node_id;
    rt->entries[rt->entry_count].next_hop_node_id = next_hop_node_id;
    rt->entries[rt->entry_count].route_type = ROUTE__VIA_HOP;
    rt->entries[rt->entry_count].fd = -1;
    rt->entry_count++;

    LOG_INFO("Added route to node %u via node %u", node_id, next_hop_node_id);
    ROUTING__log_table(rt);

    pthread_mutex_unlock(&rt->mutex);

l_cleanup:
    return rc;
}

err_t ROUTING__remove(IN routing_table_t *rt, IN uint32_t node_id) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(rt);

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            LOG_INFO("Removed route to node %u", node_id);
            rt->entry_count--;
            if (i < rt->entry_count) {
                rt->entries[i] = rt->entries[rt->entry_count];
            }
            pthread_mutex_unlock(&rt->mutex);
            ROUTING__log_table(rt);
            goto l_cleanup;
        }
    }

    pthread_mutex_unlock(&rt->mutex);
    rc = E__ROUTING__NODE_NOT_FOUND;

l_cleanup:
    return rc;
}

err_t ROUTING__remove_via_node(IN routing_table_t *rt, IN uint32_t via_node_id) {
    err_t rc = E__SUCCESS;
    size_t removed = 0;

    VALIDATE_ARGS(rt);

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].route_type == ROUTE__VIA_HOP && rt->entries[i].next_hop_node_id == via_node_id) {
            LOG_INFO("Removed route to node %u (was via node %u)", rt->entries[i].node_id, via_node_id);
            rt->entry_count--;
            if (i < rt->entry_count) {
                rt->entries[i] = rt->entries[rt->entry_count];
            }
            removed++;
            i--;
        }
    }

    pthread_mutex_unlock(&rt->mutex);

    if (removed > 0) {
        LOG_INFO("Removed %zu routes via node %u", removed, via_node_id);
        ROUTING__log_table(rt);
    }

l_cleanup:
    return rc;
}

uint32_t *ROUTING__get_via_nodes(IN routing_table_t *rt, IN uint32_t via_node_id, OUT size_t *count) {
    uint32_t *result = NULL;
    *count = 0;

    if (NULL == rt || NULL == count) {
        return NULL;
    }

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        return NULL;
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].route_type == ROUTE__VIA_HOP && rt->entries[i].next_hop_node_id == via_node_id) {
            uint32_t *new_result = realloc(result, (*count + 1) * sizeof(uint32_t));
            if (NULL == new_result) {
                pthread_mutex_unlock(&rt->mutex);
                free(result);
                *count = 0;
                return NULL;
            }
            result = new_result;
            result[*count] = rt->entries[i].node_id;
            (*count)++;
        }
    }

    pthread_mutex_unlock(&rt->mutex);
    return result;
}

err_t ROUTING__get_route(IN routing_table_t *rt, IN uint32_t node_id, OUT route_entry_t *entry) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(rt, entry);

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            *entry = rt->entries[i];
            pthread_mutex_unlock(&rt->mutex);
            goto l_cleanup;
        }
    }

    pthread_mutex_unlock(&rt->mutex);
    rc = E__ROUTING__NODE_NOT_FOUND;

l_cleanup:
    return rc;
}

err_t ROUTING__get_next_hop(IN routing_table_t *rt, IN uint32_t node_id, OUT uint32_t *next_hop) {
    err_t rc = E__SUCCESS;
    route_entry_t entry;

    VALIDATE_ARGS(rt, next_hop);

    rc = ROUTING__get_route(rt, node_id, &entry);
    if (E__SUCCESS != rc) {
        goto l_cleanup;
    }

    *next_hop = entry.next_hop_node_id;

l_cleanup:
    return rc;
}

int ROUTING__is_direct(IN routing_table_t *rt, IN uint32_t node_id) {
    route_entry_t entry;
    if (E__SUCCESS == ROUTING__get_route(rt, node_id, &entry)) {
        return entry.route_type == ROUTE__DIRECT;
    }
    return 0;
}

err_t ROUTING__send_to_node(IN routing_table_t *rt, IN uint32_t node_id, IN const uint8_t *buf, IN size_t len, IN ssize_t (*send_fn)(IN int, IN const uint8_t *, IN size_t)) {
    err_t rc = E__SUCCESS;
    route_entry_t entry;

    if (NULL == rt || NULL == buf) {
        LOG_ERROR("Invalid arguments to ROUTING__send_to_node");
        FAIL(E__ROUTING__INVALID_ARG);
    }

    rc = ROUTING__get_route(rt, node_id, &entry);
    if (E__SUCCESS != rc) {
        LOG_WARNING("No route to node %u", node_id);
        FAIL(rc);
    }

    if (entry.route_type == ROUTE__DIRECT) {
        ssize_t sent = send_fn(entry.fd, buf, len);
        if (0 > sent) {
            LOG_WARNING("Failed to send to node %u (fd=%d)", node_id, entry.fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        LOG_DEBUG("Sent %zd bytes to node %u (direct)", sent, node_id);
    } else {
        rc = ROUTING__send_to_node(rt, entry.next_hop_node_id, buf, len, send_fn);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to forward to node %u via node %u", node_id, entry.next_hop_node_id);
            FAIL(rc);
        }
    }

l_cleanup:
    return rc;
}

void ROUTING__broadcast(IN routing_table_t *rt, IN uint32_t exclude_node_id, IN uint32_t src_node_id, IN const protocol_msg_t *msg, IN const uint8_t *data) {
    if (NULL == rt || NULL == msg) {
        return;
    }

    if (0 != pthread_mutex_lock(&rt->mutex)) {
        LOG_ERROR("Failed to lock routing table mutex");
        return;
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        uint32_t node_id = rt->entries[i].node_id;
        if (node_id != exclude_node_id && rt->entries[i].route_type == ROUTE__DIRECT) {
            transport_t *t = NETWORK__get_transport(&g_network, node_id);
            if (NULL != t) {
                protocol_msg_t broadcast_msg = *msg;
                broadcast_msg.src_node_id = src_node_id;
                if (broadcast_msg.ttl > 0) {
                    broadcast_msg.ttl--;
                }
                TRANSPORT__send_msg(t, &broadcast_msg, data);
            }
        }
    }

    pthread_mutex_unlock(&rt->mutex);
}