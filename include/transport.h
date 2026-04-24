#ifndef GANON_TRANSPORT_H
#define GANON_TRANSPORT_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "common.h"
#include "err.h"
#include "protocol.h"
#include "skin.h"

typedef struct network_t network_t;
typedef struct transport transport_t;

struct transport {
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    uint32_t node_id;
    void *ctx;   /* generic back-pointer (e.g. network_t*); skin state is in skin_ctx */

    /* Set to 1 when the socket is managed by an epoll event loop. */
    int is_nonblocking;

    /* Pluggable skin vtable + per-connection opaque state. */
    const skin_ops_t *skin;
    void *skin_ctx;

    /* Epoll synchronization: socket_thread_func waits on this CV after
     * registering with epoll.  The epoll loop signals it on disconnect. */
    pthread_cond_t  epoll_cv;
    pthread_mutex_t epoll_cv_mutex;
    int epoll_disconnect_flag;
    int disconnected_cb_called;
};

/* Allocate a bare transport_t with generic fields initialised.
 * Does NOT allocate skin_ctx — the skin is responsible for that. */
transport_t *TRANSPORT__alloc_base(IN int fd, IN const skin_ops_t *skin);

/* Free the bare transport_t struct (NOT skin_ctx or the fd). */
void TRANSPORT__free_base(IN transport_t *t);

/* Full destroy: calls t->skin->transport_destroy(t) then frees the struct. */
void TRANSPORT__destroy(IN transport_t *t);

/* Dispatch send/recv through the skin vtable. */
err_t TRANSPORT__recv_msg(IN transport_t *t, OUT protocol_msg_t *msg, OUT uint8_t **data);
err_t TRANSPORT__send_msg(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data);

/* Accessors */
int      TRANSPORT__get_fd(IN transport_t *t);
uint32_t TRANSPORT__get_node_id(IN transport_t *t);
void     TRANSPORT__set_node_id(IN transport_t *t, IN uint32_t node_id);

/* Convenience: look up node_id's transport in net and send. */
err_t TRANSPORT__send_to_node_id(IN network_t *net, IN uint32_t node_id,
                                  IN const protocol_msg_t *msg,
                                  IN const uint8_t *data);

#endif /* #ifndef GANON_TRANSPORT_H */
