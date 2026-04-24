#ifndef GANON_NETWORK_CAPS_H
#define GANON_NETWORK_CAPS_H

#include "common.h"

/* Network I/O capabilities detected at runtime via raw syscalls.
 * These are purely local optimizations; none change the wire format. */
typedef struct {
    int has_epoll;          /* epoll_create1 or epoll_create available */
    int has_sendmmsg;       /* SYS_sendmmsg (Linux 2.6.34+) */
    int has_recvmmsg;       /* SYS_recvmmsg (Linux 2.6.34+) */
    int has_eventfd;        /* SYS_eventfd2 (Linux 2.6.22+) */
    int has_signalfd;       /* SYS_signalfd4 (Linux 2.6.22+) */
    int has_timerfd;        /* SYS_timerfd_create (Linux 2.6.25+) */
    int has_busy_poll;      /* SO_BUSY_POLL (Linux 3.11+) */
} network_caps_t;

extern network_caps_t g_net_caps;

/* Probe kernel at runtime. Safe to call multiple times. */
void NETWORK__probe_caps(void);

/* Log detected capabilities at INFO level. */
void NETWORK__log_caps(void);

#endif /* #ifndef GANON_NETWORK_CAPS_H */
