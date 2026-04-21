#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "loadbalancer.h"
#include "logging.h"
#include "common.h"

static lb_strategy_t g_strategy = LB_STRATEGY_ROUND_ROBIN;
static int g_reorder_timeout_ms = 100;
static int g_rr_count = 1;
static int g_reorder_enabled = 0;  /* 0 = disabled (process immediately), 1 = enabled */

#define MAX_LB_NODES 256
typedef struct {
    uint32_t node_id;
    size_t last_route_idx;
} lb_rr_state_t;

static lb_rr_state_t g_rr_state[MAX_LB_NODES] = {0};
static pthread_mutex_t g_lb_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_reorder_thread;
static int g_lb_running = 0;
static routing_message_cb_t g_lb_session_cb = NULL;
// Removed g_lb_transport global hack as it's now per-entry.

static void check_reorder_buffer(uint32_t orig_src, routing_message_cb_t session_cb);

#define REORDER_BUFFER_SIZE 256
typedef struct {
    uint32_t orig_src;
    uint32_t msg_id;
    protocol_msg_t msg;
    uint8_t *data;
    struct timespec arrival_time;
    transport_t *t;
    int is_valid;
} reorder_entry_t;

static reorder_entry_t g_reorder_buf[REORDER_BUFFER_SIZE] = {0};
static uint32_t g_expected_msg_id[MAX_LB_NODES] = {0};
static uint32_t g_reorder_orig_src[MAX_LB_NODES] = {0};

static void* reorder_thread_func(void* arg) {
    (void)arg;
    while (g_lb_running) {
        pthread_mutex_lock(&g_lb_mutex);
        uint32_t sources[MAX_LB_NODES];
        size_t source_count = 0;
        for (int i=0; i<MAX_LB_NODES; i++) {
            if (g_reorder_orig_src[i] != 0) sources[source_count++] = g_reorder_orig_src[i];
        }
        
        for (size_t i=0; i<source_count; i++) {
            check_reorder_buffer(sources[i], g_lb_session_cb);
        }
        pthread_mutex_unlock(&g_lb_mutex);
        
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (g_reorder_timeout_ms / 2) * 1000000;
        if (ts.tv_nsec == 0) ts.tv_nsec = 1000000;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void LB__init(lb_strategy_t strategy, int reorder_timeout_ms, int rr_count, int reorder_enabled) {
    g_strategy = strategy;
    g_reorder_timeout_ms = reorder_timeout_ms;
    g_rr_count = (rr_count >= 1) ? rr_count : 1;
    g_reorder_enabled = reorder_enabled;
    g_lb_running = 1;
    memset(g_rr_state, 0, sizeof(g_rr_state));
    memset(g_reorder_buf, 0, sizeof(g_reorder_buf));
    memset(g_expected_msg_id, 0, sizeof(g_expected_msg_id));
    memset(g_reorder_orig_src, 0, sizeof(g_reorder_orig_src));
    
    if (g_reorder_enabled) {
        pthread_create(&g_reorder_thread, NULL, reorder_thread_func, NULL);
        LOG_INFO("Load balancer initialized with reordering ENABLED (timeout=%dms, strategy=%d)", 
                 reorder_timeout_ms, strategy);
    } else {
        LOG_INFO("Load balancer initialized with reordering DISABLED (strategy=%d)", strategy);
    }
}

err_t LB__route_message(IN routing_table_t *rt, IN uint32_t dst, IN const protocol_msg_t *msg, IN const uint8_t *data, IN uint32_t exclude_node_id, IN err_t (*send_fn)(uint32_t, const protocol_msg_t *, const uint8_t *)) {
    err_t rc = E__SUCCESS;
    route_entry_t all_routes[16];
    route_entry_t routes[16];
    size_t all_count = 0;
    size_t count = 0;
    
    rc = ROUTING__get_all_routes(rt, dst, all_routes, 16, &all_count);
    if (E__SUCCESS != rc || all_count == 0) {
        return E__ROUTING__NODE_NOT_FOUND;
    }

    for (size_t i = 0; i < all_count; i++) {
        if (all_routes[i].next_hop_node_id != exclude_node_id) {
            routes[count++] = all_routes[i];
        }
    }

    if (count == 0) {
        return E__ROUTING__NODE_NOT_FOUND;
    }

    if (LB_STRATEGY_ALL_ROUTES == g_strategy) {
        for (size_t i = 0; i < count; i++) {
            err_t send_rc = send_fn(routes[i].next_hop_node_id, msg, data);
            if (E__SUCCESS != send_rc) rc = send_rc;
        }
    } else if (LB_STRATEGY_STICKY == g_strategy && 0 != msg->channel_id) {
        /* Sticky mode, non-zero channel: channel N always uses route (N-1) % count. */
        size_t route_idx = (size_t)((msg->channel_id - 1) % count);
        LOG_DEBUG("LB: Channel %u pinned to route %zu/%zu (next hop %u)", msg->channel_id, route_idx + 1, count, routes[route_idx].next_hop_node_id);
        rc = send_fn(routes[route_idx].next_hop_node_id, msg, data);
    } else {
        /* Round-robin (default for all strategies, and for channel_id=0 in sticky mode):
         * send to g_rr_count consecutive routes, sliding the window by 1 each call. */
        pthread_mutex_lock(&g_lb_mutex);
        size_t first_idx = 0;
        int found = 0;
        for (int i=0; i<MAX_LB_NODES; i++) {
            if (g_rr_state[i].node_id == dst) {
                first_idx = g_rr_state[i].last_route_idx;
                g_rr_state[i].last_route_idx = (first_idx + 1) % count;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (int i=0; i<MAX_LB_NODES; i++) {
                if (g_rr_state[i].node_id == 0) {
                    g_rr_state[i].node_id = dst;
                    g_rr_state[i].last_route_idx = 1 % count;
                    first_idx = 0;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_lb_mutex);

        size_t send_count = (size_t)g_rr_count < count ? (size_t)g_rr_count : count;
        for (size_t j = 0; j < send_count; j++) {
            size_t route_idx = (first_idx + j) % count;
            LOG_DEBUG("LB: Round-robin node %u [%zu/%zu] route %zu (next hop %u)", dst, j + 1, send_count, route_idx + 1, routes[route_idx].next_hop_node_id);
            err_t send_rc = send_fn(routes[route_idx].next_hop_node_id, msg, data);
            if (E__SUCCESS != send_rc) rc = send_rc;
        }
    }
    return rc;
}

static uint32_t* get_expected_msg_id_ptr(uint32_t orig_src) {
    for (int i=0; i<MAX_LB_NODES; i++) {
        if (g_reorder_orig_src[i] == orig_src) return &g_expected_msg_id[i];
    }
    for (int i=0; i<MAX_LB_NODES; i++) {
        if (g_reorder_orig_src[i] == 0) {
            g_reorder_orig_src[i] = orig_src;
            g_expected_msg_id[i] = 0;
            return &g_expected_msg_id[i];
        }
    }
    return NULL;
}

static void check_reorder_buffer(uint32_t orig_src, routing_message_cb_t session_cb) {
    uint32_t *expected_ptr = get_expected_msg_id_ptr(orig_src);
    if (!expected_ptr) return;
    
    int advanced = 1;
    int sticky_timeout = 0;
    while (advanced) {
        advanced = 0;
        uint32_t lowest_msg_id = 0xFFFFFFFF;
        int lowest_idx = -1;
        int current_timeout = 0;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // Find the oldest/lowest msg_id and check if anything timed out
        for (int i=0; i<REORDER_BUFFER_SIZE; i++) {
            if (g_reorder_buf[i].is_valid && g_reorder_buf[i].orig_src == orig_src) {
                if (g_reorder_buf[i].msg_id < lowest_msg_id) {
                    lowest_msg_id = g_reorder_buf[i].msg_id;
                    lowest_idx = i;
                }
                
                long elapsed_ms = (now.tv_sec - g_reorder_buf[i].arrival_time.tv_sec) * 1000 + 
                                 (now.tv_nsec - g_reorder_buf[i].arrival_time.tv_nsec) / 1000000;
                if (elapsed_ms > g_reorder_timeout_ms) {
                    current_timeout = 1;
                }
            }
        }
        
        if (current_timeout) sticky_timeout = 1;

        if (lowest_idx != -1) {
            if ((*expected_ptr - lowest_msg_id) < 0x7FFFFFFF) {
                // Drop late packet
                int i = lowest_idx;
                FREE(g_reorder_buf[i].data);
                g_reorder_buf[i].is_valid = 0;
                advanced = 1;
            } else if (lowest_msg_id == *expected_ptr || sticky_timeout) {
                // Deliver the lowest msg_id.
                // IMPORTANT: session_cb can call LB__route_message which tries to acquire
                // g_lb_mutex. Release the lock before calling it to prevent deadlock,
                // then re-acquire to continue the flush loop.
                int i = lowest_idx;
                transport_t *del_t = g_reorder_buf[i].t;
                protocol_msg_t del_msg = g_reorder_buf[i].msg;
                uint8_t *del_data = g_reorder_buf[i].data;
                *expected_ptr = g_reorder_buf[i].msg_id + 1;
                g_reorder_buf[i].data = NULL;
                g_reorder_buf[i].is_valid = 0;
                advanced = 1;

                if (sticky_timeout) {
                    LOG_DEBUG("Reorder buffer timeout/flush for orig_src %u msg_id %u", orig_src, lowest_msg_id);
                }

                pthread_mutex_unlock(&g_lb_mutex);
                if (session_cb) session_cb(del_t, &del_msg, del_data, del_msg.data_length);
                FREE(del_data);
                pthread_mutex_lock(&g_lb_mutex);

                expected_ptr = get_expected_msg_id_ptr(orig_src);
                if (!expected_ptr) return;
            }
        }
    }
}

void LB__handle_incoming(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len, IN routing_message_cb_t session_cb) {
    /* If reordering is disabled, process packet immediately without buffering */
    if (!g_reorder_enabled) {
        if (session_cb) session_cb(t, msg, data, data_len);
        return;
    }
    
    if (msg->message_id == 0) {
        if (session_cb) session_cb(t, msg, data, data_len);
        return;
    }

    pthread_mutex_lock(&g_lb_mutex);
    uint32_t orig_src = msg->orig_src_node_id;
    uint32_t msg_id = msg->message_id;
    
    // Store for background thread
    g_lb_session_cb = session_cb;
    
    uint32_t *expected_ptr = get_expected_msg_id_ptr(orig_src);
    if (!expected_ptr) {
        pthread_mutex_unlock(&g_lb_mutex);
        if (session_cb) session_cb(t, msg, data, data_len);
        return;
    }
    
    if (*expected_ptr == 0) {
        *expected_ptr = msg_id;
    }

    if (msg_id == *expected_ptr) {
        *expected_ptr = msg_id + 1;
        pthread_mutex_unlock(&g_lb_mutex);
        if (session_cb) session_cb(t, msg, data, data_len);
        
        pthread_mutex_lock(&g_lb_mutex);
        check_reorder_buffer(orig_src, session_cb);
        pthread_mutex_unlock(&g_lb_mutex);
        return;
    } else if (*expected_ptr - msg_id < 0x7FFFFFFF) {
        LOG_DEBUG("Dropped late/duplicate msg_id %u from %u (expected %u)", msg_id, orig_src, *expected_ptr);
        pthread_mutex_unlock(&g_lb_mutex);
        return;
    } else {
        LOG_DEBUG("Buffering future msg_id %u from %u (expected %u)", msg_id, orig_src, *expected_ptr);
        int buffered = 0;
        for (int i=0; i<REORDER_BUFFER_SIZE; i++) {
            if (!g_reorder_buf[i].is_valid) {
                g_reorder_buf[i].orig_src = orig_src;
                g_reorder_buf[i].msg_id = msg_id;
                g_reorder_buf[i].msg = *msg;
                if (data_len > 0 && data) {
                    g_reorder_buf[i].data = malloc(data_len);
                    if (g_reorder_buf[i].data) memcpy(g_reorder_buf[i].data, data, data_len);
                } else {
                    g_reorder_buf[i].data = NULL;
                }
                g_reorder_buf[i].t = t;
                clock_gettime(CLOCK_MONOTONIC, &g_reorder_buf[i].arrival_time);
                g_reorder_buf[i].is_valid = 1;
                buffered = 1;
                break;
            }
        }
        if (!buffered) {
            LOG_WARNING("LB: Reorder buffer full for node %u, message %u dropped", orig_src, msg_id);
        }
        check_reorder_buffer(orig_src, session_cb);
        pthread_mutex_unlock(&g_lb_mutex);
    }
}

void LB__clear_state_for_node(uint32_t node_id) {
    pthread_mutex_lock(&g_lb_mutex);
    for (int i=0; i<MAX_LB_NODES; i++) {
        if (g_reorder_orig_src[i] == node_id) {
            g_expected_msg_id[i] = 0;
            // Also clear buffer entries for this source
            for (int buf_idx=0; buf_idx<REORDER_BUFFER_SIZE; buf_idx++) {
                if (g_reorder_buf[buf_idx].is_valid && g_reorder_buf[buf_idx].orig_src == node_id) {
                    FREE(g_reorder_buf[buf_idx].data);
                    g_reorder_buf[buf_idx].is_valid = 0;
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_lb_mutex);
}

void LB__destroy(void) {
    g_lb_running = 0;
    pthread_join(g_reorder_thread, NULL);
    for (int i=0; i<REORDER_BUFFER_SIZE; i++) {
        if (g_reorder_buf[i].is_valid) FREE(g_reorder_buf[i].data);
        g_reorder_buf[i].is_valid = 0;
    }
}
