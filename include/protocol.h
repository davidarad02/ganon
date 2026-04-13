#ifndef GANON_PROTOCOL_H
#define GANON_PROTOCOL_H

#include <stdint.h>
#include <arpa/inet.h>

#define GANON_PROTOCOL_MAGIC "GNN\0"

#define PROTOCOL_FIELD_TO_NETWORK(x) htonl(x)
#define PROTOCOL_FIELD_FROM_NETWORK(x) ntohl(x)

typedef enum {
    MSG__PING = 0,
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
