#ifndef GANON_NETWORK_H
#define GANON_NETWORK_H

#include <pthread.h>
#include <netinet/in.h>

#include "args.h"
#include "common.h"
#include "err.h"
#include "transport.h"

#define NETWORK_BUFFER_SIZE 4096

typedef struct network_t network_t;
typedef struct socket_entry socket_entry_t;

typedef void (*network_message_cb_t)(transport_t *t, const protocol_msg_t *msg, const uint8_t *data, size_t data_len);
typedef void (*network_disconnected_cb_t)(transport_t *t);
typedef void (*network_connected_cb_t)(transport_t *t);

struct socket_entry {
    transport_t *t;
    pthread_t thread;
    struct socket_entry *next;
    network_t *net;
};

struct network_t {
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
    int connect_timeout;
    int reconnect_retries;
    int reconnect_delay;
    int node_id;
    network_message_cb_t message_cb;
    network_disconnected_cb_t disconnected_cb;
    network_connected_cb_t connected_cb;
};

err_t NETWORK__init(OUT network_t *net, IN const args_t *args, IN int node_id, IN network_message_cb_t msg_cb, IN network_disconnected_cb_t disc_cb, IN network_connected_cb_t conn_cb);
err_t NETWORK__shutdown(IN network_t *net);

transport_t *NETWORK__get_transport(IN network_t *net, IN uint32_t node_id);
void NETWORK__close_transport(IN network_t *net, IN transport_t *t);

extern network_t g_network;

#endif /* #ifndef GANON_NETWORK_H */
