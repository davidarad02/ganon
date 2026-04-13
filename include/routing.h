#ifndef GANON_ROUTING_H
#define GANON_ROUTING_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>

#include "err.h"

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
} route_entry_t;

typedef struct {
    route_entry_t entries[ROUTING_TABLE_MAX_ENTRIES];
    size_t entry_count;
    pthread_mutex_t mutex;
} routing_table_t;

err_t ROUTING__init(routing_table_t *rt);
void ROUTING__destroy(routing_table_t *rt);

err_t ROUTING__add_direct(routing_table_t *rt, uint32_t node_id, int fd);
err_t ROUTING__add_via_hop(routing_table_t *rt, uint32_t node_id, uint32_t next_hop_node_id);
err_t ROUTING__remove(routing_table_t *rt, uint32_t node_id);
err_t ROUTING__remove_via_node(routing_table_t *rt, uint32_t via_node_id);
err_t ROUTING__get_route(routing_table_t *rt, uint32_t node_id, route_entry_t *entry);
err_t ROUTING__get_next_hop(routing_table_t *rt, uint32_t node_id, uint32_t *next_hop);
int ROUTING__is_direct(routing_table_t *rt, uint32_t node_id);
err_t ROUTING__send_to_node(routing_table_t *rt, uint32_t node_id, const uint8_t *buf, size_t len, ssize_t (*send_fn)(int, const uint8_t *, size_t));

#endif /* #ifndef GANON_ROUTING_H */
