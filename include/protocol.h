#ifndef GANON_PROTOCOL_H
#define GANON_PROTOCOL_H

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GANON_PROTOCOL_MAGIC "GNN\0"
#define PROTOCOL_HEADER_SIZE sizeof(protocol_msg_t)
#define DEFAULT_TTL 16

#define PROTOCOL_FIELD_TO_NETWORK(x) htonl(x)
#define PROTOCOL_FIELD_FROM_NETWORK(x) ntohl(x)

typedef enum {
    MSG__NODE_INIT = 0,
    MSG__PEER_INFO = 1,
    MSG__NODE_DISCONNECT = 2,
    MSG__CONNECTION_REJECTED = 3,
} msg_type_t;

typedef struct {
    uint8_t magic[4];
    uint32_t orig_src_node_id;
    uint32_t src_node_id;
    uint32_t dst_node_id;
    uint32_t message_id;
    uint32_t type;
    uint32_t data_length;
    uint32_t ttl;
} protocol_msg_t;

bool PROTOCOL__validate_magic(const uint8_t *buf);

err_t PROTOCOL__parse_header(const uint8_t *buf, protocol_msg_t *msg);
err_t PROTOCOL__serialize(const protocol_msg_t *msg, uint8_t *buf, size_t buf_len, size_t *bytes_written);

void PROTOCOL__msg_ntoh(protocol_msg_t *msg);
void PROTOCOL__msg_hton(protocol_msg_t *msg);

#endif /* #ifndef GANON_PROTOCOL_H */
