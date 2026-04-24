#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include "common.h"
#include "logging.h"
#include "network_epoll.h"
#include "transport.h"

#ifdef USE_EPOLL

int g_epoll_fd = -1;
pthread_t g_epoll_thread = 0;

static void *epoll_thread_func(void *arg) {
    network_t *net = (network_t *)arg;
    struct epoll_event events[64];

    while (net->running) {
        int nfds = epoll_wait(g_epoll_fd, events, 64, 500);
        if (0 > nfds) {
            if (EINTR == errno) continue;
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            transport_t *t = (transport_t *)events[i].data.ptr;
            uint32_t ev    = events[i].events;

            if (NULL == t) continue;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                goto handle_disconnect;
            }

            if (ev & EPOLLIN) {
                err_t read_rc = E__SUCCESS;
                if (NULL != t->skin && NULL != t->skin->on_readable) {
                    read_rc = t->skin->on_readable(t, net->message_cb);
                }
                if (E__SUCCESS != read_rc) {
                    goto handle_disconnect;
                }
            }

            if (ev & EPOLLOUT) {
                int would_block = 0;
                err_t drain_rc = E__SUCCESS;
                if (NULL != t->skin && NULL != t->skin->on_writable) {
                    drain_rc = t->skin->on_writable(t, &would_block);
                }
                if (E__SUCCESS != drain_rc) {
                    goto handle_disconnect;
                }
            }

            continue;

        handle_disconnect:
            NETWORK__epoll_remove(t);

            pthread_mutex_lock(&t->epoll_cv_mutex);
            if (!t->disconnected_cb_called) {
                t->disconnected_cb_called = 1;
                if (NULL != net->disconnected_cb) {
                    net->disconnected_cb(t);
                }
            }
            t->epoll_disconnect_flag = 1;
            pthread_cond_broadcast(&t->epoll_cv);
            pthread_mutex_unlock(&t->epoll_cv_mutex);
        }
    }

    return NULL;
}

err_t NETWORK__epoll_init(network_t *net) {
    err_t rc = E__SUCCESS;

    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (0 > g_epoll_fd) {
        g_epoll_fd = epoll_create(1);
        if (0 > g_epoll_fd) {
            LOG_ERROR("epoll_create failed: %s", strerror(errno));
            FAIL(E__NET__SOCKET_CREATE_FAILED);
        }
    }

    if (0 != pthread_create(&g_epoll_thread, NULL, epoll_thread_func, net)) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
        LOG_ERROR("Failed to create epoll thread");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    LOG_INFO("Epoll event loop started (fd=%d)", g_epoll_fd);

l_cleanup:
    return rc;
}

void NETWORK__epoll_shutdown(void) {
    if (0 != g_epoll_thread) {
        pthread_join(g_epoll_thread, NULL);
        g_epoll_thread = 0;
    }
    if (g_epoll_fd >= 0) {
        close(g_epoll_fd);
        g_epoll_fd = -1;
    }
    LOG_INFO("Epoll event loop stopped");
}

err_t NETWORK__epoll_add(transport_t *t) {
    err_t rc = E__SUCCESS;

    if (NULL == t || 0 > t->fd) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    struct epoll_event ee;
    ee.events   = EPOLLIN;
    ee.data.ptr = t;

    if (0 != epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, t->fd, &ee)) {
        LOG_ERROR("epoll_ctl ADD failed for fd=%d: %s", t->fd, strerror(errno));
        FAIL(E__NET__SOCKET_SETOPT_FAILED);
    }

    LOG_DEBUG("Added fd=%d to epoll (node_id=%u)", t->fd, t->node_id);

l_cleanup:
    return rc;
}

void NETWORK__epoll_remove(transport_t *t) {
    if (NULL == t || 0 > t->fd || 0 > g_epoll_fd) return;
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, t->fd, NULL);
}

void NETWORK__epoll_enable_out(transport_t *t) {
    if (NULL == t || 0 > t->fd || 0 > g_epoll_fd) return;
    struct epoll_event ee;
    ee.events   = EPOLLIN | EPOLLOUT;
    ee.data.ptr = t;
    epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, t->fd, &ee);
}

#endif /* USE_EPOLL */
