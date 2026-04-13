#ifndef GANON_NETWORK_H
#define GANON_NETWORK_H

#include <pthread.h>
#include <netinet/in.h>

#include "args.h"
#include "err.h"
#include "routing.h"

#define NETWORK_BUFFER_SIZE 4096

typedef struct network_t network_t;

typedef struct socket_entry {
    int fd;
    pthread_t thread;
    struct socket_entry *next;
    network_t *net;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    int is_incoming;
    uint32_t peer_node_id;
} socket_entry_t;

struct network_t {
    int listen_fd;
    pthread_t accept_thread;
    socket_entry_t *clients;
    pthread_mutex_t clients_mutex;
    routing_table_t routing_table;
    int running;
    addr_t listen_addr;
    addr_t *connect_addrs;
    int connect_count;
    pthread_t *connect_threads;
    int connect_thread_count;
    int connect_timeout;
    int reconnect_retries;
    int reconnect_delay;
};

err_t NETWORK__init(network_t *net, const args_t *args);
err_t NETWORK__shutdown(network_t *net);

extern int g_node_id;

#endif /* #ifndef GANON_NETWORK_H */
