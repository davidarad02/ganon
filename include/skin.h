#ifndef GANON_SKIN_H
#define GANON_SKIN_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "err.h"
#include "protocol.h"

typedef struct transport transport_t;
typedef struct skin_ops skin_ops_t;
typedef struct skin_listener skin_listener_t;

/* Per-message callback fired by the skin once per decoded inbound message.
 * Defined here (not in network.h) so skins can use it without depending on
 * the full network layer. */
typedef void (*network_message_cb_t)(transport_t *t, const protocol_msg_t *msg,
                                     const uint8_t *data, size_t data_len);

/* Wire-stable skin identifiers. */
typedef enum {
    SKIN_ID__TCP_MONOCYPHER = 1,
    SKIN_ID__TCP_PLAIN    = 2,
    SKIN_ID__TCP_XOR      = 3,
    SKIN_ID__TCP_CHACHA20 = 4,
    SKIN_ID__SSH          = 5,
    SKIN_ID__QUIC         = 6,
} skin_id_t;

/* Vtable: every skin implements these. */
struct skin_ops {
    uint32_t    skin_id;   /* wire-stable, matches skin_id_t */
    const char *name;      /* "tcp-monocypher", "tls13", etc. */

    /* ---- Listener lifecycle (server side) ---- */
    /* Create listening endpoint on addr.  Returns skin_listener_t* and the
     * OS listen fd (-1 if skin manages its own I/O). */
    err_t (*listener_create)(IN const addr_t *addr,
                             OUT skin_listener_t **out_listener,
                             OUT int *out_listen_fd);

    /* Block until a new connection arrives.  Returns a fully-initialised
     * transport_t with handshake complete. */
    err_t (*listener_accept)(IN skin_listener_t *l,
                             OUT transport_t **out_transport);

    void  (*listener_destroy)(IN skin_listener_t *l);

    /* ---- Client side ---- */
    /* Dial ip:port, perform handshake, return ready transport. */
    err_t (*connect)(IN const char *ip, IN int port,
                     IN int connect_timeout_sec,
                     OUT transport_t **out_transport);

    /* ---- Per-message I/O ---- */
    err_t (*send_msg)(IN transport_t *t, IN const protocol_msg_t *msg,
                      IN const uint8_t *data);
    err_t (*recv_msg)(IN transport_t *t, OUT protocol_msg_t *msg,
                      OUT uint8_t **data);

    /* ---- Teardown ---- */
    /* Close fd, wipe keys, free skin_ctx (but NOT the transport_t itself). */
    void  (*transport_destroy)(IN transport_t *t);
};

/* Skin registry. */
err_t SKIN__register(IN const skin_ops_t *ops);
const skin_ops_t *SKIN__by_id(IN uint32_t skin_id);
const skin_ops_t *SKIN__by_name(IN const char *name);
const skin_ops_t *SKIN__default(void);
void SKIN__set_default(IN uint32_t skin_id);

#endif /* #ifndef GANON_SKIN_H */
