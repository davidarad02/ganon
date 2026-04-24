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

/* Encryption frame constants */
#define ENC_FRAME_OVERHEAD 44
#define ENC_NONCE_SIZE 24
#define ENC_MAC_SIZE 16

typedef struct network_t network_t;
typedef struct transport transport_t;

typedef enum {
    ENC__INIT = 0,
    ENC__WAIT,
    ENC__ESTABLISHED,
} enc_state_t;

struct transport {
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    uint32_t node_id;
    void *ctx;
    ssize_t (*recv)(int fd, uint8_t *buf, size_t len);
    ssize_t (*send)(int fd, const uint8_t *buf, size_t len);

    /* Set to 1 when the socket is managed by an epoll event loop.
     * In this mode TRANSPORT__send_msg() enqueues instead of calling
     * send() directly, and the event loop drains the outbound queue. */
    int is_nonblocking;

    /* Transport-layer encryption state */
    enc_state_t enc_state;
    uint8_t enc_ephemeral_priv[32];
    uint8_t enc_ephemeral_pub[32];
    uint8_t enc_send_key[32];
    uint8_t enc_recv_key[32];
    uint64_t enc_send_nonce;
    uint64_t enc_recv_nonce;
    uint8_t enc_session_id[8];
    int enc_is_initiator;

    /* Cached XChaCha20 subkeys: crypto_aead_lock()/unlock() internally
     * call crypto_chacha20_h() on every message.  Because our nonces
     * always have 16 zero bytes in the first half, the subkey is
     * identical for every message.  We precompute it once after the
     * handshake and reuse it, shaving 20 ChaCha20 rounds off each
     * encrypt/decrypt operation. */
    uint8_t enc_send_subkey[32];
    uint8_t enc_recv_subkey[32];

    /* Outbound queue for epoll event-loop mode.
     * Tunnel / routing threads enqueue frames here; the event loop
     * drains them via EPOLLOUT.  In thread-per-connection mode these
     * are unused (always NULL).  Protected by out_mutex. */
    struct transport_outbuf {
        uint8_t *data;
        size_t len;
        size_t sent;
        struct transport_outbuf *next;
    } *out_head, *out_tail;
    pthread_mutex_t out_mutex;
    int out_has_data;  /* atomic-ish flag: 1 if queue non-empty */

    /* Epoll recv buffer: accumulates partial encrypted frames. */
    uint8_t *recv_buf;
    size_t recv_buf_len;
    size_t recv_buf_cap;

    /* Epoll synchronization: socket_thread_func waits on this CV after
     * registering with epoll.  The epoll loop signals it on disconnect. */
    pthread_cond_t epoll_cv;
    pthread_mutex_t epoll_cv_mutex;
    int epoll_disconnect_flag;
    int disconnected_cb_called;
};

ssize_t TRANSPORT__recv(IN int fd, OUT uint8_t *buf, IN size_t len);
ssize_t TRANSPORT__send(IN int fd, IN const uint8_t *buf, IN size_t len);

transport_t *TRANSPORT__create(IN int fd);
void TRANSPORT__destroy(IN transport_t *t);

err_t TRANSPORT__recv_all(IN transport_t *t, OUT uint8_t *buf, IN size_t len, OUT ssize_t *bytes_read);
err_t TRANSPORT__send_all(IN transport_t *t, IN const uint8_t *buf, IN size_t len, OUT ssize_t *bytes_sent);

err_t TRANSPORT__recv_msg(IN transport_t *t, OUT protocol_msg_t *msg, OUT uint8_t **data);
err_t TRANSPORT__send_msg(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data);

/* Decrypt and unserialize a complete encrypted frame (nonce+mac+ciphertext).
 * Used by both blocking recv and epoll event loop. */
err_t TRANSPORT__decrypt_frame(IN transport_t *t, IN uint8_t *payload, IN size_t payload_len,
                               OUT protocol_msg_t *msg, OUT uint8_t **data);

int TRANSPORT__get_fd(IN transport_t *t);
uint32_t TRANSPORT__get_node_id(IN transport_t *t);
void TRANSPORT__set_node_id(IN transport_t *t, IN uint32_t node_id);

err_t TRANSPORT__send_to_node_id(IN network_t *net, IN uint32_t node_id, IN const protocol_msg_t *msg, IN const uint8_t *data);

err_t TRANSPORT__do_handshake(IN transport_t *t, IN int is_initiator);

/* Epoll mode: enqueue an encrypted frame for the event loop to send.
 * Called by TRANSPORT__send_msg() when the socket is non-blocking and
 * managed by an epoll event loop.  Thread-safe. */
err_t TRANSPORT__enqueue_outbuf(IN transport_t *t, IN uint8_t *data, IN size_t len);

/* Epoll mode: drain as much of the outbound queue as the kernel will
 * accept without blocking.  Called only from the event loop thread.
 * Returns E__SUCCESS even if partial send occurred. */
err_t TRANSPORT__drain_outbuf(IN transport_t *t, OUT int *would_block);

#endif /* #ifndef GANON_TRANSPORT_H */
