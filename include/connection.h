#ifndef GANON_CONNECTION_H
#define GANON_CONNECTION_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

typedef struct connection connection_t;

struct connection {
    uint32_t node_id;
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    void *ctx;
};

void CONNECTION__init(connection_t *conn, int fd);
void CONNECTION__set_node_id(connection_t *conn, uint32_t node_id);
uint32_t CONNECTION__get_node_id(connection_t *conn);
int CONNECTION__get_fd(connection_t *conn);
void CONNECTION__close(connection_t *conn);

#endif /* #ifndef GANON_CONNECTION_H */
