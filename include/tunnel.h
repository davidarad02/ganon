#ifndef GANON_TUNNEL_H
#define GANON_TUNNEL_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "protocol.h"
#include "transport.h"

void TUNNEL__init(IN int tcp_rcvbuf);
void TUNNEL__destroy(void);
extern int g_tunnel_tcp_rcvbuf;  /* TCP receive buffer size in bytes */
void TUNNEL__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len);
void TUNNEL__handle_disconnect(IN uint32_t node_id);

#endif /* GANON_TUNNEL_H */
