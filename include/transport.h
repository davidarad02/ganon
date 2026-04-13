#ifndef GANON_TRANSPORT_H
#define GANON_TRANSPORT_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "err.h"
#include "protocol.h"

typedef struct transport transport_t;

struct transport {
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    uint32_t node_id;
    void *ctx;
    ssize_t (*recv)(int fd, uint8_t *buf, size_t len);
    ssize_t (*send)(int fd, const uint8_t *buf, size_t len);
};

ssize_t TRANSPORT__recv(int fd, uint8_t *buf, size_t len);
ssize_t TRANSPORT__send(int fd, const uint8_t *buf, size_t len);

transport_t *TRANSPORT__create(int fd);
void TRANSPORT__destroy(transport_t *t);

err_t TRANSPORT__recv_all(transport_t *t, uint8_t *buf, size_t len, ssize_t *bytes_read);
err_t TRANSPORT__send_all(transport_t *t, const uint8_t *buf, size_t len, ssize_t *bytes_sent);

err_t TRANSPORT__recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data);
err_t TRANSPORT__send_msg(transport_t *t, const protocol_msg_t *msg, const uint8_t *data);

int TRANSPORT__get_fd(transport_t *t);
uint32_t TRANSPORT__get_node_id(transport_t *t);
void TRANSPORT__set_node_id(transport_t *t, uint32_t node_id);

#endif /* #ifndef GANON_TRANSPORT_H */
