#ifndef GANON_SESSION_H
#define GANON_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
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

err_t SESSION__init(OUT session_t *s, IN int node_id);
void SESSION__destroy(IN session_t *s);

void SESSION__set_network(IN session_t *s, IN network_t *net);
network_t *SESSION__get_network(IN session_t *s);
session_t *SESSION__get_session(void);
uint32_t SESSION__get_next_msg_id(void);

void SESSION__on_connected(IN transport_t *t);
void SESSION__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len);
void SESSION__on_disconnected(IN transport_t *t);

int SESSION__get_node_id(IN session_t *s);
routing_table_t *SESSION__get_routing_table(IN session_t *s);

/* File chunk size for chunked uploads/downloads (0 = use default 256KB) */
extern int g_session_file_chunk_size;
void SESSION__set_file_chunk_size(int chunk_size);

#endif /* #ifndef GANON_SESSION_H */
