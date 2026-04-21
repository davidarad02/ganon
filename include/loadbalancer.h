#ifndef GANON_LOADBALANCER_H
#define GANON_LOADBALANCER_H

#include "protocol.h"
#include "routing.h"
#include "transport.h"

typedef enum {
    LB_STRATEGY_ROUND_ROBIN = 0,
    LB_STRATEGY_ALL_ROUTES,
    LB_STRATEGY_STICKY
} lb_strategy_t;

/* Initialize global load balancer state
 * reorder_enabled: 0 = process packets immediately (no buffering), 1 = enable reordering/buffering */
void LB__init(lb_strategy_t strategy, int reorder_timeout_ms, int rr_count, int reorder_enabled);

/* Select routes for destination and dispatch via the provided callback */
err_t LB__route_message(IN routing_table_t *rt, IN uint32_t dst, IN const protocol_msg_t *msg, IN const uint8_t *data, IN uint32_t exclude_node_id, IN err_t (*send_fn)(uint32_t, const protocol_msg_t *, const uint8_t *));

/* Handle incoming packets for the local node to deduplicate and reorder */
void LB__handle_incoming(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len, IN routing_message_cb_t session_cb);

void LB__destroy(void);
void LB__clear_state_for_node(uint32_t node_id);

#endif /* GANON_LOADBALANCER_H */
