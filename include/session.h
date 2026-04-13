#ifndef GANON_SESSION_H
#define GANON_SESSION_H

#include "protocol.h"
#include "routing.h"
#include "transport.h"

err_t SESSION__process(routing_table_t *rt, int fd, transport_t *t, uint32_t *peer_node_id, uint8_t *out_header, size_t header_len, uint32_t **out_peer_list, size_t *out_peer_count, uint8_t **out_data, size_t *out_data_len);

err_t SESSION__send_packet(transport_t *t, uint32_t src_node_id, uint32_t dst_node_id, msg_type_t type, const uint8_t *data, size_t data_len);

#endif /* #ifndef GANON_SESSION_H */
