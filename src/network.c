#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "routing.h"
#include "transport.h"
#include "skin.h"
#include "tunnel.h"
#include "network_caps.h"

#ifdef USE_EPOLL
#include "network_epoll.h"
#endif

network_t g_network;

static void socket_entry_remove_locked(network_t *net, socket_entry_t *entry) {
    if (NULL == net || NULL == entry) {
        return;
    }
    socket_entry_t **prev = &net->clients;
    while (NULL != *prev) {
        if (*prev == entry) {
            *prev = entry->next;
            break;
        }
        prev = &(*prev)->next;
    }
}

static void *socket_thread_func(void *arg) {
    socket_entry_t *entry = (socket_entry_t *)arg;
    network_t *net = entry->net;
    transport_t *t = entry->t;

    if (t->is_incoming) {
        LOG_INFO("Connection from %s:%d (fd=%d)", t->client_ip, t->client_port, t->fd);
    } else {
        LOG_INFO("Connected to %s:%d (fd=%d)", t->client_ip, t->client_port, t->fd);
    }

    /* Handshake is already done by skin->connect or skin->listener_accept. */

    if (NULL != net->connected_cb) {
        net->connected_cb(t);
    }

#ifdef USE_EPOLL
    if (g_net_caps.has_epoll && g_epoll_fd >= 0) {
        int flags = fcntl(t->fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(t->fd, F_SETFL, flags | O_NONBLOCK);
        }
        t->is_nonblocking = 1;
        if (E__SUCCESS == NETWORK__epoll_add(t)) {
            pthread_mutex_lock(&t->epoll_cv_mutex);
            while (!t->epoll_disconnect_flag && net->running) {
                pthread_cond_wait(&t->epoll_cv, &t->epoll_cv_mutex);
            }
            pthread_mutex_unlock(&t->epoll_cv_mutex);
        }
        goto l_cleanup;
    }
#endif

    while (true) {
        protocol_msg_t msg;
        uint8_t *data = NULL;
        err_t rc = TRANSPORT__recv_msg(t, &msg, &data);
        if (E__SUCCESS != rc) {
            break;
        }
        if (NULL != net->message_cb) {
            net->message_cb(t, &msg, data, msg.data_length);
        }
        free(data);
    }

l_cleanup:
    LOG_INFO("Connection %s:%d closed", t->client_ip, t->client_port);

    if (t->is_nonblocking) {
#ifdef USE_EPOLL
        NETWORK__epoll_remove(t);
#endif
    }

    if (NULL != net->disconnected_cb && !t->disconnected_cb_called) {
        t->disconnected_cb_called = 1;
        net->disconnected_cb(t);
    }

    TRANSPORT__destroy(t);
    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_remove_locked(net, entry);
    pthread_mutex_unlock(&net->clients_mutex);
    FREE(entry);
    return NULL;
}

/* ---- Accept thread (one per listener) ----------------------------------- */

static void *accept_thread_func(void *arg) {
    listener_t *listener = (listener_t *)arg;
    network_t  *net      = listener->net;

    LOG_INFO("Accept thread started for %s:%d (skin=%s)",
             listener->addr.ip, listener->addr.port, listener->skin->name);

    while (listener->running && net->running) {
        /* listener_accept blocks until a connection + handshake completes */
        transport_t *t = NULL;
        err_t rc = listener->skin->listener_accept(listener->skin_listener, &t);
        if (E__SUCCESS != rc) {
            if (!net->running || !listener->running) {
                break;
            }
            /* Transient error (EINTR, etc.) — retry */
            continue;
        }
        if (NULL == t) {
            continue;
        }

        socket_entry_t *entry = malloc(sizeof(socket_entry_t));
        if (NULL == entry) {
            LOG_ERROR("Failed to allocate socket entry");
            TRANSPORT__destroy(t);
            continue;
        }
        entry->t    = t;
        entry->net  = net;
        entry->next = NULL;

        pthread_mutex_lock(&net->clients_mutex);
        socket_entry_t *tail = net->clients;
        if (NULL == tail) {
            net->clients = entry;
        } else {
            while (NULL != tail->next) tail = tail->next;
            tail->next = entry;
        }

        if (0 != pthread_create(&entry->thread, NULL, socket_thread_func, entry)) {
            LOG_ERROR("Failed to create client thread");
            socket_entry_remove_locked(net, entry);
            pthread_mutex_unlock(&net->clients_mutex);
            TRANSPORT__destroy(t);
            FREE(entry);
            continue;
        }
        pthread_mutex_unlock(&net->clients_mutex);
    }

    LOG_INFO("Accept thread stopped for %s:%d", listener->addr.ip, listener->addr.port);
    return NULL;
}

/* ---- Outbound connect thread -------------------------------------------- */

typedef struct {
    connect_entry_t entry;
    int             connect_timeout;
    int             reconnect_retries;
    int             reconnect_delay;
    const skin_ops_t *skin;
    network_t       *net;
} connect_thread_arg_t;

static void *connect_and_run_thread(void *arg) {
    connect_thread_arg_t *targ = (connect_thread_arg_t *)arg;
    const char  *ip              = targ->entry.addr.ip;
    int          port            = targ->entry.addr.port;
    int          connect_timeout = targ->connect_timeout;
    int          reconnect_retries = targ->reconnect_retries;
    int          reconnect_delay   = targ->reconnect_delay;
    const skin_ops_t *skin         = targ->skin;
    network_t   *net               = targ->net;
    int retries_remaining = reconnect_retries;

    while (true) {
        transport_t *t = NULL;
        err_t rc = skin->connect(ip, port, connect_timeout, &t);
        if (E__SUCCESS != rc || NULL == t) {
            if (retries_remaining == 0) {
                LOG_WARNING("Failed to connect to %s:%d, giving up", ip, port);
                FREE(arg);
                return NULL;
            }
            if (retries_remaining > 0) retries_remaining--;
            LOG_WARNING("Failed to connect to %s:%d, retrying in %ds...", ip, port, reconnect_delay);
            sleep((unsigned int)reconnect_delay);
            continue;
        }

        socket_entry_t *entry = malloc(sizeof(socket_entry_t));
        if (NULL == entry) {
            LOG_ERROR("Failed to allocate socket entry");
            TRANSPORT__destroy(t);
            FREE(arg);
            return NULL;
        }
        entry->t    = t;
        entry->net  = net;
        entry->next = NULL;

        pthread_mutex_lock(&net->clients_mutex);
        socket_entry_t *tail = net->clients;
        if (NULL == tail) {
            net->clients = entry;
        } else {
            while (NULL != tail->next) tail = tail->next;
            tail->next = entry;
        }
        pthread_mutex_unlock(&net->clients_mutex);

        pthread_t socket_thread;
        if (0 != pthread_create(&socket_thread, NULL, socket_thread_func, entry)) {
            LOG_ERROR("Failed to create socket thread");
            pthread_mutex_lock(&net->clients_mutex);
            socket_entry_remove_locked(net, entry);
            pthread_mutex_unlock(&net->clients_mutex);
            TRANSPORT__destroy(t);
            FREE(entry);
            FREE(arg);
            return NULL;
        }

        pthread_join(socket_thread, NULL);

        LOG_INFO("Connection to %s:%d closed, reconnecting...", ip, port);

        if (retries_remaining == 0) {
            LOG_WARNING("Reconnect retries exhausted for %s:%d", ip, port);
            FREE(arg);
            return NULL;
        }
        if (retries_remaining > 0) retries_remaining--;
    }
}

/* ---- NETWORK__init ------------------------------------------------------- */

err_t NETWORK__init(network_t *net, const args_t *args, int node_id,
                    network_message_cb_t msg_cb,
                    network_disconnected_cb_t disc_cb,
                    network_connected_cb_t conn_cb) {
    err_t rc = E__SUCCESS;

    if (NULL == net || NULL == args) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    memset(net, 0, sizeof(network_t));
    net->connect_count    = args->connect_count;
    net->connect_timeout  = args->connect_timeout;
    net->reconnect_retries = args->reconnect_retries;
    net->reconnect_delay  = args->reconnect_delay;
    net->node_id          = node_id;
    net->default_skin_id  = args->default_skin_id;
    net->message_cb       = msg_cb;
    net->disconnected_cb  = disc_cb;
    net->connected_cb     = conn_cb;

    if (0 != pthread_mutex_init(&net->clients_mutex, NULL)) {
        LOG_ERROR("Failed to initialize mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    /* Allocate listeners array */
    net->listener_count = args->listener_count;
    if (net->listener_count > 0) {
        net->listeners = calloc((size_t)net->listener_count, sizeof(listener_t));
        if (NULL == net->listeners) {
            pthread_mutex_destroy(&net->clients_mutex);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }
    }

    net->running = 1;

    NETWORK__probe_caps();
    NETWORK__log_caps();

#ifdef USE_EPOLL
    if (g_net_caps.has_epoll) {
        rc = NETWORK__epoll_init(net);
        if (E__SUCCESS != rc) {
            LOG_WARNING("Failed to initialize epoll, falling back to thread-per-connection");
            g_net_caps.has_epoll = 0;
            rc = E__SUCCESS;
        }
    }
#endif

    /* Create listeners */
    for (int i = 0; i < net->listener_count; i++) {
        listener_t *li = &net->listeners[i];
        const listener_entry_t *ae = &args->listeners[i];

        uint32_t skin_id = ae->skin_id ? ae->skin_id : args->default_skin_id;
        li->skin = SKIN__by_id(skin_id);
        if (NULL == li->skin) {
            LOG_ERROR("Unknown skin_id %u for listener %d", skin_id, i);
            rc = E__INVALID_ARG_NULL_POINTER;
            goto l_cleanup;
        }
        li->addr    = ae->addr;
        li->net     = net;
        li->running = 1;

        int listen_fd = -1;
        rc = li->skin->listener_create(&li->addr, &li->skin_listener, &listen_fd);
        if (E__SUCCESS != rc) {
            LOG_ERROR("Failed to create listener %d (%s:%d)",
                      i, ae->addr.ip, ae->addr.port);
            goto l_cleanup;
        }
        li->listen_fd = listen_fd;

        if (0 != pthread_create(&li->accept_thread, NULL, accept_thread_func, li)) {
            LOG_ERROR("Failed to create accept thread for listener %d", i);
            li->skin->listener_destroy(li->skin_listener);
            li->skin_listener = NULL;
            FAIL(E__NET__THREAD_CREATE_FAILED);
        }
    }

    /* Create outbound connect threads */
    if (net->connect_count > 0) {
        net->connect_entries = (connect_entry_t *)args->connect_addrs;
        net->connect_threads = malloc((size_t)net->connect_count * sizeof(pthread_t));
        if (NULL == net->connect_threads) {
            LOG_ERROR("Failed to allocate connect threads array");
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }

        for (int i = 0; i < net->connect_count; i++) {
            connect_thread_arg_t *targ = malloc(sizeof(connect_thread_arg_t));
            if (NULL == targ) {
                LOG_ERROR("Failed to allocate connect thread arg");
                continue;
            }
            targ->entry           = args->connect_addrs[i];
            targ->connect_timeout = net->connect_timeout;
            targ->reconnect_retries = net->reconnect_retries;
            targ->reconnect_delay = net->reconnect_delay;
            targ->net             = net;

            /* Resolve skin for this connect entry */
            uint32_t skin_id = targ->entry.skin_id ? targ->entry.skin_id
                                                    : args->default_skin_id;
            targ->skin = SKIN__by_id(skin_id);
            if (NULL == targ->skin) {
                LOG_ERROR("Unknown skin_id %u for connect entry %d", skin_id, i);
                FREE(targ);
                continue;
            }

            if (0 != pthread_create(&net->connect_threads[i], NULL,
                                    connect_and_run_thread, targ)) {
                LOG_ERROR("Failed to create connect thread for %s:%d",
                          targ->entry.addr.ip, targ->entry.addr.port);
                FREE(targ);
                continue;
            }
            net->connect_thread_count++;
        }
    }

l_cleanup:
    if (E__SUCCESS != rc && NULL != net->listeners) {
        /* clean up any listeners already started */
        for (int i = 0; i < net->listener_count; i++) {
            listener_t *li = &net->listeners[i];
            if (NULL != li->skin_listener && NULL != li->skin) {
                li->running = 0;
                if (0 != li->accept_thread) {
                    pthread_join(li->accept_thread, NULL);
                }
                li->skin->listener_destroy(li->skin_listener);
            }
        }
        FREE(net->listeners);
        pthread_mutex_destroy(&net->clients_mutex);
    }
    return rc;
}

/* ---- NETWORK__shutdown --------------------------------------------------- */

err_t NETWORK__shutdown(network_t *net) {
    err_t rc = E__SUCCESS;

    if (NULL == net) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("Shutting down network...");
    net->running = 0;

    /* Stop accept threads */
    for (int i = 0; i < net->listener_count; i++) {
        listener_t *li = &net->listeners[i];
        li->running = 0;
        /* listener_destroy closes the listen_fd which unblocks accept() */
        if (NULL != li->skin && NULL != li->skin_listener) {
            li->skin->listener_destroy(li->skin_listener);
            li->skin_listener = NULL;
        }
        if (0 != li->accept_thread) {
            pthread_join(li->accept_thread, NULL);
            li->accept_thread = 0;
        }
    }
    FREE(net->listeners);
    net->listener_count = 0;

#ifdef USE_EPOLL
    NETWORK__epoll_shutdown();
#endif

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *head = net->clients;
    net->clients = NULL;
    pthread_mutex_unlock(&net->clients_mutex);

    /* Signal all epoll stub threads to exit */
    socket_entry_t *iter = head;
    while (NULL != iter) {
        if (iter->t->is_nonblocking) {
            pthread_mutex_lock(&iter->t->epoll_cv_mutex);
            iter->t->epoll_disconnect_flag = 1;
            pthread_cond_broadcast(&iter->t->epoll_cv);
            pthread_mutex_unlock(&iter->t->epoll_cv_mutex);
        }
        iter = iter->next;
    }

    iter = head;
    while (NULL != iter) {
        socket_entry_t *next = iter->next;
        if (0 != iter->thread) {
            pthread_join(iter->thread, NULL);
        } else {
            TRANSPORT__destroy(iter->t);
            FREE(iter);
        }
        iter = next;
    }

    pthread_mutex_destroy(&net->clients_mutex);

    for (int i = 0; i < net->connect_thread_count; i++) {
        if (0 != net->connect_threads[i]) {
            pthread_detach(net->connect_threads[i]);
        }
    }
    FREE(net->connect_threads);

    LOG_INFO("Network shutdown complete");

l_cleanup:
    return rc;
}

/* ---- Accessors ---------------------------------------------------------- */

transport_t *NETWORK__get_transport(network_t *net, uint32_t node_id) {
    if (NULL == net) return NULL;

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *entry = net->clients;
    while (NULL != entry) {
        if (entry->t->node_id == node_id) {
            pthread_mutex_unlock(&net->clients_mutex);
            return entry->t;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);
    return NULL;
}

void NETWORK__close_transport(network_t *net, transport_t *t) {
    (void)net;
    if (NULL == t) return;
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
}

/* ---- NETWORK__connect_to_peer ------------------------------------------- */

err_t NETWORK__connect_to_peer(network_t *net, const char *ip, int port,
                                const skin_ops_t *skin,
                                int *status, uint32_t *error_code, int *out_fd) {
    err_t rc = E__SUCCESS;

    if (NULL != out_fd) *out_fd = -1;

    if (NULL == net || NULL == ip || NULL == status || NULL == error_code) {
        if (NULL != status) *status = CONNECT_STATUS_ERROR;
        if (NULL != error_code) *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    *status     = CONNECT_STATUS_SUCCESS;
    *error_code = 0;

    if (NULL == skin) {
        skin = SKIN__default();
    }
    if (NULL == skin) {
        LOG_ERROR("No skin available for connect_to_peer");
        *status     = CONNECT_STATUS_ERROR;
        *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("Connecting to peer %s:%d (skin=%s)", ip, port, skin->name);

    transport_t *t = NULL;
    rc = skin->connect(ip, port, net->connect_timeout, &t);
    if (E__SUCCESS != rc || NULL == t) {
        LOG_WARNING("Failed to connect to %s:%d: rc=%d", ip, port, (int)rc);
        *status     = CONNECT_STATUS_ERROR;
        *error_code = (uint32_t)rc;
        /* Map common errors to status codes */
        if (E__NET__SOCKET_CONNECT_FAILED == rc) {
            *status = CONNECT_STATUS_REFUSED;
        }
        FAIL(rc);
    }

    if (NULL != out_fd) {
        *out_fd = t->fd;
    }

    socket_entry_t *entry = malloc(sizeof(socket_entry_t));
    if (NULL == entry) {
        LOG_ERROR("Failed to allocate socket entry");
        TRANSPORT__destroy(t);
        *status     = CONNECT_STATUS_ERROR;
        *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    entry->t    = t;
    entry->net  = net;
    entry->next = NULL;

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *tail = net->clients;
    if (NULL == tail) {
        net->clients = entry;
    } else {
        while (NULL != tail->next) tail = tail->next;
        tail->next = entry;
    }
    pthread_mutex_unlock(&net->clients_mutex);

    if (0 != pthread_create(&entry->thread, NULL, socket_thread_func, entry)) {
        LOG_ERROR("Failed to create socket thread");
        pthread_mutex_lock(&net->clients_mutex);
        socket_entry_t **cur = &net->clients;
        while (*cur != entry) cur = &(*cur)->next;
        *cur = entry->next;
        pthread_mutex_unlock(&net->clients_mutex);
        TRANSPORT__destroy(t);
        FREE(entry);
        *status     = CONNECT_STATUS_ERROR;
        *error_code = E__NET__THREAD_CREATE_FAILED;
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    LOG_INFO("Connected to peer %s:%d", ip, port);

l_cleanup:
    return rc;
}

/* ---- NETWORK__disconnect_from_peer -------------------------------------- */

err_t NETWORK__disconnect_from_peer(network_t *net, uint32_t node_id,
                                     int *status, uint32_t *error_code) {
    err_t rc = E__SUCCESS;

    if (NULL == net || NULL == status || NULL == error_code) {
        if (NULL != status) *status = DISCONNECT_STATUS_ERROR;
        if (NULL != error_code) *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    *status     = DISCONNECT_STATUS_SUCCESS;
    *error_code = 0;

    LOG_INFO("Disconnecting from peer %u", node_id);

    transport_t *t = NETWORK__get_transport(net, node_id);
    if (NULL == t) {
        LOG_WARNING("No direct connection to node %u", node_id);
        *status     = DISCONNECT_STATUS_NOT_CONNECTED;
        *error_code = E__ROUTING__NODE_NOT_FOUND;
        FAIL(E__ROUTING__NODE_NOT_FOUND);
    }

    NETWORK__close_transport(net, t);

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t **current = &net->clients;
    while (NULL != *current) {
        if ((*current)->t == t) {
            socket_entry_t *to_remove = *current;
            *current = (*current)->next;
            pthread_join(to_remove->thread, NULL);
            TRANSPORT__destroy(to_remove->t);
            FREE(to_remove);
            break;
        }
        current = &(*current)->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);

    ROUTING__handle_disconnect(node_id);
    TUNNEL__handle_disconnect(node_id);

    LOG_INFO("Disconnected from peer %u", node_id);

l_cleanup:
    return rc;
}
