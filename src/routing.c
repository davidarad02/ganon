#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "routing.h"
#include "transport.h"
#include "loadbalancer.h"

static routing_table_t *g_rt = NULL;
static routing_message_cb_t g_session_cb = NULL;
int g_node_id = 0;

static uint32_t g_msg_id_counter = 1;
static pthread_mutex_t g_msg_id_mutex = PTHREAD_MUTEX_INITIALIZER;

#define SEEN_CACHE_MAX 1024
typedef struct {
    uint32_t orig_src;
    uint32_t msg_id;
} seen_msg_t;
static seen_msg_t g_seen_msgs[SEEN_CACHE_MAX];
static size_t g_seen_msgs_idx = 0;
static pthread_mutex_t g_seen_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_PENDING_PACKETS 32
typedef struct {
    uint32_t dst;
    protocol_msg_t msg;
    uint8_t *data;
    time_t timestamp;
} pending_packet_t;

static pending_packet_t g_pending[MAX_PENDING_PACKETS] = {0};
static pthread_mutex_t g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ROUTING__buffer_message(uint32_t dst, const protocol_msg_t *msg, const uint8_t *data) {
    if (0 != pthread_mutex_lock(&g_pending_mutex)) return;
    time_t now = time(NULL);
    for (int i=0; i<MAX_PENDING_PACKETS; i++) {
        if (g_pending[i].timestamp == 0 || (now - g_pending[i].timestamp) > 5) {
            FREE(g_pending[i].data);
            g_pending[i].dst = dst;
            g_pending[i].msg = *msg;
            if (msg->data_length > 0 && NULL != data) {
                g_pending[i].data = malloc(msg->data_length);
                if (NULL != g_pending[i].data) memcpy(g_pending[i].data, data, msg->data_length);
            } else {
                g_pending[i].data = NULL;
            }
            g_pending[i].timestamp = now;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);
}

static void ROUTING__flush_pending(uint32_t dst) {
    if (0 != pthread_mutex_lock(&g_pending_mutex)) return;
    for (int i=0; i<MAX_PENDING_PACKETS; i++) {
        if (g_pending[i].timestamp != 0 && g_pending[i].dst == dst) {
            LOG_DEBUG("Flushing buffered message for newly mapped destination %u", dst);
            protocol_msg_t pending_msg = g_pending[i].msg;
            uint8_t *pending_data = g_pending[i].data;
            g_pending[i].data = NULL;
            g_pending[i].timestamp = 0;
            pthread_mutex_unlock(&g_pending_mutex);
            
            ROUTING__route_message(&pending_msg, pending_data, 0);
            FREE(pending_data);
            
            if (0 != pthread_mutex_lock(&g_pending_mutex)) return;
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);
}

static err_t ROUTING__is_msg_seen(IN uint32_t orig_src, IN uint32_t msg_id, OUT int *seen) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(seen);
    *seen = 0;

    if (0 != pthread_mutex_lock(&g_seen_mutex)) {
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }
    
    for (size_t i = 0; i < SEEN_CACHE_MAX; i++) {
        if (g_seen_msgs[i].orig_src == orig_src && g_seen_msgs[i].msg_id == msg_id) {
            *seen = 1;
            pthread_mutex_unlock(&g_seen_mutex);
            goto l_cleanup;
        }
    }
    g_seen_msgs[g_seen_msgs_idx].orig_src = orig_src;
    g_seen_msgs[g_seen_msgs_idx].msg_id = msg_id;
    g_seen_msgs_idx = (g_seen_msgs_idx + 1) % SEEN_CACHE_MAX;
    
    pthread_mutex_unlock(&g_seen_mutex);

l_cleanup:
    return rc;
}

void ROUTING__clear_seen_for_node(IN uint32_t node_id) {
    if (0 != pthread_mutex_lock(&g_seen_mutex)) return;
    for (size_t i = 0; i < SEEN_CACHE_MAX; i++) {
        if (g_seen_msgs[i].orig_src == node_id) {
            g_seen_msgs[i].orig_src = 0;
            g_seen_msgs[i].msg_id = 0;
        }
    }
    pthread_mutex_unlock(&g_seen_mutex);
}

void ROUTING__clear_state_for_node(IN uint32_t node_id) {
    ROUTING__clear_seen_for_node(node_id);
    LB__clear_state_for_node(node_id);
}

void ROUTING__init_globals(IN routing_table_t *rt, IN routing_message_cb_t session_cb) {
    g_rt = rt;
    g_session_cb = session_cb;
    
    /* Initialize message ID counter from a small sequential value.
     * Previously using timestamp (time(NULL)) caused message IDs to look
     * like node IDs (e.g., 1776634649), which was confusing in logs.
     * Now we start from 1 for readable, sequential message IDs. */
    pthread_mutex_lock(&g_msg_id_mutex);
    g_msg_id_counter = 1;
    pthread_mutex_unlock(&g_msg_id_mutex);
}

static err_t ROUTING__send_to_node_id(IN uint32_t node_id, IN const protocol_msg_t *msg, IN const uint8_t *data) {
    err_t rc = E__SUCCESS;
    transport_t *t = NETWORK__get_transport(&g_network, node_id);
    protocol_msg_t fwd_msg;

    if (NULL == t) {
        LOG_WARNING("No transport for node %u", node_id);
        FAIL(E__ROUTING__NODE_NOT_FOUND);
    }
    
    fwd_msg = *msg;
    fwd_msg.src_node_id = (uint32_t)g_node_id;
    if (fwd_msg.ttl <= 1) {
        goto l_cleanup; // Depleted, drops passively
    }
    fwd_msg.ttl--;
    
    rc = TRANSPORT__send_msg(t, &fwd_msg, data);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    return rc;
}

err_t ROUTING__route_message(IN const protocol_msg_t *msg, IN const uint8_t *data, IN uint32_t exclude_node_id) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(msg);

    if (0 == msg->dst_node_id) {
        ROUTING__broadcast(g_rt, msg->src_node_id, (uint32_t)g_node_id, msg, data);
        goto l_cleanup;
    }

    rc = LB__route_message(g_rt, msg->dst_node_id, msg, data, exclude_node_id, ROUTING__send_to_node_id);
    if (E__SUCCESS != rc) {
        /* No route to destination - buffer message and trigger route discovery
         * (same logic as for received messages in ROUTING__on_message) */
        LOG_INFO("No route to %u, buffering message and initiating RREQ", msg->dst_node_id);
        ROUTING__buffer_message(msg->dst_node_id, msg, data);
        ROUTING__send_rreq(msg->dst_node_id);
        rc = E__ROUTING__NODE_NOT_FOUND;
    }

l_cleanup:
    return rc;
}

static void ROUTING__local_dispatch(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len) {
    msg_type_t type = (msg_type_t)msg->type;
    uint32_t src = msg->src_node_id;
    uint32_t orig_src = msg->orig_src_node_id;
    uint32_t dst = msg->dst_node_id;
    uint32_t ttl = msg->ttl;

    if (MSG__RREQ == type) {
        if (orig_src != (uint32_t)g_node_id && data_len >= sizeof(uint32_t)) {
            uint32_t target_node_id = ntohl(*(uint32_t *)data);
            LOG_TRACE("ROUTING: Forwarding RREQ for %u from %u", target_node_id, orig_src);
        }
        if (dst == 0) {
            ROUTING__broadcast(g_rt, src, (uint32_t)g_node_id, msg, data);
        }
    } 
    else if (MSG__RREP == type) {
        LOG_INFO("ROUTING: RREP arrived from %u traversing towards %u (via %u)", orig_src, dst, src);
        if (dst == (uint32_t)g_node_id) {
            uint8_t hop_count = (uint8_t)(DEFAULT_TTL > ttl ? (DEFAULT_TTL - ttl) : 1);
            ROUTING__add_via_hop(g_rt, orig_src, src, hop_count);
            /* Flush any pending messages now that we have a route to orig_src */
            ROUTING__flush_pending(orig_src);
            if (NULL != g_session_cb) {
                g_session_cb(t, msg, data, data_len);
            }
        } else {
            ROUTING__route_message(msg, data, src);
        }
    }
    else if (MSG__RERR == type) {
        if (NULL != data && data_len > 0) {
            size_t count = data_len / sizeof(uint32_t);
            for (size_t i = 0; i < count; i++) {
                uint32_t lost_node_id = ntohl(((uint32_t *)data)[i]);
                if (lost_node_id != (uint32_t)g_node_id) {
                    route_entry_t entry;
                    if (E__SUCCESS == ROUTING__get_route(g_rt, lost_node_id, &entry)) {
                        // RERR should only invalidate VIA_HOP routes. Direct routes are
                        // managed by the network layer's connect/disconnect events.
                        if (entry.route_type == ROUTE__VIA_HOP && entry.next_hop_node_id == src) {
                            LOG_INFO("ROUTING: Removing unreachable node %u reported by %u", lost_node_id, src);
                            ROUTING__remove(g_rt, lost_node_id);
                        }
                    }
                }
            }
        }
        if (dst == 0) {
            ROUTING__broadcast(g_rt, src, (uint32_t)g_node_id, msg, data);
        }
    } else {
        if (NULL != g_session_cb) {
            g_session_cb(t, msg, data, data_len);
        }
    }
}

void ROUTING__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    int seen = 0;
    uint32_t dst = 0;
    uint32_t src = 0;
    uint32_t orig_src = 0;
    uint32_t ttl = 0;
    msg_type_t type = MSG__NODE_INIT;

    if (NULL == g_rt || NULL == msg || NULL == t) {
        return;
    }

    dst = msg->dst_node_id;
    src = msg->src_node_id;
    orig_src = msg->orig_src_node_id;
    ttl = msg->ttl;
    type = (msg_type_t)msg->type;

    if (0 == ttl) {
        LOG_DEBUG("Dropping message with TTL 0 from node %u", src);
        return;
    }

    if (MSG__NODE_INIT == type) {
        if (orig_src != src) {
            rc = ROUTING__is_msg_seen(orig_src, msg->message_id, &seen);
            if (E__SUCCESS == rc && seen) {
                return;
            }
        }
        ROUTING__clear_state_for_node(orig_src);
        int dummy;
        ROUTING__is_msg_seen(orig_src, msg->message_id, &dummy);

        if (orig_src != (uint32_t)g_node_id && orig_src != src) {
            uint8_t hop_count = (uint8_t)(DEFAULT_TTL > ttl ? (DEFAULT_TTL - ttl) : 1);
            ROUTING__add_via_hop(g_rt, orig_src, src, hop_count);
        }
        if (NULL != g_session_cb) {
            g_session_cb(t, msg, data, data_len);
        }
        ROUTING__broadcast(g_rt, src, (uint32_t)g_node_id, msg, data);
        return;
    }

    if (orig_src == (uint32_t)g_node_id) {
        return;
    }

    /* Multipath discovery: the destination always replies on EVERY path a RREQ
     * arrives on, not just the first. Normal dedup would drop all but the first
     * copy before local_dispatch, so the requester would only ever learn one
     * route. By replying here (before dedup) and returning, each RREQ copy
     * triggers an independent RREP traveling back via its own intermediate hop. */
    if (MSG__RREQ == type && 0 == dst && data_len >= sizeof(uint32_t)) {
        uint32_t rreq_target = ntohl(*(uint32_t *)data);
        if (rreq_target == (uint32_t)g_node_id) {
            protocol_msg_t rrep;
            memset(&rrep, 0, sizeof(rrep));
            memcpy(rrep.magic, GANON_PROTOCOL_MAGIC, 4);
            rrep.orig_src_node_id = (uint32_t)g_node_id;
            rrep.src_node_id = (uint32_t)g_node_id;
            rrep.dst_node_id = orig_src;
            pthread_mutex_lock(&g_msg_id_mutex);
            rrep.message_id = ++g_msg_id_counter;
            pthread_mutex_unlock(&g_msg_id_mutex);
            rrep.type = MSG__RREP;
            rrep.data_length = 0;
            rrep.ttl = DEFAULT_TTL - 1;
            LOG_INFO("ROUTING: RREQ for us from %u via %u - sending RREP directly on arrival path", orig_src, src);
            TRANSPORT__send_msg(t, &rrep, NULL);
            ROUTING__is_msg_seen(orig_src, msg->message_id, &seen);
            return;
        }
    }

    /* Deduplication: Only drop duplicates for:
     * 1. Broadcast messages (dst == 0) - these flood the network
     * 2. Unicast messages destined for this node (we process them once)
     * 
     * For unicast messages NOT destined for us (we're just forwarding),
     * we should NOT drop duplicates because load balancing may send the same
     * message via multiple paths, and we need to forward each copy. */
    rc = ROUTING__is_msg_seen(orig_src, msg->message_id, &seen);
    if (E__SUCCESS == rc && seen) {
        if (0 == dst) {
            /* Broadcast: drop duplicate to prevent flooding */
            LOG_TRACE("ROUTING: Dropping broadcast duplicate from %u with ID %u (type %u)", orig_src, msg->message_id, type);
            return;
        } else if (dst == (uint32_t)g_node_id) {
            /* Unicast for us: drop duplicate, we already processed it */
            LOG_TRACE("ROUTING: Dropping unicast duplicate for us from %u with ID %u (type %u)", orig_src, msg->message_id, type);
            return;
        }
        /* Unicast not for us: continue forwarding even if "duplicate" - 
         * this is normal for multi-path load balancing */
    }

    if (orig_src != src) {
        uint8_t hop_count = (uint8_t)(DEFAULT_TTL > ttl ? (DEFAULT_TTL - ttl) : 1);
        ROUTING__add_via_hop(g_rt, orig_src, src, hop_count);
        LOG_TRACE("ROUTING: Learned reverse path to %u via %u (hops: %u)", orig_src, src, hop_count);
    } else {
        if (!ROUTING__is_direct(g_rt, src)) {
            LOG_WARNING("ROUTING: Auto-healing direct topology map for unmapped neighbor %u", src);
            ROUTING__add_direct(g_rt, src, t->fd);
            t->node_id = src;
        }
    }
    ROUTING__flush_pending(orig_src);

    if (dst == 0) {
        /* Broadcast: control messages only (RREQ/RREP/RERR). All go directly
         * to local dispatch without LB sequencing, since broadcasts are
         * inherently lossy (dedup may drop copies). */
        ROUTING__local_dispatch(t, msg, data, data_len);
    } else if (dst == (uint32_t)g_node_id) {
        if (MSG__RREQ == type || MSG__RREP == type || MSG__RERR == type) {
            /* Unicast control: no sequencing */
            ROUTING__local_dispatch(t, msg, data, data_len);
        } else if (MSG__TUNNEL_DATA == type) {
            /* Tunnel data: use LB reorder buffer per-connection for in-order delivery.
             * Each tunnel connection has its own channel_id for proper isolation. */
            LB__handle_incoming(t, msg, data, data_len, g_session_cb);
        } else {
            /* Unicast data (PING, PONG, USER_DATA): use LB reorder buffer for
             * in-order delivery. This is the main path that goes through LB. */
            LB__handle_incoming(t, msg, data, data_len, g_session_cb);
        }
    } else {
        route_entry_t entry;
        if (E__SUCCESS == ROUTING__get_route(g_rt, dst, &entry)) {
            ROUTING__route_message(msg, data, src);
        } else {
            LOG_INFO("No route to %u, buffering payload and initiating RREQ", dst);
            ROUTING__buffer_message(dst, msg, data);
            ROUTING__send_rreq(dst);
        }
    }
}

err_t ROUTING__init(OUT routing_table_t *rt) {
    err_t rc = E__SUCCESS;
    
    VALIDATE_ARGS(rt);
    
    memset(rt, 0, sizeof(routing_table_t));
    if (0 != pthread_mutex_init(&rt->mutex, NULL)) {
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
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id && rt->entries[i].next_hop_node_id == node_id) {
            rt->entries[i].route_type = ROUTE__DIRECT;
            rt->entries[i].fd = fd;
            rt->entries[i].hop_count = 1;
            rt->entries[i].last_updated = time(NULL);
            pthread_mutex_unlock(&rt->mutex);
            goto l_cleanup;
        }
    }

    if (rt->entry_count >= ROUTING_TABLE_MAX_ENTRIES) {
        pthread_mutex_unlock(&rt->mutex);
        FAIL(E__ROUTING__TABLE_FULL);
    }

    rt->entries[rt->entry_count].node_id = node_id;
    rt->entries[rt->entry_count].next_hop_node_id = node_id;
    rt->entries[rt->entry_count].route_type = ROUTE__DIRECT;
    rt->entries[rt->entry_count].fd = fd;
    rt->entries[rt->entry_count].hop_count = 1;
    rt->entries[rt->entry_count].last_updated = time(NULL);
    rt->entry_count++;

    pthread_mutex_unlock(&rt->mutex);
    
l_cleanup:
    return rc;
}

err_t ROUTING__add_via_hop(IN routing_table_t *rt, IN uint32_t node_id, IN uint32_t next_hop_node_id, IN uint8_t hop_count) {
    err_t rc = E__SUCCESS;
    
    VALIDATE_ARGS(rt);
    
    if (0 != pthread_mutex_lock(&rt->mutex)) {
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id && rt->entries[i].next_hop_node_id == next_hop_node_id) {
            if (rt->entries[i].route_type == ROUTE__VIA_HOP) {
                rt->entries[i].hop_count = hop_count;
                rt->entries[i].last_updated = time(NULL);
            }
            pthread_mutex_unlock(&rt->mutex);
            goto l_cleanup;
        }
    }

    if (rt->entry_count >= ROUTING_TABLE_MAX_ENTRIES) {
        pthread_mutex_unlock(&rt->mutex);
        FAIL(E__ROUTING__TABLE_FULL);
    }

    rt->entries[rt->entry_count].node_id = node_id;
    rt->entries[rt->entry_count].next_hop_node_id = next_hop_node_id;
    rt->entries[rt->entry_count].route_type = ROUTE__VIA_HOP;
    rt->entries[rt->entry_count].fd = -1;
    rt->entries[rt->entry_count].hop_count = hop_count;
    rt->entries[rt->entry_count].last_updated = time(NULL);
    rt->entry_count++;

    pthread_mutex_unlock(&rt->mutex);
    
l_cleanup:
    return rc;
}

err_t ROUTING__remove(IN routing_table_t *rt, IN uint32_t node_id) {
    err_t rc = E__SUCCESS;
    int found = 0;
    
    VALIDATE_ARGS(rt);
    
    if (0 != pthread_mutex_lock(&rt->mutex)) {
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            rt->entry_count--;
            if (i < rt->entry_count) {
                rt->entries[i] = rt->entries[rt->entry_count];
            }
            i--; // Re-check the swapped element
            found = 1;
        }
    }
    pthread_mutex_unlock(&rt->mutex);
    if (!found) rc = E__ROUTING__NODE_NOT_FOUND;
    
l_cleanup:
    return rc;
}

err_t ROUTING__remove_via_node(IN routing_table_t *rt, IN uint32_t via_node_id) {
    err_t rc = E__SUCCESS;
    
    VALIDATE_ARGS(rt);
    
    if (0 != pthread_mutex_lock(&rt->mutex)) {
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].route_type == ROUTE__VIA_HOP && rt->entries[i].next_hop_node_id == via_node_id) {
            rt->entry_count--;
            if (i < rt->entry_count) {
                rt->entries[i] = rt->entries[rt->entry_count];
            }
            i--;
        }
    }
    pthread_mutex_unlock(&rt->mutex);
    
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
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            if (rt->entries[i].route_type == ROUTE__VIA_HOP && time(NULL) - rt->entries[i].last_updated > 600) {
                continue;
            }
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

int ROUTING__has_route(IN uint32_t node_id) {
    route_entry_t entry;
    return (E__SUCCESS == ROUTING__get_route(g_rt, node_id, &entry));
}

err_t ROUTING__get_all_routes(IN routing_table_t *rt, IN uint32_t node_id, OUT route_entry_t *out_entries, IN size_t max_out, OUT size_t *out_count) {
    err_t rc = E__SUCCESS;
    
    VALIDATE_ARGS(rt, out_entries, out_count);
    *out_count = 0;
    
    if (0 != pthread_mutex_lock(&rt->mutex)) {
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        if (rt->entries[i].node_id == node_id) {
            if (rt->entries[i].route_type == ROUTE__VIA_HOP && time(NULL) - rt->entries[i].last_updated > 600) {
                continue;
            }
            if (*out_count < max_out) {
                out_entries[*out_count] = rt->entries[i];
                (*out_count)++;
            }
        }
    }
    pthread_mutex_unlock(&rt->mutex);
    if (0 == *out_count) rc = E__ROUTING__NODE_NOT_FOUND;
    
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

err_t ROUTING__send_rreq(IN uint32_t target_node_id) {
    err_t rc = E__SUCCESS;
    protocol_msg_t rreq_msg;
    uint32_t target;
    int seen = 0;

    memset(&rreq_msg, 0, sizeof(rreq_msg));
    memcpy(rreq_msg.magic, GANON_PROTOCOL_MAGIC, 4);
    rreq_msg.orig_src_node_id = (uint32_t)g_node_id;
    rreq_msg.src_node_id = (uint32_t)g_node_id;
    rreq_msg.dst_node_id = 0;
    
    pthread_mutex_lock(&g_msg_id_mutex);
    rreq_msg.message_id = ++g_msg_id_counter;
    pthread_mutex_unlock(&g_msg_id_mutex);
    
    rreq_msg.type = MSG__RREQ;
    rreq_msg.data_length = sizeof(uint32_t);
    rreq_msg.ttl = DEFAULT_TTL;

    target = htonl(target_node_id);

    ROUTING__is_msg_seen((uint32_t)g_node_id, rreq_msg.message_id, &seen); 
    ROUTING__broadcast(g_rt, 0, (uint32_t)g_node_id, &rreq_msg, (const uint8_t *)&target);

    return rc;
}

void ROUTING__rediscover_active_routes(IN routing_table_t *rt) {
    if (NULL == rt) return;
    
    time_t now = time(NULL);
    pthread_mutex_lock(&rt->mutex);
    
    // Create a copy of targets to avoid holding lock while broadcasting
    uint32_t active_targets[ROUTING_TABLE_MAX_ENTRIES];
    size_t active_count = 0;
    
    for (size_t i = 0; i < rt->entry_count; i++) {
        // Only rediscover VIA_HOP routes that were updated in the last 5 minutes
        if (rt->entries[i].route_type == ROUTE__VIA_HOP && (now - rt->entries[i].last_updated < 300)) {
            // Avoid duplicates in the list
            int found = 0;
            for (size_t j = 0; j < active_count; j++) {
                if (active_targets[j] == rt->entries[i].node_id) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                active_targets[active_count++] = rt->entries[i].node_id;
            }
        }
    }
    pthread_mutex_unlock(&rt->mutex);
    
    if (active_count > 0) {
        LOG_DEBUG("ROUTING: Triggering proactive rediscovery for %zu active routes", active_count);
        for (size_t i = 0; i < active_count; i++) {
            ROUTING__send_rreq(active_targets[i]);
        }
    }
}

err_t ROUTING__send_to_node(IN routing_table_t *rt, IN uint32_t node_id, IN const uint8_t *buf, IN size_t len, IN ssize_t (*send_fn)(IN int, IN const uint8_t *, IN size_t)) {
    err_t rc = E__SUCCESS;
    route_entry_t entry;
    
    VALIDATE_ARGS(rt, buf);
    
    rc = ROUTING__get_route(rt, node_id, &entry);
    if (E__SUCCESS != rc) {
        FAIL(rc);
    }
    
    if (entry.route_type == ROUTE__DIRECT) {
        ssize_t sent = send_fn(entry.fd, buf, len);
        if (0 > sent) {
            FAIL(E__NET__SEND_FAILED);
        }
    } else {
        rc = ROUTING__send_to_node(rt, entry.next_hop_node_id, buf, len, send_fn);
    }
    
l_cleanup:
    return rc;
}

void ROUTING__broadcast(IN routing_table_t *rt, IN uint32_t exclude_node_id, IN uint32_t src_node_id, IN const protocol_msg_t *msg, IN const uint8_t *data) {
    if (NULL == rt || NULL == msg) {
        return;
    }
    if (msg->ttl <= 1) {
        return; // Depleted TTL, don't forward over physical wire
    }
    if (0 != pthread_mutex_lock(&rt->mutex)) {
        return;
    }

    for (size_t i = 0; i < rt->entry_count; i++) {
        uint32_t node_id = rt->entries[i].node_id;
        if (node_id != exclude_node_id && rt->entries[i].route_type == ROUTE__DIRECT) {
            transport_t *t = NETWORK__get_transport(&g_network, node_id);
            if (NULL != t) {
                protocol_msg_t broadcast_msg = *msg;
                broadcast_msg.src_node_id = src_node_id;
                broadcast_msg.ttl--;
                TRANSPORT__send_msg(t, &broadcast_msg, data);
            }
        }
    }
    pthread_mutex_unlock(&rt->mutex);
}

void ROUTING__handle_disconnect(IN uint32_t node_id) {
    err_t rc = E__SUCCESS;
    size_t via_count = 0;
    uint32_t *via_nodes = NULL;
    protocol_msg_t msg;
    uint8_t *data = NULL;
    
    via_nodes = ROUTING__get_via_nodes(g_rt, node_id, &via_count);

    ROUTING__remove(g_rt, node_id);
    ROUTING__remove_via_node(g_rt, node_id);

    memset(&msg, 0, sizeof(msg));
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)g_node_id;
    msg.src_node_id = (uint32_t)g_node_id;
    msg.dst_node_id = 0;
    pthread_mutex_lock(&g_msg_id_mutex);
    msg.message_id = ++g_msg_id_counter;
    pthread_mutex_unlock(&g_msg_id_mutex);
    msg.type = MSG__RERR;
    msg.data_length = (uint32_t)(via_count * sizeof(uint32_t));
    msg.ttl = DEFAULT_TTL;

    if (via_count > 0 && NULL != via_nodes) {
        uint32_t *truly_lost = malloc(via_count * sizeof(uint32_t));
        size_t truly_lost_count = 0;
        
        if (NULL != truly_lost) {
            for (size_t i = 0; i < via_count; i++) {
                route_entry_t entry;
                if (E__SUCCESS != ROUTING__get_route(g_rt, via_nodes[i], &entry)) {
                    truly_lost[truly_lost_count++] = htonl(via_nodes[i]);
                }
            }
        }
        
        if (truly_lost_count > 0) {
            msg.data_length = (uint32_t)(truly_lost_count * sizeof(uint32_t));
            ROUTING__broadcast(g_rt, node_id, (uint32_t)g_node_id, &msg, (uint8_t *)truly_lost);
        }
        FREE(truly_lost);
    } else {
        // Just report the neighbor itself as lost
        uint32_t lost = htonl(node_id);
        msg.data_length = sizeof(uint32_t);
        ROUTING__broadcast(g_rt, node_id, (uint32_t)g_node_id, &msg, (uint8_t *)&lost);
    }

    FREE(data);
    FREE(via_nodes);
    goto l_cleanup;
l_cleanup:
    (void)rc;
}