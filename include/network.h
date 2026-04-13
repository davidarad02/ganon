#ifndef GANON_NETWORK_H
#define GANON_NETWORK_H

#include <pthread.h>

#include "args.h"
#include "err.h"

#define NETWORK_BUFFER_SIZE 4096
#define NETWORK_CONNECT_TIMEOUT_SEC 5

typedef struct socket_entry {
    int fd;
    pthread_t thread;
    struct socket_entry *next;
} socket_entry_t;

typedef struct {
    int listen_fd;
    pthread_t accept_thread;
    socket_entry_t *clients;
    pthread_mutex_t clients_mutex;
    int running;
    addr_t listen_addr;
    addr_t *connect_addrs;
    int connect_count;
    pthread_t *connect_threads;
    int connect_thread_count;
} network_t;

err_t network_init(network_t *net, const args_t *args);
err_t network_shutdown(network_t *net);
void network_print_status(network_t *net);

#endif /* #ifndef GANON_NETWORK_H */
