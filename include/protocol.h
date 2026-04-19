#ifndef GANON_PROTOCOL_H
#define GANON_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "err.h"

#define GANON_PROTOCOL_MAGIC "GNN\0"
#define PROTOCOL_HEADER_SIZE sizeof(protocol_msg_t)
#define DEFAULT_TTL 16

typedef enum {
    MSG__NODE_INIT = 0,
    MSG__CONNECTION_REJECTED = 1,
    MSG__RREQ = 2,
    MSG__RREP = 3,
    MSG__RERR = 4,
    MSG__USER_DATA = 5,
    MSG__PING = 6,
    MSG__PONG = 7,
    MSG__TUNNEL_OPEN = 8,
    MSG__TUNNEL_CONN_OPEN = 9,
    MSG__TUNNEL_CONN_ACK = 10,
    MSG__TUNNEL_DATA = 11,
    MSG__TUNNEL_CONN_CLOSE = 12,
    MSG__TUNNEL_CLOSE = 13,
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
    uint32_t channel_id;
} protocol_msg_t;

bool PROTOCOL__validate_magic(IN const uint8_t *buf);

err_t PROTOCOL__unserialize(IN const uint8_t *buf, IN size_t len, OUT protocol_msg_t *msg, OUT uint8_t **data, OUT size_t *data_len);
err_t PROTOCOL__serialize(IN const protocol_msg_t *msg, IN const uint8_t *data, OUT uint8_t *buf, IN size_t buf_len, OUT size_t *bytes_written);

#endif /* #ifndef GANON_PROTOCOL_H */
