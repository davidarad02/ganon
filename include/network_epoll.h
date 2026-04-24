#ifndef GANON_NETWORK_EPOLL_H
#define GANON_NETWORK_EPOLL_H

#include "network.h"
#include "transport.h"

#ifdef USE_EPOLL

/* Global epoll instance and thread */
extern int g_epoll_fd;
extern pthread_t g_epoll_thread;

/* Start/stop the epoll event loop. */
err_t NETWORK__epoll_init(IN network_t *net);
void NETWORK__epoll_shutdown(void);

/* Add/remove a transport from epoll.  Transport fd must be non-blocking. */
err_t NETWORK__epoll_add(IN transport_t *t);
void NETWORK__epoll_remove(IN transport_t *t);

/* Enable EPOLLOUT for a transport (called when data is enqueued). */
void NETWORK__epoll_enable_out(IN transport_t *t);

#endif /* USE_EPOLL */

#endif /* GANON_NETWORK_EPOLL_H */
