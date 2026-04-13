#ifndef GANON_PROTOCOL_H
#define GANON_PROTOCOL_H

#include <stdint.h>

#define GANON_PROTOCOL_MAGIC "GNN\0"

typedef enum {
    E__MSG__FIRST = 0,

    /* keep last */
    E__MSG__LAST,
} msg_type_t;

typedef struct {
    char magic[4];
    uint32_t node_id;
    uint32_t message_id;
    msg_type_t type;
    uint32_t data_length;
    uint8_t data[];
} protocol_msg_t;

#endif /* #ifndef GANON_PROTOCOL_H */
