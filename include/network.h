#ifndef GANON_NETWORK_H
#define GANON_NETWORK_H

#include <pthread.h>
#include <netinet/in.h>

#include "args.h"
#include "connection.h"
#include "err.h"

#define NETWORK_BUFFER_SIZE 4096

typedef struct network_t network_t;
typedef struct socket_entry socket_entry_t;

typedef void (*network_message_cb_t)(void *session_ctx, connection_t *conn, const uint8_t *buf, size_t len);
typedef void (*network_disconnected_cb_t)(void *session_ctx, connection_t *conn);
typedef void (*network_connected_cb_t)(void *session_ctx, connection_t *conn);
typedef void (*network_send_fn_t)(uint32_t node_id, const uint8_t *buf, size_t len, void *ctx);

struct socket_entry {
    connection_t conn;
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

connection_t *NETWORK__get_connection(network_t *net, uint32_t node_id);
void NETWORK__close_connection(network_t *net, connection_t *conn);
void NETWORK__broadcast_to_all(network_t *net, const uint8_t *buf, size_t len, uint32_t exclude_node_id);

extern int g_node_id;

#endif /* #ifndef GANON_NETWORK_H */
