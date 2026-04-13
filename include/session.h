#ifndef GANON_SESSION_H
#define GANON_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "routing.h"
#include "transport.h"

typedef struct network_t network_t;

typedef struct session_t session_t;

struct session_t {
    int node_id;
    routing_table_t routing_table;
    network_t *net;
};

err_t SESSION__init(session_t *s, int node_id);
void SESSION__destroy(session_t *s);

void SESSION__set_network(session_t *s, network_t *net);
network_t *SESSION__get_network(session_t *s);
session_t *SESSION__get_session(void);

void SESSION__on_connected(transport_t *t);
void SESSION__on_message(transport_t *t, const protocol_msg_t *msg, const uint8_t *data, size_t data_len);
void SESSION__on_disconnected(transport_t *t);

int SESSION__get_node_id(session_t *s);
routing_table_t *SESSION__get_routing_table(session_t *s);

#endif /* #ifndef GANON_SESSION_H */
