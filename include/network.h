#ifndef GANON_NETWORK_H
#define GANON_NETWORK_H

#include <pthread.h>
#include <netinet/in.h>

#include "args.h"
#include "err.h"
#include "transport.h"

#define NETWORK_BUFFER_SIZE 4096

typedef struct network_t network_t;
typedef struct socket_entry socket_entry_t;

typedef void (*network_message_cb_t)(void *session_ctx, transport_t *t, const uint8_t *buf, size_t len);
typedef void (*network_disconnected_cb_t)(void *session_ctx, transport_t *t);
typedef void (*network_connected_cb_t)(void *session_ctx, transport_t *t);
typedef void (*network_send_fn_t)(uint32_t node_id, const uint8_t *buf, size_t len, void *ctx);

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
    void *session_ctx;
    network_message_cb_t message_cb;
    network_disconnected_cb_t disconnected_cb;
    network_connected_cb_t connected_cb;
    network_send_fn_t send_fn;
    void *send_ctx;
};

err_t NETWORK__init(network_t *net, const args_t *args, int node_id, network_message_cb_t msg_cb, network_disconnected_cb_t disc_cb, network_connected_cb_t conn_cb, void *ctx);
err_t NETWORK__shutdown(network_t *net);

void NETWORK__set_send_fn(network_t *net, network_send_fn_t fn, void *ctx);
err_t NETWORK__send_to(network_t *net, uint32_t node_id, const uint8_t *buf, size_t len);

transport_t *NETWORK__get_transport(network_t *net, uint32_t node_id);
void NETWORK__close_transport(network_t *net, transport_t *t);
void NETWORK__broadcast_to_all(network_t *net, const uint8_t *buf, size_t len, uint32_t exclude_node_id);

extern int g_node_id;

#endif /* #ifndef GANON_NETWORK_H */
