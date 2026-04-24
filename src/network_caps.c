#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#include <time.h>

#include "common.h"
#include "logging.h"
#include "network_caps.h"

network_caps_t g_net_caps = {0};

/* Linux syscall numbers for x86_64.  These are stable ABI. */
#ifndef SYS_epoll_create1
#define SYS_epoll_create1 291
#endif
#ifndef SYS_eventfd2
#define SYS_eventfd2 290
#endif
#ifndef SYS_signalfd4
#define SYS_signalfd4 289
#endif
#ifndef SYS_timerfd_create
#define SYS_timerfd_create 283
#endif
#ifndef SYS_sendmmsg
#define SYS_sendmmsg 307
#endif
#ifndef SYS_recvmmsg
#define SYS_recvmmsg 337
#endif

void NETWORK__probe_caps(void) {
    /* epoll: 2.5.44+.  Try epoll_create1 first, fall back to epoll_create. */
    {
        int fd = (int)syscall(SYS_epoll_create1, 0);
        if (fd >= 0) {
            close(fd);
            g_net_caps.has_epoll = 1;
        } else {
            fd = (int)syscall(SYS_epoll_create, 1);
            if (fd >= 0) {
                close(fd);
                g_net_caps.has_epoll = 1;
            }
        }
    }

    /* eventfd: 2.6.22+ */
    {
        int fd = (int)syscall(SYS_eventfd2, 0, 0);
        if (fd >= 0) {
            close(fd);
            g_net_caps.has_eventfd = 1;
        }
    }

    /* sendmmsg: 2.6.34+ */
    {
        int r = (int)syscall(SYS_sendmmsg, -1, NULL, 0, 0);
        g_net_caps.has_sendmmsg = (r == -1 && errno != ENOSYS);
    }

    /* recvmmsg: 2.6.34+ */
    {
        int r = (int)syscall(SYS_recvmmsg, -1, NULL, 0, 0, NULL);
        g_net_caps.has_recvmmsg = (r == -1 && errno != ENOSYS);
    }

    /* signalfd: 2.6.22+ */
    {
        sigset_t mask;
        sigemptyset(&mask);
        int fd = (int)syscall(SYS_signalfd4, -1, &mask, sizeof(mask), 0);
        g_net_caps.has_signalfd = (fd >= 0 || errno != ENOSYS);
        if (fd >= 0) close(fd);
    }

    /* timerfd: 2.6.25+ */
    {
        int fd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
        if (fd >= 0) {
            close(fd);
            g_net_caps.has_timerfd = 1;
        }
    }

    /* SO_BUSY_POLL: we cannot probe without a socket, so we leave it
     * at 0 and let the user enable via CLI if desired. */
    g_net_caps.has_busy_poll = 0;
}

void NETWORK__log_caps(void) {
    LOG_INFO("Network capabilities: epoll=%d sendmmsg=%d recvmmsg=%d eventfd=%d signalfd=%d timerfd=%d",
             g_net_caps.has_epoll, g_net_caps.has_sendmmsg, g_net_caps.has_recvmmsg,
             g_net_caps.has_eventfd, g_net_caps.has_signalfd, g_net_caps.has_timerfd);
}
