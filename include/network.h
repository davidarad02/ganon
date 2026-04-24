#ifndef GANON_NETWORK_H
#define GANON_NETWORK_H

#include <pthread.h>
#include <netinet/in.h>

#include "args.h"
#include "common.h"
#include "err.h"
#include "transport.h"

/* network_message_cb_t is defined in skin.h (pulled in via transport.h) */

#define NETWORK_BUFFER_SIZE 4096

typedef struct network_t network_t;
typedef struct socket_entry socket_entry_t;

typedef void (*network_disconnected_cb_t)(transport_t *t);
typedef void (*network_connected_cb_t)(transport_t *t);

/* One listening endpoint bound to one skin. */
typedef struct {
    const skin_ops_t *skin;
    skin_listener_t  *skin_listener;
    int               listen_fd;   /* -1 if skin manages its own I/O */
    addr_t            addr;
    pthread_t         accept_thread;
    network_t        *net;
    int               running;
} listener_t;

struct socket_entry {
    transport_t    *t;
    pthread_t       thread;
    struct socket_entry *next;
    network_t      *net;
};

struct network_t {
    listener_t     *listeners;
    int             listener_count;

    socket_entry_t *clients;
    pthread_mutex_t clients_mutex;
    int             running;

    addr_t *connect_addrs;   /* legacy pointer into args; skin_id in connect_entries */
    connect_entry_t *connect_entries;
    int              connect_count;
    pthread_t       *connect_threads;
    int              connect_thread_count;
    int              connect_timeout;
    int              reconnect_retries;
    int              reconnect_delay;
    int              node_id;
    uint32_t         default_skin_id;

    network_message_cb_t     message_cb;
    network_disconnected_cb_t disconnected_cb;
    network_connected_cb_t    connected_cb;
};

err_t NETWORK__init(OUT network_t *net, IN const args_t *args, IN int node_id,
                    IN network_message_cb_t msg_cb,
                    IN network_disconnected_cb_t disc_cb,
                    IN network_connected_cb_t conn_cb);
err_t NETWORK__shutdown(IN network_t *net);

transport_t *NETWORK__get_transport(IN network_t *net, IN uint32_t node_id);
void         NETWORK__close_transport(IN network_t *net, IN transport_t *t);

/* Dynamic connect/disconnect */
err_t NETWORK__connect_to_peer(IN network_t *net,
                                IN const char *ip, IN int port,
                                IN const skin_ops_t *skin,
                                OUT int *status, OUT uint32_t *error_code,
                                OUT int *out_fd);
err_t NETWORK__disconnect_from_peer(IN network_t *net, IN uint32_t node_id,
                                     OUT int *status, OUT uint32_t *error_code);

extern network_t g_network;

#endif /* #ifndef GANON_NETWORK_H */
