#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "logging.h"
#include "routing.h"

err_t ROUTING__init(routing_table_t *rt) {
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

void ROUTING__destroy(routing_table_t *rt) {
    if (NULL != rt) {
        pthread_mutex_destroy(&rt->mutex);
        memset(rt, 0, sizeof(routing_table_t));
    }
}

err_t ROUTING__add_direct(routing_table_t *rt, uint32_t node_id, int fd) {
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

    pthread_mutex_unlock(&rt->mutex);

l_cleanup:
    return rc;
}

err_t ROUTING__add_via_hop(routing_table_t *rt, uint32_t node_id, uint32_t next_hop_node_id) {
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

    pthread_mutex_unlock(&rt->mutex);

l_cleanup:
    return rc;
}

err_t ROUTING__remove(routing_table_t *rt, uint32_t node_id) {
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
            goto l_cleanup;
        }
    }

    pthread_mutex_unlock(&rt->mutex);
    rc = E__ROUTING__NODE_NOT_FOUND;

l_cleanup:
    return rc;
}

err_t ROUTING__remove_via_node(routing_table_t *rt, uint32_t via_node_id) {
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
    }

l_cleanup:
    return rc;
}

err_t ROUTING__get_route(routing_table_t *rt, uint32_t node_id, route_entry_t *entry) {
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

err_t ROUTING__get_next_hop(routing_table_t *rt, uint32_t node_id, uint32_t *next_hop) {
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

int ROUTING__is_direct(routing_table_t *rt, uint32_t node_id) {
    route_entry_t entry;
    if (E__SUCCESS == ROUTING__get_route(rt, node_id, &entry)) {
        return entry.route_type == ROUTE__DIRECT;
    }
    return 0;
}

err_t ROUTING__send_to_node(routing_table_t *rt, uint32_t node_id, const uint8_t *buf, size_t len, ssize_t (*send_fn)(int, const uint8_t *, size_t)) {
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
