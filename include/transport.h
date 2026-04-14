#ifndef GANON_TRANSPORT_H
#define GANON_TRANSPORT_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "common.h"
#include "err.h"
#include "protocol.h"

typedef struct network_t network_t;
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

ssize_t TRANSPORT__recv(IN int fd, OUT uint8_t *buf, IN size_t len);
ssize_t TRANSPORT__send(IN int fd, IN const uint8_t *buf, IN size_t len);

transport_t *TRANSPORT__create(IN int fd);
void TRANSPORT__destroy(IN transport_t *t);

err_t TRANSPORT__recv_all(IN transport_t *t, OUT uint8_t *buf, IN size_t len, OUT ssize_t *bytes_read);
err_t TRANSPORT__send_all(IN transport_t *t, IN const uint8_t *buf, IN size_t len, OUT ssize_t *bytes_sent);

err_t TRANSPORT__recv_msg(IN transport_t *t, OUT protocol_msg_t *msg, OUT uint8_t **data);
err_t TRANSPORT__send_msg(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data);

int TRANSPORT__get_fd(IN transport_t *t);
uint32_t TRANSPORT__get_node_id(IN transport_t *t);
void TRANSPORT__set_node_id(IN transport_t *t, IN uint32_t node_id);

err_t TRANSPORT__send_to_node_id(IN network_t *net, IN uint32_t node_id, IN const protocol_msg_t *msg, IN const uint8_t *data);

#endif /* #ifndef GANON_TRANSPORT_H */
