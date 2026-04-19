#ifndef GANON_ROUTING_H
#define GANON_ROUTING_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#include "common.h"
#include "err.h"
#include "protocol.h"
#include "transport.h"

#define ROUTING_TABLE_MAX_ENTRIES 256

typedef enum {
    ROUTE__DIRECT = 0,
    ROUTE__VIA_HOP,
} route_type_t;

typedef struct route_entry {
    uint32_t node_id;
    uint32_t next_hop_node_id;
    route_type_t route_type;
    int fd;
    time_t last_updated;
    uint8_t hop_count;
} route_entry_t;

typedef struct {
    route_entry_t entries[ROUTING_TABLE_MAX_ENTRIES];
    size_t entry_count;
    pthread_mutex_t mutex;
} routing_table_t;

err_t ROUTING__init(OUT routing_table_t *rt);
void ROUTING__destroy(IN routing_table_t *rt);

err_t ROUTING__add_direct(IN routing_table_t *rt, IN uint32_t node_id, IN int fd);
err_t ROUTING__add_via_hop(IN routing_table_t *rt, IN uint32_t node_id, IN uint32_t next_hop_node_id, IN uint8_t hop_count);
err_t ROUTING__remove(IN routing_table_t *rt, IN uint32_t node_id);
err_t ROUTING__remove_via_node(IN routing_table_t *rt, IN uint32_t via_node_id);
uint32_t *ROUTING__get_via_nodes(IN routing_table_t *rt, IN uint32_t via_node_id, OUT size_t *count);
err_t ROUTING__get_route(IN routing_table_t *rt, IN uint32_t node_id, OUT route_entry_t *entry);
err_t ROUTING__get_all_routes(IN routing_table_t *rt, IN uint32_t node_id, OUT route_entry_t *out_entries, IN size_t max_out, OUT size_t *out_count);
err_t ROUTING__get_next_hop(IN routing_table_t *rt, IN uint32_t node_id, OUT uint32_t *next_hop);
int ROUTING__is_direct(IN routing_table_t *rt, IN uint32_t node_id);
err_t ROUTING__send_to_node(IN routing_table_t *rt, IN uint32_t node_id, IN const uint8_t *buf, IN size_t len, IN ssize_t (*send_fn)(IN int, IN const uint8_t *, IN size_t));

err_t ROUTING__route_message(IN const protocol_msg_t *msg, IN const uint8_t *data, IN uint32_t exclude_node_id);
err_t ROUTING__send_rreq(IN uint32_t target_node_id);
int ROUTING__has_route(IN uint32_t node_id);
void ROUTING__rediscover_active_routes(IN routing_table_t *rt);

void ROUTING__handle_disconnect(IN uint32_t node_id);
typedef void (*routing_message_cb_t)(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len);
void ROUTING__init_globals(IN routing_table_t *rt, IN routing_message_cb_t session_cb);
void ROUTING__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len);
void ROUTING__broadcast(IN routing_table_t *rt, IN uint32_t exclude_node_id, IN uint32_t src_node_id, IN const protocol_msg_t *msg, IN const uint8_t *data);

extern int g_node_id;

#endif /* #ifndef GANON_ROUTING_H */
