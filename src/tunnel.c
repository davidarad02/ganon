#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "protocol.h"
#include "routing.h"
#include "session.h"
#include "tunnel.h"

#define MAX_SRC_TUNNELS      32
#define MAX_CONNS_PER_TUNNEL 64
#define MAX_DST_CONNS        512
#define TUNNEL_BUF_SIZE      65536

#define TUNNEL_PROTO_TCP 0
#define TUNNEL_PROTO_UDP 1

/* ---- payload wire formats (network byte order) ---- */

typedef struct __attribute__((packed)) {
    uint32_t tunnel_id;
    uint32_t dst_node_id;
    uint16_t src_port;
    uint16_t remote_port;
    uint8_t  protocol;
    uint8_t  pad[3];
    char     src_host[64];
    char     remote_host[256];
} tunnel_open_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t tunnel_id;
    uint32_t conn_id;
    uint16_t remote_port;
    uint8_t  protocol;
    uint8_t  pad;
    char     remote_host[256];
} tunnel_conn_open_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t tunnel_id;
    uint32_t conn_id;
} tunnel_ids_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t tunnel_id;
    uint32_t flags;  /* 0 = soft close (default), 1 = force close */
} tunnel_id_payload_t;

#define TUNNEL_CLOSE_FLAG_SOFT  0
#define TUNNEL_CLOSE_FLAG_FORCE 1

/* ---- runtime state ---- */

/* Forward declaration so src_tunnel_t can hold a udp_ctx_t* before the full
 * definition appears below. */
struct udp_ctx_t;
typedef struct udp_ctx_t udp_ctx_t;

typedef struct {
    int             fd;
    uint32_t        conn_id;
    uint32_t        tunnel_id;
    uint32_t        dst_node_id;
    volatile int    is_active;
    volatile int    is_stalled;  /* 1 = no route available, TCP backpressure applied */
    volatile int    ack_received; /* 1 = TUNNEL_CONN_ACK received from dst node */
    pthread_t       fwd_thread;
} src_conn_t;

typedef struct {
    uint32_t        tunnel_id;
    uint32_t        dst_node_id;
    uint16_t        src_port;
    uint16_t        remote_port;
    uint8_t         protocol;
    char            src_host[64];
    char            remote_host[256];
    int             listen_fd;
    volatile int    is_active;
    volatile int    is_soft_closed;  /* 1 = soft closed (no new connections), 0 = normal */
    uint32_t        next_conn_id;
    pthread_t       accept_thread;
    udp_ctx_t      *udp_ctx; /* NULL unless protocol == TUNNEL_PROTO_UDP */
    src_conn_t      conns[MAX_CONNS_PER_TUNNEL];
} src_tunnel_t;

typedef struct {
    uint32_t        tunnel_id;
    uint32_t        conn_id;
    uint32_t        src_node_id;
    int             fd;
    volatile int    is_active;
    volatile int    is_stalled;  /* 1 = no route available, TCP backpressure applied */
    pthread_t       fwd_thread;
    /* UDP-specific fields for destination-side forwarding */
    int             is_udp;
    struct sockaddr_in udp_remote_addr;
    socklen_t       udp_remote_addr_len;
} dst_conn_t;

typedef struct {
    int                fd;
    uint32_t           tunnel_id;
    uint32_t           conn_id;
    uint32_t           peer_node_id;
    volatile int      *p_is_active;
    volatile uint32_t *p_slot_conn_id; /* points to conn->conn_id in the slot; used to
                                          detect slot reuse before doing fd cleanup */
    volatile int      *p_ack_received; /* NULL for dst side; &conn->ack_received for src side */
    /* UDP-specific fields for destination-side forwarding */
    int                is_udp;
    struct sockaddr_in udp_remote_addr;
    socklen_t          udp_remote_addr_len;
} fwd_ctx_t;

typedef struct {
    src_tunnel_t *tunnel;
} accept_ctx_t;

/* Pre-ACK packet queue for UDP clients */
#define MAX_PRE_ACK_PACKETS 8
#define MAX_PRE_ACK_DATA    2048

typedef struct {
    uint8_t  data[MAX_PRE_ACK_DATA];
    uint32_t data_len;
    int      valid;
} pre_ack_packet_t;

/* UDP client tracking - since UDP is connectionless, we track clients by address */
typedef struct {
    struct sockaddr_in addr;
    socklen_t addr_len;
    uint32_t conn_id;
    time_t last_activity;
    int is_active;
    volatile int ack_received;  /* Set when CONN_ACK arrives */
    pre_ack_packet_t pre_ack_queue[MAX_PRE_ACK_PACKETS];
    int pre_ack_count;
} udp_client_t;

#define MAX_UDP_CLIENTS 64

struct udp_ctx_t {
    src_tunnel_t *tunnel;
    int udp_fd;
    udp_client_t clients[MAX_UDP_CLIENTS];
    pthread_mutex_t clients_mutex;
};

/* Heap-allocated context for the per-connection dst-side setup thread, so that
 * handle_tunnel_conn_open can return immediately without blocking the recv thread. */
typedef struct {
    uint32_t tunnel_id;
    uint32_t conn_id;
    uint32_t src_node_id;
    uint16_t remote_port;
    uint8_t  protocol;
    char     remote_host[256];
} dst_connect_ctx_t;

static src_tunnel_t g_src_tunnels[MAX_SRC_TUNNELS];
static dst_conn_t   g_dst_conns[MAX_DST_CONNS];
static pthread_mutex_t g_tunnel_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ack_cond     = PTHREAD_COND_INITIALIZER;
static volatile int g_tunnel_running = 0;
int g_tunnel_tcp_rcvbuf = 0;  /* TCP receive buffer size in bytes (0 = system default) */

/* ---- helpers ---- */

static err_t tunnel_send_with_retry(uint32_t dst_node_id, msg_type_t type, uint32_t channel_id,
                                     const uint8_t *data, uint32_t data_len, int *no_route) {
    err_t rc = E__SUCCESS;
    protocol_msg_t msg;
    session_t *s = SESSION__get_session();

    if (NULL != no_route) {
        *no_route = 0;
    }

    memset(&msg, 0, sizeof(msg));
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)s->node_id;
    msg.src_node_id      = (uint32_t)s->node_id;
    msg.dst_node_id      = dst_node_id;
    msg.message_id       = SESSION__get_next_msg_id();
    msg.type             = (uint32_t)type;
    msg.data_length      = data_len;
    msg.ttl              = DEFAULT_TTL;
    msg.channel_id       = channel_id;

    rc = ROUTING__route_message(&msg, data, 0);
    if (E__SUCCESS != rc) {
        if (NULL != no_route && rc == E__ROUTING__NODE_NOT_FOUND) {
            *no_route = 1;
        }
        goto l_cleanup;
    }

l_cleanup:
    return rc;
}

static err_t tunnel_send(uint32_t dst_node_id, msg_type_t type, uint32_t channel_id,
                          const uint8_t *data, uint32_t data_len) {
    return tunnel_send_with_retry(dst_node_id, type, channel_id, data, data_len, NULL);
}

/* ---- forward thread (shared for src and dst sides) ---- */

static void *fwd_thread_func(void *arg) {
    fwd_ctx_t *ctx = (fwd_ctx_t *)arg;
    uint8_t *buf = malloc(TUNNEL_BUF_SIZE + 8);
    if (NULL == buf) {
        free(ctx);
        return NULL;
    }

    /* Src side: block until dst has registered its connection slot and sent CONN_ACK.
     * Without this, TUNNEL_DATA can arrive at the dst before dst_connect_thread has
     * stored the dconn entry, causing the data to be silently dropped. */
    if (NULL != ctx->p_ack_received) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 10;
        pthread_mutex_lock(&g_tunnel_mutex);
        while (g_tunnel_running && *ctx->p_is_active && !*ctx->p_ack_received) {
            int wrc = pthread_cond_timedwait(&g_ack_cond, &g_tunnel_mutex, &deadline);
            if (ETIMEDOUT == wrc) break;
        }
        int got_ack = *ctx->p_ack_received;
        pthread_mutex_unlock(&g_tunnel_mutex);
        if (!got_ack) {
            LOG_WARNING("TUNNEL %u conn %u: timed out waiting for CONN_ACK, closing",
                        ctx->tunnel_id, ctx->conn_id);
            pthread_mutex_lock(&g_tunnel_mutex);
            if ((*ctx->p_is_active == 1 || *ctx->p_is_active == 2) &&
                *ctx->p_slot_conn_id == ctx->conn_id) {
                *ctx->p_is_active = 0;
                close(ctx->fd);
            }
            pthread_mutex_unlock(&g_tunnel_mutex);
            free(buf);
            free(ctx);
            return NULL;
        }
    }

    if (ctx->is_udp) {
        /* UDP dst side: socket is connect()ed so we can use plain recv().
         * The connected socket binds a local ephemeral port up-front, ensuring
         * the kernel can deliver replies from the remote before any data is sent. */
        while (g_tunnel_running && *ctx->p_is_active) {
            ssize_t n = recv(ctx->fd, buf + 8, TUNNEL_BUF_SIZE, 0);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                continue;
            }

            uint32_t tid = htonl(ctx->tunnel_id);
            uint32_t cid = htonl(ctx->conn_id);
            memcpy(buf,     &tid, 4);
            memcpy(buf + 4, &cid, 4);

            /* Use conn_id as channel_id for per-connection data sequencing */
            tunnel_send(ctx->peer_node_id, MSG__TUNNEL_DATA, ctx->conn_id,
                        buf, (uint32_t)(n + 8));
        }

        if (*ctx->p_is_active) {
            tunnel_ids_payload_t close_p;
            close_p.tunnel_id = htonl(ctx->tunnel_id);
            close_p.conn_id   = htonl(ctx->conn_id);
            tunnel_send(ctx->peer_node_id, MSG__TUNNEL_CONN_CLOSE, ctx->conn_id,
                        (uint8_t *)&close_p, sizeof(close_p));
            pthread_mutex_lock(&g_tunnel_mutex);
            if (*ctx->p_is_active && *ctx->p_slot_conn_id == ctx->conn_id) {
                *ctx->p_is_active = 0;
                close(ctx->fd);
            }
            pthread_mutex_unlock(&g_tunnel_mutex);
            LOG_DEBUG("TUNNEL %u conn %u: UDP local socket closed, sent CONN_CLOSE", ctx->tunnel_id, ctx->conn_id);
        }
    } else {
        /* TCP: Use connected socket recv/send with route outage handling */
        int is_stalled = 0;
        while (g_tunnel_running && *ctx->p_is_active) {
            if (!is_stalled) {
                /* Normal operation: read from socket and forward */
                ssize_t n = recv(ctx->fd, buf + 8, TUNNEL_BUF_SIZE, 0);
                if (n <= 0) {
                    break;
                }

                uint32_t tid = htonl(ctx->tunnel_id);
                uint32_t cid = htonl(ctx->conn_id);
                memcpy(buf,     &tid, 4);
                memcpy(buf + 4, &cid, 4);

                /* Try to send - if no route, enter stalled state */
                int no_route = 0;
                err_t rc = tunnel_send_with_retry(ctx->peer_node_id, MSG__TUNNEL_DATA, ctx->conn_id,
                                                   buf, (uint32_t)(n + 8), &no_route);
                if (E__SUCCESS != rc) {
                    if (no_route) {
                        /* No route available - enter stalled state.
                         * TCP backpressure: stop reading, kernel will buffer incoming data */
                        is_stalled = 1;
                        *ctx->p_is_active = 2; /* 2 = stalled (still active but paused) */
                        LOG_INFO("TUNNEL %u conn %u: no route to node %u, entering stalled state (TCP backpressure)",
                                 ctx->tunnel_id, ctx->conn_id, ctx->peer_node_id);
                        /* Trigger route discovery */
                        ROUTING__send_rreq(ctx->peer_node_id);
                    } else {
                        /* Other error - close connection */
                        break;
                    }
                }
            } else {
                /* Stalled state: no route available.
                 * We don't read from socket, letting TCP backpressure build up.
                 * Periodically try to send a probe to check if route is back. */
                sleep(1);
                
                /* Check if route is available */
                if (ROUTING__has_route(ctx->peer_node_id)) {
                    /* Route is back! Resume normal operation */
                    LOG_INFO("TUNNEL %u conn %u: route to node %u restored, resuming",
                             ctx->tunnel_id, ctx->conn_id, ctx->peer_node_id);
                    is_stalled = 0;
                    *ctx->p_is_active = 1; /* Back to active */
                } else {
                    /* Still no route - trigger another RREQ periodically */
                    static time_t last_rreq = 0;
                    time_t now = time(NULL);
                    if (now - last_rreq >= 5) {
                        ROUTING__send_rreq(ctx->peer_node_id);
                        last_rreq = now;
                    }
                }
            }
        }

        /* Connection closing - only send CONN_CLOSE if we weren't stalled or on clean close */
        if (*ctx->p_is_active && !is_stalled) {
            tunnel_ids_payload_t close_p;
            close_p.tunnel_id = htonl(ctx->tunnel_id);
            close_p.conn_id   = htonl(ctx->conn_id);
            /* Use conn_id as channel_id for per-connection isolation */
            tunnel_send(ctx->peer_node_id, MSG__TUNNEL_CONN_CLOSE, ctx->conn_id,
                        (uint8_t *)&close_p, sizeof(close_p));
        }
        
        if (*ctx->p_is_active) {
            /* Re-check under lock: handle_tunnel_conn_close may have already cleaned up
             * this slot, or the slot may have been reused for a new connection. */
            pthread_mutex_lock(&g_tunnel_mutex);
            if ((*ctx->p_is_active == 1 || *ctx->p_is_active == 2) && *ctx->p_slot_conn_id == ctx->conn_id) {
                *ctx->p_is_active = 0;
                close(ctx->fd);
            }
            pthread_mutex_unlock(&g_tunnel_mutex);
            if (is_stalled) {
                LOG_INFO("TUNNEL %u conn %u: closed after route outage (was stalled)", ctx->tunnel_id, ctx->conn_id);
            } else {
                LOG_DEBUG("TUNNEL %u conn %u: local socket closed, sent CONN_CLOSE", ctx->tunnel_id, ctx->conn_id);
            }
        }
    }

    free(buf);
    free(ctx);
    return NULL;
}

/* ---- accept thread (src side) ---- */

static void *src_accept_thread(void *arg) {
    accept_ctx_t *ctx = (accept_ctx_t *)arg;
    src_tunnel_t *tunnel = ctx->tunnel;
    free(ctx);

    while (g_tunnel_running && tunnel->is_active) {
        /* Check if soft-closed: don't accept new connections, but keep
         * thread alive until existing connections close naturally */
        if (tunnel->is_soft_closed) {
            /* Check if all connections are closed */
            int has_active_conns = 0;
            pthread_mutex_lock(&g_tunnel_mutex);
            for (int i = 0; i < MAX_CONNS_PER_TUNNEL; i++) {
                if (tunnel->conns[i].is_active) {
                    has_active_conns = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&g_tunnel_mutex);
            if (!has_active_conns) {
                LOG_INFO("TUNNEL %u: soft close complete, all connections closed", tunnel->tunnel_id);
                break;
            }
            /* Wait a bit before checking again */
            usleep(100000); /* 100ms */
            continue;
        }

        int client_fd = accept(tunnel->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!tunnel->is_active) {
                break;
            }
            if (EINTR == errno) {
                continue;
            }
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                /* Non-blocking socket - check again after short delay */
                usleep(10000); /* 10ms */
                continue;
            }
            LOG_WARNING("TUNNEL %u: accept() error: %s", tunnel->tunnel_id, strerror(errno));
            break;
        }

        pthread_mutex_lock(&g_tunnel_mutex);
        src_conn_t *conn = NULL;
        for (int i = 0; i < MAX_CONNS_PER_TUNNEL; i++) {
            if (!tunnel->conns[i].is_active) {
                conn = &tunnel->conns[i];
                break;
            }
        }
        if (NULL == conn) {
            pthread_mutex_unlock(&g_tunnel_mutex);
            LOG_WARNING("TUNNEL %u: max connections reached, dropping", tunnel->tunnel_id);
            close(client_fd);
            continue;
        }

        uint32_t conn_id = ++tunnel->next_conn_id;
        conn->fd           = client_fd;
        conn->conn_id      = conn_id;
        conn->tunnel_id    = tunnel->tunnel_id;
        conn->dst_node_id  = tunnel->dst_node_id;
        conn->is_active    = 1;
        conn->ack_received = 0;

        int nodelay = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
            LOG_WARNING("TUNNEL %u conn %u: setsockopt(TCP_NODELAY) failed: %s",
                       tunnel->tunnel_id, conn_id, strerror(errno));
        }

        /* Set TCP receive buffer size for connection socket if configured */
        if (g_tunnel_tcp_rcvbuf > 0) {
            int rcvbuf = g_tunnel_tcp_rcvbuf;
            if (setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
                LOG_WARNING("TUNNEL %u conn %u: setsockopt(SO_RCVBUF) failed: %s", 
                           tunnel->tunnel_id, conn_id, strerror(errno));
            }
        }
        
        pthread_mutex_unlock(&g_tunnel_mutex);

        /* Send TUNNEL_CONN_OPEN to dst node using tunnel_id as master channel */
        tunnel_conn_open_payload_t p;
        memset(&p, 0, sizeof(p));
        p.tunnel_id   = htonl(tunnel->tunnel_id);
        p.conn_id     = htonl(conn_id);
        p.remote_port = htons(tunnel->remote_port);
        p.protocol    = tunnel->protocol;
        snprintf(p.remote_host, sizeof(p.remote_host), "%s", tunnel->remote_host);

        /* Use conn_id as channel_id for per-connection isolation in load balancer */
        tunnel_send(tunnel->dst_node_id, MSG__TUNNEL_CONN_OPEN, conn_id,
                    (uint8_t *)&p, sizeof(p));
        LOG_INFO("TUNNEL %u conn %u: client connected, sent CONN_OPEN to node %u",
                 tunnel->tunnel_id, conn_id, tunnel->dst_node_id);

        fwd_ctx_t *fwd = malloc(sizeof(fwd_ctx_t));
        if (NULL == fwd) {
            conn->is_active = 0;
            close(client_fd);
            continue;
        }
        fwd->fd              = client_fd;
        fwd->tunnel_id       = tunnel->tunnel_id;
        fwd->conn_id         = conn_id;
        fwd->peer_node_id    = tunnel->dst_node_id;
        fwd->p_is_active     = &conn->is_active;
        fwd->p_slot_conn_id  = (volatile uint32_t *)&conn->conn_id;
        fwd->p_ack_received  = &conn->ack_received;

        pthread_create(&conn->fwd_thread, NULL, fwd_thread_func, fwd);
        pthread_detach(conn->fwd_thread);
    }

    return NULL;
}

/* ---- UDP packet forwarding thread ---- */

static udp_client_t* udp_find_or_create_client(udp_ctx_t *ctx, struct sockaddr_in *addr, socklen_t addr_len, uint32_t *conn_id_out) {
    time_t now = time(NULL);
    pthread_mutex_lock(&ctx->clients_mutex);
    
    /* Look for existing client by address */
    for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
        if (ctx->clients[i].is_active && 
            ctx->clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            ctx->clients[i].addr.sin_port == addr->sin_port) {
            ctx->clients[i].last_activity = now;
            *conn_id_out = ctx->clients[i].conn_id;
            pthread_mutex_unlock(&ctx->clients_mutex);
            return &ctx->clients[i];
        }
    }
    
    /* Find free slot for new client */
    for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
        if (!ctx->clients[i].is_active) {
            ctx->clients[i].is_active = 1;
            memcpy(&ctx->clients[i].addr, addr, sizeof(*addr));
            ctx->clients[i].addr_len = addr_len;
            ctx->clients[i].conn_id = ++ctx->tunnel->next_conn_id;
            ctx->clients[i].last_activity = now;
            ctx->clients[i].ack_received = 0;
            ctx->clients[i].pre_ack_count = 0;
            memset(ctx->clients[i].pre_ack_queue, 0, sizeof(ctx->clients[i].pre_ack_queue));
            *conn_id_out = ctx->clients[i].conn_id;
            
            /* Send TUNNEL_CONN_OPEN for this UDP "connection" */
            tunnel_conn_open_payload_t p;
            memset(&p, 0, sizeof(p));
            p.tunnel_id   = htonl(ctx->tunnel->tunnel_id);
            p.conn_id     = htonl(*conn_id_out);
            p.remote_port = htons(ctx->tunnel->remote_port);
            p.protocol    = ctx->tunnel->protocol;
            snprintf(p.remote_host, sizeof(p.remote_host), "%s", ctx->tunnel->remote_host);
            tunnel_send(ctx->tunnel->dst_node_id, MSG__TUNNEL_CONN_OPEN, *conn_id_out,
                        (uint8_t *)&p, sizeof(p));
            LOG_INFO("TUNNEL %u conn %u: UDP client %s:%u added",
                     ctx->tunnel->tunnel_id, *conn_id_out,
                     inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
            
            pthread_mutex_unlock(&ctx->clients_mutex);
            return &ctx->clients[i];
        }
    }
    
    pthread_mutex_unlock(&ctx->clients_mutex);
    return NULL;
}

static void *udp_recv_thread(void *arg) {
    udp_ctx_t *ctx = (udp_ctx_t *)arg;
    uint8_t *buf = malloc(TUNNEL_BUF_SIZE + 8);
    if (NULL == buf) {
        free(ctx);
        return NULL;
    }
    
    while (g_tunnel_running && ctx->tunnel->is_active && !ctx->tunnel->is_soft_closed) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        ssize_t n = recvfrom(ctx->udp_fd, buf + 8, TUNNEL_BUF_SIZE, 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            LOG_WARNING("TUNNEL %u: UDP recvfrom error: %s", ctx->tunnel->tunnel_id, strerror(errno));
            break;
        }
        if (n == 0) {
            continue;
        }
        
        uint32_t conn_id;
        udp_client_t *client = udp_find_or_create_client(ctx, &client_addr, addr_len, &conn_id);
        if (NULL == client) {
            LOG_WARNING("TUNNEL %u: max UDP clients reached, dropping packet", ctx->tunnel->tunnel_id);
            continue;
        }
        
        /* Check if ACK received - if not, queue the packet */
        if (!client->ack_received) {
            if (client->pre_ack_count < MAX_PRE_ACK_PACKETS) {
                pre_ack_packet_t *pkt = &client->pre_ack_queue[client->pre_ack_count];
                if ((uint32_t)n <= MAX_PRE_ACK_DATA) {
                    memcpy(pkt->data, buf + 8, n);
                    pkt->data_len = (uint32_t)n;
                    pkt->valid = 1;
                    client->pre_ack_count++;
                    LOG_TRACE("TUNNEL %u conn %u: queued pre-ACK UDP packet (%d/%d)",
                              ctx->tunnel->tunnel_id, conn_id,
                              client->pre_ack_count, MAX_PRE_ACK_PACKETS);
                }
            } else {
                LOG_WARNING("TUNNEL %u conn %u: pre-ACK queue full, dropping packet",
                            ctx->tunnel->tunnel_id, conn_id);
            }
            continue;
        }
        
        /* ACK received - forward UDP packet immediately */
        uint32_t tid = htonl(ctx->tunnel->tunnel_id);
        uint32_t cid = htonl(conn_id);
        memcpy(buf, &tid, 4);
        memcpy(buf + 4, &cid, 4);
        
        tunnel_send(ctx->tunnel->dst_node_id, MSG__TUNNEL_DATA, conn_id,
                    buf, (uint32_t)(n + 8));
    }
    
    /* Cleanup: close all UDP client connections */
    pthread_mutex_lock(&ctx->clients_mutex);
    for (int i = 0; i < MAX_UDP_CLIENTS; i++) {
        if (ctx->clients[i].is_active) {
            tunnel_ids_payload_t close_p;
            close_p.tunnel_id = htonl(ctx->tunnel->tunnel_id);
            close_p.conn_id   = htonl(ctx->clients[i].conn_id);
            tunnel_send(ctx->tunnel->dst_node_id, MSG__TUNNEL_CONN_CLOSE, ctx->clients[i].conn_id,
                        (uint8_t *)&close_p, sizeof(close_p));
            ctx->clients[i].is_active = 0;
        }
    }
    pthread_mutex_unlock(&ctx->clients_mutex);
    
    pthread_mutex_destroy(&ctx->clients_mutex);
    free(buf);
    free(ctx);
    return NULL;
}

/* ---- message handlers ---- */

static void handle_tunnel_open(uint32_t src_node_id, const uint8_t *data, size_t data_len) {
    (void)src_node_id;

    if (data_len < sizeof(tunnel_open_payload_t)) {
        LOG_WARNING("TUNNEL_OPEN: payload too small (%zu)", data_len);
        return;
    }

    const tunnel_open_payload_t *p = (const tunnel_open_payload_t *)data;
    uint32_t tunnel_id   = ntohl(p->tunnel_id);
    uint32_t dst_node_id = ntohl(p->dst_node_id);
    uint16_t src_port    = ntohs(p->src_port);
    uint16_t remote_port = ntohs(p->remote_port);
    uint8_t  protocol    = p->protocol;

    pthread_mutex_lock(&g_tunnel_mutex);

    /* Reject duplicate tunnel IDs (but allow reuse of soft-closed tunnels) */
    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        if (g_src_tunnels[i].is_active && !g_src_tunnels[i].is_soft_closed && 
            g_src_tunnels[i].tunnel_id == tunnel_id) {
            pthread_mutex_unlock(&g_tunnel_mutex);
            LOG_WARNING("TUNNEL %u: already exists", tunnel_id);
            return;
        }
    }

    src_tunnel_t *tunnel = NULL;
    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        /* Allow reuse of inactive or soft-closed tunnels */
        if (!g_src_tunnels[i].is_active || g_src_tunnels[i].is_soft_closed) {
            tunnel = &g_src_tunnels[i];
            break;
        }
    }
    if (NULL == tunnel) {
        pthread_mutex_unlock(&g_tunnel_mutex);
        LOG_WARNING("TUNNEL: max tunnels reached");
        return;
    }

    memset(tunnel, 0, sizeof(*tunnel));
    tunnel->tunnel_id   = tunnel_id;
    tunnel->dst_node_id = dst_node_id;
    tunnel->src_port    = src_port;
    tunnel->remote_port = remote_port;
    tunnel->protocol    = protocol;
    snprintf(tunnel->src_host,    sizeof(tunnel->src_host),    "%s", p->src_host);
    snprintf(tunnel->remote_host, sizeof(tunnel->remote_host), "%s", p->remote_host);

    /* Determine socket type based on protocol */
    int sock_type = (TUNNEL_PROTO_UDP == protocol) ? SOCK_DGRAM : SOCK_STREAM;

    int listen_fd = socket(AF_INET, sock_type, 0);
    if (listen_fd < 0) {
        pthread_mutex_unlock(&g_tunnel_mutex);
        LOG_ERROR("TUNNEL: socket() failed: %s", strerror(errno));
        return;
    }

    /* Set non-blocking so accept()/recvfrom() can be interrupted by close() */
    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (-1 == flags || -1 == fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK)) {
        close(listen_fd);
        pthread_mutex_unlock(&g_tunnel_mutex);
        LOG_ERROR("TUNNEL: fcntl() failed: %s", strerror(errno));
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Set TCP receive buffer size if configured (for TCP sockets only) */
    if (TUNNEL_PROTO_UDP != protocol && g_tunnel_tcp_rcvbuf > 0) {
        int rcvbuf = g_tunnel_tcp_rcvbuf;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            LOG_WARNING("TUNNEL: setsockopt(SO_RCVBUF) failed: %s", strerror(errno));
        } else {
            LOG_INFO("TUNNEL: set TCP receive buffer to %d bytes", rcvbuf);
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(src_port);
    if ('\0' == tunnel->src_host[0] || 0 == strcmp(tunnel->src_host, "0.0.0.0")) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, tunnel->src_host, &addr.sin_addr);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        pthread_mutex_unlock(&g_tunnel_mutex);
        LOG_ERROR("TUNNEL %u: bind() on port %u failed: %s", tunnel_id, src_port, strerror(errno));
        return;
    }

    tunnel->listen_fd = listen_fd;
    tunnel->is_active = 1;
    pthread_mutex_unlock(&g_tunnel_mutex);

    if (TUNNEL_PROTO_UDP == protocol) {
        /* UDP: Start packet receive thread (no listen/accept needed) */
        udp_ctx_t *udp_ctx = malloc(sizeof(udp_ctx_t));
        if (NULL == udp_ctx) {
            close(listen_fd);
            tunnel->is_active = 0;
            return;
        }
        udp_ctx->tunnel = tunnel;
        udp_ctx->udp_fd = listen_fd;
        memset(udp_ctx->clients, 0, sizeof(udp_ctx->clients));
        pthread_mutex_init(&udp_ctx->clients_mutex, NULL);
        tunnel->udp_ctx = udp_ctx;
        pthread_create(&tunnel->accept_thread, NULL, udp_recv_thread, udp_ctx);
        pthread_detach(tunnel->accept_thread);
    } else {
        /* TCP: Listen and start accept thread */
        if (listen(listen_fd, 16) < 0) {
            close(listen_fd);
            tunnel->is_active = 0;
            LOG_ERROR("TUNNEL %u: listen() failed: %s", tunnel_id, strerror(errno));
            return;
        }

        accept_ctx_t *ctx = malloc(sizeof(accept_ctx_t));
        if (NULL == ctx) {
            close(listen_fd);
            tunnel->is_active = 0;
            return;
        }
        ctx->tunnel = tunnel;
        pthread_create(&tunnel->accept_thread, NULL, src_accept_thread, ctx);
        pthread_detach(tunnel->accept_thread);
    }

    LOG_INFO("TUNNEL %u: listening on %s:%u, forwarding via node %u to %s:%u",
             tunnel_id, tunnel->src_host[0] ? tunnel->src_host : "0.0.0.0",
             src_port, dst_node_id, tunnel->remote_host, remote_port);
}

/* Runs in its own thread so handle_tunnel_conn_open can return immediately
 * without blocking the ganon recv thread on getaddrinfo/connect. */
static void *dst_connect_thread(void *arg) {
    dst_connect_ctx_t *ctx = (dst_connect_ctx_t *)arg;
    uint32_t tunnel_id   = ctx->tunnel_id;
    uint32_t conn_id     = ctx->conn_id;
    uint32_t src_node_id = ctx->src_node_id;
    uint16_t remote_port = ctx->remote_port;
    uint8_t  protocol    = ctx->protocol;
    char     remote_host[256];
    memcpy(remote_host, ctx->remote_host, sizeof(remote_host));
    free(ctx);

    int sock_type = (TUNNEL_PROTO_UDP == protocol) ? SOCK_DGRAM : SOCK_STREAM;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = sock_type;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", remote_port);

    if (0 != getaddrinfo(remote_host, port_str, &hints, &res)) {
        LOG_ERROR("TUNNEL %u conn %u: getaddrinfo(%s) failed", tunnel_id, conn_id, remote_host);
        return NULL;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        LOG_ERROR("TUNNEL %u conn %u: socket() failed", tunnel_id, conn_id);
        return NULL;
    }

    /* Store remote address for UDP sendto() */
    struct sockaddr_in udp_remote_addr;
    socklen_t udp_remote_addr_len = res->ai_addrlen;
    memcpy(&udp_remote_addr, res->ai_addr, sizeof(udp_remote_addr));

    /* TCP: connect() establishes the stream connection.
     * UDP: connect() just installs the remote peer address in the kernel — no packets
     * are sent — but it binds the local ephemeral port up-front so that recvfrom in
     * fwd_thread_func can receive reply datagrams from the moment the thread starts. */
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(fd);
        LOG_ERROR("TUNNEL %u conn %u: connect() to %s:%u failed: %s",
                  tunnel_id, conn_id, remote_host, remote_port, strerror(errno));
        return NULL;
    }
    freeaddrinfo(res);

    if (TUNNEL_PROTO_UDP != protocol) {
        int nodelay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
            LOG_WARNING("TUNNEL %u conn %u: setsockopt(TCP_NODELAY) failed: %s",
                       tunnel_id, conn_id, strerror(errno));
        }
    }

    if (!g_tunnel_running) {
        close(fd);
        return NULL;
    }

    pthread_mutex_lock(&g_tunnel_mutex);
    
    /* Check if connection already exists (duplicate TUNNEL_CONN_OPEN via multi-path) */
    for (int i = 0; i < MAX_DST_CONNS; i++) {
        if (g_dst_conns[i].is_active &&
            g_dst_conns[i].tunnel_id == tunnel_id &&
            g_dst_conns[i].conn_id == conn_id) {
            pthread_mutex_unlock(&g_tunnel_mutex);
            close(fd);
            LOG_INFO("TUNNEL %u conn %u: duplicate CONN_OPEN received, ignoring", tunnel_id, conn_id);
            return NULL;
        }
    }
    
    dst_conn_t *dconn = NULL;
    for (int i = 0; i < MAX_DST_CONNS; i++) {
        if (!g_dst_conns[i].is_active) {
            dconn = &g_dst_conns[i];
            break;
        }
    }
    if (NULL == dconn) {
        pthread_mutex_unlock(&g_tunnel_mutex);
        close(fd);
        LOG_WARNING("TUNNEL: max dst connections reached");
        return NULL;
    }

    dconn->tunnel_id   = tunnel_id;
    dconn->conn_id     = conn_id;
    dconn->src_node_id = src_node_id;
    dconn->fd          = fd;
    dconn->is_active   = 1;
    dconn->is_udp      = (TUNNEL_PROTO_UDP == protocol);
    if (dconn->is_udp) {
        memcpy(&dconn->udp_remote_addr, &udp_remote_addr, sizeof(udp_remote_addr));
        dconn->udp_remote_addr_len = udp_remote_addr_len;
    }
    pthread_mutex_unlock(&g_tunnel_mutex);

    tunnel_ids_payload_t ack;
    ack.tunnel_id = htonl(tunnel_id);
    ack.conn_id   = htonl(conn_id);
    /* Use conn_id as channel_id for per-connection isolation */
    tunnel_send(src_node_id, MSG__TUNNEL_CONN_ACK, conn_id,
                (uint8_t *)&ack, sizeof(ack));

    fwd_ctx_t *fwd = malloc(sizeof(fwd_ctx_t));
    if (NULL == fwd) {
        dconn->is_active = 0;
        close(fd);
        return NULL;
    }
    fwd->fd                     = fd;
    fwd->tunnel_id              = tunnel_id;
    fwd->conn_id                = conn_id;
    fwd->peer_node_id           = src_node_id;
    fwd->p_is_active            = &dconn->is_active;
    fwd->p_slot_conn_id         = (volatile uint32_t *)&dconn->conn_id;
    fwd->p_ack_received         = NULL; /* dst side never waits for ACK */
    fwd->is_udp                 = (TUNNEL_PROTO_UDP == protocol);
    fwd->udp_remote_addr        = udp_remote_addr;
    fwd->udp_remote_addr_len    = udp_remote_addr_len;

    pthread_create(&dconn->fwd_thread, NULL, fwd_thread_func, fwd);
    pthread_detach(dconn->fwd_thread);

    LOG_INFO("TUNNEL %u conn %u: connected to %s:%u", tunnel_id, conn_id, remote_host, remote_port);
    return NULL;
}

static void handle_tunnel_conn_open(uint32_t src_node_id, const uint8_t *data, size_t data_len) {
    if (data_len < sizeof(tunnel_conn_open_payload_t)) {
        LOG_WARNING("TUNNEL_CONN_OPEN: payload too small (%zu)", data_len);
        return;
    }

    const tunnel_conn_open_payload_t *p = (const tunnel_conn_open_payload_t *)data;

    dst_connect_ctx_t *ctx = malloc(sizeof(dst_connect_ctx_t));
    if (NULL == ctx) {
        LOG_ERROR("TUNNEL_CONN_OPEN: malloc failed");
        return;
    }
    ctx->tunnel_id   = ntohl(p->tunnel_id);
    ctx->conn_id     = ntohl(p->conn_id);
    ctx->src_node_id = src_node_id;
    ctx->remote_port = ntohs(p->remote_port);
    ctx->protocol    = p->protocol;
    strncpy(ctx->remote_host, p->remote_host, sizeof(ctx->remote_host) - 1);
    ctx->remote_host[255] = '\0';

    pthread_t t;
    pthread_create(&t, NULL, dst_connect_thread, ctx);
    pthread_detach(t);
}

static void handle_tunnel_conn_ack(const uint8_t *data, size_t data_len) {
    if (data_len < 8) {
        return;
    }
    uint32_t tunnel_id = ntohl(*(const uint32_t *)data);
    uint32_t conn_id   = ntohl(*(const uint32_t *)(data + 4));

    pthread_mutex_lock(&g_tunnel_mutex);
    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        if (!g_src_tunnels[i].is_active || g_src_tunnels[i].tunnel_id != tunnel_id) {
            continue;
        }
        
        /* Handle TCP connections */
        for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
            if (g_src_tunnels[i].conns[j].is_active &&
                g_src_tunnels[i].conns[j].conn_id == conn_id) {
                g_src_tunnels[i].conns[j].ack_received = 1;
                pthread_cond_broadcast(&g_ack_cond);
                pthread_mutex_unlock(&g_tunnel_mutex);
                LOG_INFO("TUNNEL %u conn %u: dst node confirmed connection", tunnel_id, conn_id);
                return;
            }
        }
        
        /* Handle UDP clients */
        if (NULL != g_src_tunnels[i].udp_ctx) {
            udp_ctx_t *uctx = g_src_tunnels[i].udp_ctx;
            pthread_mutex_lock(&uctx->clients_mutex);
            for (int j = 0; j < MAX_UDP_CLIENTS; j++) {
                if (uctx->clients[j].is_active && uctx->clients[j].conn_id == conn_id) {
                    uctx->clients[j].ack_received = 1;
                    LOG_INFO("TUNNEL %u conn %u: dst node confirmed UDP connection (%d queued packets to flush)",
                             tunnel_id, conn_id, uctx->clients[j].pre_ack_count);
                    
                    /* Flush queued pre-ACK packets */
                    LOG_INFO("TUNNEL %u conn %u: starting flush of %d packets",
                             tunnel_id, conn_id, uctx->clients[j].pre_ack_count);
                    for (int k = 0; k < uctx->clients[j].pre_ack_count; k++) {
                        pre_ack_packet_t *pkt = &uctx->clients[j].pre_ack_queue[k];
                        LOG_TRACE("TUNNEL %u conn %u: packet %d - valid=%d, len=%u",
                                 tunnel_id, conn_id, k, pkt->valid, pkt->data_len);
                        if (pkt->valid && pkt->data_len > 0) {
                            /* Build the packet with tunnel_id and conn_id header */
                            uint8_t buf[TUNNEL_BUF_SIZE + 8];
                            uint32_t tid = htonl(tunnel_id);
                            uint32_t cid = htonl(conn_id);
                            memcpy(buf, &tid, 4);
                            memcpy(buf + 4, &cid, 4);
                            memcpy(buf + 8, pkt->data, pkt->data_len);
                            
                            err_t send_rc = tunnel_send(g_src_tunnels[i].dst_node_id, MSG__TUNNEL_DATA, conn_id,
                                        buf, pkt->data_len + 8);
                            if (E__SUCCESS != send_rc) {
                                LOG_WARNING("TUNNEL %u conn %u: failed to flush pre-ACK packet %d/%d (rc=%d)",
                                            tunnel_id, conn_id, k+1, uctx->clients[j].pre_ack_count, send_rc);
                            } else {
                                LOG_TRACE("TUNNEL %u conn %u: flushed pre-ACK packet %d/%d (%u bytes)",
                                          tunnel_id, conn_id, k+1, uctx->clients[j].pre_ack_count, pkt->data_len);
                            }
                            pkt->valid = 0;
                        }
                    }
                    uctx->clients[j].pre_ack_count = 0;
                    
                    pthread_mutex_unlock(&uctx->clients_mutex);
                    pthread_mutex_unlock(&g_tunnel_mutex);
                    return;
                }
            }
            pthread_mutex_unlock(&uctx->clients_mutex);
        }
        break;
    }
    pthread_mutex_unlock(&g_tunnel_mutex);
    LOG_WARNING("TUNNEL %u conn %u: CONN_ACK for unknown connection", tunnel_id, conn_id);
}

static void handle_tunnel_data(const uint8_t *data, size_t data_len) {
    if (data_len < 9) {
        return;
    }
    uint32_t tunnel_id  = ntohl(*(const uint32_t *)data);
    uint32_t conn_id    = ntohl(*(const uint32_t *)(data + 4));
    const uint8_t *payload     = data + 8;
    size_t    payload_len = data_len - 8;

    pthread_mutex_lock(&g_tunnel_mutex);
    int fd = -1;
    int is_udp = 0;
    struct sockaddr_in udp_addr;
    socklen_t udp_addr_len = sizeof(udp_addr);
    int found = 0;

    /* Check source-side tunnels (TCP connections) */
    for (int i = 0; i < MAX_SRC_TUNNELS && !found; i++) {
        if (!g_src_tunnels[i].is_active || g_src_tunnels[i].tunnel_id != tunnel_id) {
            continue;
        }
        /* Check if this is a UDP tunnel - need to find client by conn_id */
        if (TUNNEL_PROTO_UDP == g_src_tunnels[i].protocol) {
            fd = g_src_tunnels[i].listen_fd;
            is_udp = 1;
            /* Look up the UDP client by conn_id to get its address for sendto() */
            if (NULL != g_src_tunnels[i].udp_ctx) {
                pthread_mutex_lock(&g_src_tunnels[i].udp_ctx->clients_mutex);
                for (int k = 0; k < MAX_UDP_CLIENTS; k++) {
                    if (g_src_tunnels[i].udp_ctx->clients[k].is_active &&
                        g_src_tunnels[i].udp_ctx->clients[k].conn_id == conn_id) {
                        memcpy(&udp_addr, &g_src_tunnels[i].udp_ctx->clients[k].addr,
                               sizeof(udp_addr));
                        udp_addr_len = g_src_tunnels[i].udp_ctx->clients[k].addr_len;
                        found = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&g_src_tunnels[i].udp_ctx->clients_mutex);
            }
            if (!found) {
                LOG_DEBUG("TUNNEL %u conn %u: UDP client not found for response", tunnel_id, conn_id);
            }
            break;
        }
        for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
            if (g_src_tunnels[i].conns[j].is_active &&
                g_src_tunnels[i].conns[j].conn_id == conn_id) {
                fd = g_src_tunnels[i].conns[j].fd;
                found = 1;
                break;
            }
        }
    }

    /* Check destination-side connections */
    if (!found) {
        for (int i = 0; i < MAX_DST_CONNS; i++) {
            if (g_dst_conns[i].is_active &&
                g_dst_conns[i].tunnel_id == tunnel_id &&
                g_dst_conns[i].conn_id == conn_id) {
                fd = g_dst_conns[i].fd;
                is_udp = g_dst_conns[i].is_udp;
                if (is_udp) {
                    memcpy(&udp_addr, &g_dst_conns[i].udp_remote_addr, sizeof(udp_addr));
                    udp_addr_len = g_dst_conns[i].udp_remote_addr_len;
                }
                found = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_tunnel_mutex);

    if (!found || fd < 0) {
        LOG_DEBUG("TUNNEL %u conn %u: data for unknown connection, dropping", tunnel_id, conn_id);
        return;
    }

    /* Write with MSG_DONTWAIT to avoid blocking the ganon recv thread.
     * If the remote socket buffer is full the data chunk is dropped.
     * A proper implementation would use a per-connection write queue here. */
    const uint8_t *pos = payload;
    size_t remaining = payload_len;
    while (remaining > 0) {
        ssize_t n;
        if (is_udp) {
            n = sendto(fd, pos, remaining, MSG_NOSIGNAL | MSG_DONTWAIT,
                       (struct sockaddr *)&udp_addr, udp_addr_len);
        } else {
            n = send(fd, pos, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        if (n <= 0) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                LOG_DEBUG("TUNNEL %u conn %u: remote socket buffer full, dropping %zu bytes",
                          tunnel_id, conn_id, remaining);
            } else {
                LOG_WARNING("TUNNEL %u conn %u: send failed: %s",
                            tunnel_id, conn_id, strerror(errno));
            }
            break;
        }
        pos       += n;
        remaining -= (size_t)n;
    }
}

static void handle_tunnel_conn_close(const uint8_t *data, size_t data_len) {
    if (data_len < 8) {
        return;
    }
    uint32_t tunnel_id = ntohl(*(const uint32_t *)data);
    uint32_t conn_id   = ntohl(*(const uint32_t *)(data + 4));

    pthread_mutex_lock(&g_tunnel_mutex);

    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        if (!g_src_tunnels[i].is_active || g_src_tunnels[i].tunnel_id != tunnel_id) {
            continue;
        }
        for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
            if (g_src_tunnels[i].conns[j].is_active &&
                g_src_tunnels[i].conns[j].conn_id == conn_id) {
                g_src_tunnels[i].conns[j].is_active = 0;
                shutdown(g_src_tunnels[i].conns[j].fd, SHUT_RDWR);
                close(g_src_tunnels[i].conns[j].fd);
                break;
            }
        }
        break;
    }

    for (int i = 0; i < MAX_DST_CONNS; i++) {
        if (g_dst_conns[i].is_active &&
            g_dst_conns[i].tunnel_id == tunnel_id &&
            g_dst_conns[i].conn_id == conn_id) {
            g_dst_conns[i].is_active = 0;
            shutdown(g_dst_conns[i].fd, SHUT_RDWR);
            close(g_dst_conns[i].fd);
            break;
        }
    }

    pthread_cond_broadcast(&g_ack_cond); /* wake any fwd thread still waiting for ACK */
    pthread_mutex_unlock(&g_tunnel_mutex);
    LOG_INFO("TUNNEL %u conn %u: closed by peer", tunnel_id, conn_id);
}

static void handle_tunnel_close(const uint8_t *data, size_t data_len) {
    if (data_len < sizeof(tunnel_id_payload_t)) {
        return;
    }
    const tunnel_id_payload_t *p = (const tunnel_id_payload_t *)data;
    uint32_t tunnel_id = ntohl(p->tunnel_id);
    uint32_t flags = ntohl(p->flags);
    int is_force = (flags & TUNNEL_CLOSE_FLAG_FORCE) != 0;

    pthread_mutex_lock(&g_tunnel_mutex);
    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        if (!g_src_tunnels[i].is_active || g_src_tunnels[i].tunnel_id != tunnel_id) {
            continue;
        }

        /* Always close the listening socket to stop accepting new connections */
        close(g_src_tunnels[i].listen_fd);
        g_src_tunnels[i].listen_fd = -1;

        if (is_force) {
            /* Force close: terminate all existing connections immediately */
            g_src_tunnels[i].is_active = 0;
            for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
                if (g_src_tunnels[i].conns[j].is_active) {
                    g_src_tunnels[i].conns[j].is_active = 0;
                    shutdown(g_src_tunnels[i].conns[j].fd, SHUT_RDWR);
                    close(g_src_tunnels[i].conns[j].fd);
                }
            }
            LOG_INFO("TUNNEL %u: force closed (all connections terminated)", tunnel_id);
        } else {
            /* Soft close: mark as soft-closed, existing connections continue */
            g_src_tunnels[i].is_soft_closed = 1;
            int active_count = 0;
            for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
                if (g_src_tunnels[i].conns[j].is_active) {
                    active_count++;
                }
            }
            LOG_INFO("TUNNEL %u: soft closed (listening stopped, %d connections active)",
                     tunnel_id, active_count);
        }
        break;
    }
    pthread_mutex_unlock(&g_tunnel_mutex);
}

/* ---- public API ---- */

void TUNNEL__init(IN int tcp_rcvbuf) {
    memset(g_src_tunnels, 0, sizeof(g_src_tunnels));
    memset(g_dst_conns,   0, sizeof(g_dst_conns));
    g_tunnel_running = 1;
    g_tunnel_tcp_rcvbuf = tcp_rcvbuf;
}

void TUNNEL__destroy(void) {
    g_tunnel_running = 0;

    pthread_mutex_lock(&g_tunnel_mutex);

    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        if (!g_src_tunnels[i].is_active && !g_src_tunnels[i].is_soft_closed) {
            continue;
        }
        g_src_tunnels[i].is_active = 0;
        g_src_tunnels[i].is_soft_closed = 0;
        if (g_src_tunnels[i].listen_fd >= 0) {
            close(g_src_tunnels[i].listen_fd);
            g_src_tunnels[i].listen_fd = -1;
        }
        for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
            if (g_src_tunnels[i].conns[j].is_active) {
                g_src_tunnels[i].conns[j].is_active = 0;
                shutdown(g_src_tunnels[i].conns[j].fd, SHUT_RDWR);
                close(g_src_tunnels[i].conns[j].fd);
            }
        }
    }

    for (int i = 0; i < MAX_DST_CONNS; i++) {
        if (g_dst_conns[i].is_active) {
            g_dst_conns[i].is_active = 0;
            shutdown(g_dst_conns[i].fd, SHUT_RDWR);
            close(g_dst_conns[i].fd);
        }
    }

    pthread_cond_broadcast(&g_ack_cond); /* wake any fwd threads still waiting for ACK */
    pthread_mutex_unlock(&g_tunnel_mutex);
}

void TUNNEL__on_message(IN transport_t *t, IN const protocol_msg_t *msg,
                        IN const uint8_t *data, IN size_t data_len) {
    (void)t;
    uint32_t src_node_id = msg->orig_src_node_id;
    msg_type_t type = (msg_type_t)msg->type;

    switch (type) {
    case MSG__TUNNEL_OPEN:
        handle_tunnel_open(src_node_id, data, data_len);
        break;
    case MSG__TUNNEL_CONN_OPEN:
        handle_tunnel_conn_open(src_node_id, data, data_len);
        break;
    case MSG__TUNNEL_CONN_ACK:
        handle_tunnel_conn_ack(data, data_len);
        break;
    case MSG__TUNNEL_DATA:
        handle_tunnel_data(data, data_len);
        break;
    case MSG__TUNNEL_CONN_CLOSE:
        handle_tunnel_conn_close(data, data_len);
        break;
    case MSG__TUNNEL_CLOSE:
        handle_tunnel_close(data, data_len);
        break;
    default:
        break;
    }
}

void TUNNEL__handle_disconnect(IN uint32_t node_id) {
    pthread_mutex_lock(&g_tunnel_mutex);

    for (int i = 0; i < MAX_DST_CONNS; i++) {
        if (g_dst_conns[i].is_active && g_dst_conns[i].src_node_id == node_id) {
            g_dst_conns[i].is_active = 0;
            shutdown(g_dst_conns[i].fd, SHUT_RDWR);
            close(g_dst_conns[i].fd);
            LOG_INFO("TUNNEL %u conn %u: closed (src node %u disconnected)",
                     g_dst_conns[i].tunnel_id, g_dst_conns[i].conn_id, node_id);
        }
    }

    for (int i = 0; i < MAX_SRC_TUNNELS; i++) {
        if ((!g_src_tunnels[i].is_active && !g_src_tunnels[i].is_soft_closed) ||
            g_src_tunnels[i].dst_node_id != node_id) {
            continue;
        }
        g_src_tunnels[i].is_active = 0;
        g_src_tunnels[i].is_soft_closed = 0;
        if (g_src_tunnels[i].listen_fd >= 0) {
            close(g_src_tunnels[i].listen_fd);
            g_src_tunnels[i].listen_fd = -1;
        }
        for (int j = 0; j < MAX_CONNS_PER_TUNNEL; j++) {
            if (g_src_tunnels[i].conns[j].is_active) {
                g_src_tunnels[i].conns[j].is_active = 0;
                shutdown(g_src_tunnels[i].conns[j].fd, SHUT_RDWR);
                close(g_src_tunnels[i].conns[j].fd);
            }
        }
        LOG_INFO("TUNNEL %u: closed (dst node %u disconnected)",
                 g_src_tunnels[i].tunnel_id, node_id);
    }

    pthread_cond_broadcast(&g_ack_cond); /* wake any fwd threads still waiting for ACK */
    pthread_mutex_unlock(&g_tunnel_mutex);
}
