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
#include "transport.h"
#include "tunnel.h"
#include "network_caps.h"

#ifdef USE_EPOLL
#include "network_epoll.h"
#endif

network_t g_network;

static int create_listen_socket(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > fd) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        LOG_ERROR("Failed to set SO_REUSEADDR: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "*")) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (0 == inet_pton(AF_INET, ip, &addr.sin_addr)) {
            LOG_ERROR("Invalid listen IP: %s", ip);
            close(fd);
            return -1;
        }
    }

    if (0 != bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        LOG_ERROR("Failed to bind to %s:%d: %s", ip, port, strerror(errno));
        close(fd);
        return -1;
    }

    if (0 != listen(fd, SOMAXCONN)) {
        LOG_ERROR("Failed to listen on %s:%d: %s", ip, port, strerror(errno));
        close(fd);
        return -1;
    }

    LOG_INFO("Listening on %s:%d", ip, port);
    return fd;
}

static int connect_to_addr(const char *ip, int port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > fd) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (-1 == flags) {
        close(fd);
        return -1;
    }
    if (-1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (0 == inet_pton(AF_INET, ip, &addr.sin_addr)) {
        struct hostent *he = gethostbyname(ip);
        if (NULL == he) {
            LOG_ERROR("Failed to resolve host: %s", ip);
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    LOG_INFO("Connecting to %s:%d...", ip, port);
    if (0 != connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        if (EINPROGRESS == errno) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);
            struct timeval tv;
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;
            int sel = select(fd + 1, NULL, &write_fds, NULL, &tv);
            if (0 > sel) {
                LOG_WARNING("Connect to %s:%d timed out", ip, port);
                close(fd);
                return -1;
            } else if (0 == sel) {
                LOG_WARNING("Connect to %s:%d timed out after %ds", ip, port, timeout_sec);
                close(fd);
                return -1;
            }

            int err = 0;
            socklen_t errlen = sizeof(err);
            if (0 != getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen)) {
                LOG_WARNING("Connect to %s:%d failed: getsockopt error", ip, port);
                close(fd);
                return -1;
            }
            if (0 != err) {
                LOG_WARNING("Connect to %s:%d failed: %s", ip, port, strerror(err));
                close(fd);
                return -1;
            }
        } else {
            LOG_WARNING("Failed to connect to %s:%d: %s", ip, port, strerror(errno));
            close(fd);
            return -1;
        }
    }

    int nodelay = 1;
    if (0 != setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay))) {
        LOG_WARNING("Failed to set TCP_NODELAY on connected socket: %s", strerror(errno));
    }

    if (-1 == fcntl(fd, F_SETFL, flags)) {
        close(fd);
        return -1;
    }

    return fd;
}

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

    /* Transport-layer encryption handshake MUST complete before any
     * protocol traffic (including NODE_INIT sent by connected_cb). */
    {
        err_t enc_rc = TRANSPORT__do_handshake(t, !t->is_incoming);
        if (E__SUCCESS != enc_rc) {
            LOG_ERROR("Encryption handshake failed on fd=%d, closing connection", t->fd);
            goto l_cleanup;
        }
    }

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

static void *accept_thread_func(void *arg) {
    network_t *net = (network_t *)arg;
    int listen_fd = net->listen_fd;

    LOG_INFO("Accept thread started");

    while (net->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(listen_fd + 1, &read_fds, NULL, NULL, &tv);
        if (0 > ready) {
            if (EINTR == errno) {
                continue;
            }
            LOG_ERROR("Select failed: %s", strerror(errno));
            continue;
        }
        if (0 == ready) {
            continue;
        }
        if (!FD_ISSET(listen_fd, &read_fds)) {
            continue;
        }

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (0 > client_fd) {
            if (EINTR == errno) {
                continue;
            }
            LOG_ERROR("Accept failed: %s", strerror(errno));
            continue;
        }

        int nodelay = 1;
        if (0 != setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay))) {
            LOG_WARNING("Failed to set TCP_NODELAY on accepted socket: %s", strerror(errno));
        }

        socket_entry_t *entry = malloc(sizeof(socket_entry_t));
        if (NULL == entry) {
            LOG_ERROR("Failed to allocate socket entry");
            close(client_fd);
            continue;
        }

        entry->t = TRANSPORT__create(client_fd);
        if (NULL == entry->t) {
            LOG_ERROR("Failed to create transport for fd %d", client_fd);
            close(client_fd);
            FREE(entry);
            continue;
        }
        entry->t->is_incoming = 1;
        inet_ntop(AF_INET, &client_addr.sin_addr, entry->t->client_ip, INET_ADDRSTRLEN);
        entry->t->client_port = ntohs(client_addr.sin_port);
        entry->net = net;
        entry->next = NULL;

        if (0 != pthread_mutex_lock(&net->clients_mutex)) {
            LOG_ERROR("Failed to lock mutex");
            close(client_fd);
            FREE(entry);
            continue;
        }

        socket_entry_t *tail = net->clients;
        if (NULL == tail) {
            net->clients = entry;
        } else {
            while (NULL != tail->next) {
                tail = tail->next;
            }
            tail->next = entry;
        }

        if (0 != pthread_create(&entry->thread, NULL, socket_thread_func, entry)) {
            LOG_ERROR("Failed to create client thread");
            close(client_fd);
            FREE(entry);
            pthread_mutex_unlock(&net->clients_mutex);
            continue;
        }

        pthread_mutex_unlock(&net->clients_mutex);
    }

    LOG_INFO("Accept thread stopped");
    return NULL;
}

typedef struct connect_thread_arg {
    addr_t addr;
    int connect_timeout;
    int reconnect_retries;
    int reconnect_delay;
    network_t *net;
} connect_thread_arg_t;

static void *connect_and_run_thread(void *arg) {
    connect_thread_arg_t *targ = (connect_thread_arg_t *)arg;
    char *ip = targ->addr.ip;
    int port = targ->addr.port;
    int connect_timeout = targ->connect_timeout;
    int reconnect_retries = targ->reconnect_retries;
    int reconnect_delay = targ->reconnect_delay;
    network_t *net = targ->net;
    int retries_remaining = reconnect_retries;

    while (true) {
        int fd = connect_to_addr(ip, port, connect_timeout);
        if (0 > fd) {
            if (retries_remaining == 0) {
                LOG_WARNING("Failed to connect to %s:%d, giving up", ip, port);
                FREE(arg);
                return NULL;
            }
            if (retries_remaining > 0) {
                retries_remaining--;
            }
            LOG_WARNING("Failed to connect to %s:%d, retrying in %ds...", ip, port, reconnect_delay);
            sleep((unsigned int)reconnect_delay);
            continue;
        }

        socket_entry_t *entry = malloc(sizeof(socket_entry_t));
        if (NULL == entry) {
            LOG_ERROR("Failed to allocate socket entry");
            close(fd);
            FREE(arg);
            return NULL;
        }

        entry->t = TRANSPORT__create(fd);
        if (NULL == entry->t) {
            LOG_ERROR("Failed to create transport for fd %d", fd);
            close(fd);
            FREE(entry);
            FREE(arg);
            return NULL;
        }
        entry->t->is_incoming = 0;
        strncpy(entry->t->client_ip, ip, INET_ADDRSTRLEN - 1);
        entry->t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
        entry->t->client_port = port;
        entry->net = net;
        entry->next = NULL;

        if (0 != pthread_mutex_lock(&net->clients_mutex)) {
            LOG_ERROR("Failed to lock mutex");
            close(fd);
            FREE(entry);
            FREE(arg);
            return NULL;
        }

        socket_entry_t *tail = net->clients;
        if (NULL == tail) {
            net->clients = entry;
        } else {
            while (NULL != tail->next) {
                tail = tail->next;
            }
            tail->next = entry;
        }

        pthread_mutex_unlock(&net->clients_mutex);

        pthread_t socket_thread;
        if (0 != pthread_create(&socket_thread, NULL, socket_thread_func, entry)) {
            LOG_ERROR("Failed to create socket thread");
            pthread_mutex_lock(&net->clients_mutex);
            socket_entry_remove_locked(net, entry);
            pthread_mutex_unlock(&net->clients_mutex);
            close(fd);
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
        if (retries_remaining > 0) {
            retries_remaining--;
        }
    }
}

err_t NETWORK__init(OUT network_t *net, IN const args_t *args, IN int node_id, IN network_message_cb_t msg_cb, IN network_disconnected_cb_t disc_cb, IN network_connected_cb_t conn_cb) {
    err_t rc = E__SUCCESS;

    if (NULL == net || NULL == args) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    memset(net, 0, sizeof(network_t));
    net->listen_addr = args->listen_addr;
    net->connect_addrs = (addr_t *)args->connect_addrs;
    net->connect_count = args->connect_count;
    net->connect_timeout = args->connect_timeout;
    net->reconnect_retries = args->reconnect_retries;
    net->reconnect_delay = args->reconnect_delay;
    net->node_id = node_id;
    net->message_cb = msg_cb;
    net->disconnected_cb = disc_cb;
    net->connected_cb = conn_cb;

    if (0 != pthread_mutex_init(&net->clients_mutex, NULL)) {
        LOG_ERROR("Failed to initialize mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    net->listen_fd = create_listen_socket(net->listen_addr.ip, net->listen_addr.port);
    if (0 > net->listen_fd) {
        pthread_mutex_destroy(&net->clients_mutex);
        FAIL(E__NET__SOCKET_BIND_FAILED);
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

    if (0 != pthread_create(&net->accept_thread, NULL, accept_thread_func, net)) {
        LOG_ERROR("Failed to create accept thread");
        close(net->listen_fd);
        pthread_mutex_destroy(&net->clients_mutex);
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    if (net->connect_count > 0) {
        net->connect_threads = malloc((size_t)net->connect_count * sizeof(pthread_t));
        if (NULL == net->connect_threads) {
            LOG_ERROR("Failed to allocate connect threads array");
            net->running = 0;
            pthread_join(net->accept_thread, NULL);
            close(net->listen_fd);
            pthread_mutex_destroy(&net->clients_mutex);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }

        for (int i = 0; i < net->connect_count; i++) {
            connect_thread_arg_t *targ = malloc(sizeof(connect_thread_arg_t));
            if (NULL == targ) {
                LOG_ERROR("Failed to allocate connect thread arg");
                continue;
            }
            targ->addr = net->connect_addrs[i];
            targ->connect_timeout = net->connect_timeout;
            targ->reconnect_retries = net->reconnect_retries;
            targ->reconnect_delay = net->reconnect_delay;
            targ->net = net;

            if (0 != pthread_create(&net->connect_threads[i], NULL, connect_and_run_thread, targ)) {
                LOG_ERROR("Failed to create connect thread for %s:%d", targ->addr.ip, targ->addr.port);
                FREE(targ);
                continue;
            }
            net->connect_thread_count++;
        }
    }

l_cleanup:
    return rc;
}

err_t NETWORK__shutdown(network_t *net) {
    err_t rc = E__SUCCESS;

    if (NULL == net) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    LOG_INFO("Shutting down network...");
    net->running = 0;

    if (0 != net->accept_thread) {
        pthread_join(net->accept_thread, NULL);
    }

    if (0 != net->listen_fd) {
        close(net->listen_fd);
    }

#ifdef USE_EPOLL
    NETWORK__epoll_shutdown();
#endif

    if (0 != pthread_mutex_lock(&net->clients_mutex)) {
        LOG_ERROR("Failed to lock mutex during shutdown");
        FAIL(E__NET__INVALID_SOCKET);
    }

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

    /* Join all client threads.  Each socket_thread_func frees its own
     * transport and entry in l_cleanup, so we must NOT touch them after
     * joining.  Capture next before joining in case the thread frees iter. */
    iter = head;
    while (NULL != iter) {
        socket_entry_t *next = iter->next;
        if (0 != iter->thread) {
            pthread_join(iter->thread, NULL);
            /* thread already called TRANSPORT__destroy(iter->t) and FREE(iter) */
        } else {
            /* thread was never created — clean up manually */
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

transport_t *NETWORK__get_transport(network_t *net, uint32_t node_id) {
    if (NULL == net) {
        return NULL;
    }

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
    if (NULL == t) {
        return;
    }
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
}

err_t NETWORK__connect_to_peer(network_t *net, const char *ip, int port,
                                int *status, uint32_t *error_code, int *out_fd) {
    err_t rc = E__SUCCESS;
    int sock_fd = -1;

    if (NULL != out_fd) {
        *out_fd = -1;
    }

    if (NULL == net || NULL == ip || NULL == status || NULL == error_code) {
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    *status = CONNECT_STATUS_SUCCESS;
    *error_code = 0;
    
    LOG_INFO("Connecting to peer %s:%d", ip, port);
    
    /* Create socket */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sock_fd) {
        LOG_ERROR("Socket creation failed: %s", strerror(errno));
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__NET__SOCKET_CREATE_FAILED;
        FAIL(E__NET__SOCKET_CREATE_FAILED);
    }
    
    /* Set connect timeout */
    struct timeval tv;
    tv.tv_sec = net->connect_timeout;
    tv.tv_usec = 0;
    if (0 > setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
        LOG_WARNING("Failed to set send timeout: %s", strerror(errno));
    }
    
    /* Resolve address */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    
    if (1 != inet_pton(AF_INET, ip, &addr.sin_addr)) {
        LOG_ERROR("Invalid IP address: %s", ip);
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__ARGS__INVALID_FORMAT;
        FAIL(E__ARGS__INVALID_FORMAT);
    }
    
    /* Connect */
    if (0 > connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr))) {
        if (ETIMEDOUT == errno || EINPROGRESS == errno) {
            LOG_WARNING("Connection to %s:%d timed out", ip, port);
            *status = CONNECT_STATUS_TIMEOUT;
            *error_code = (uint32_t)errno;
        } else if (ECONNREFUSED == errno) {
            LOG_WARNING("Connection to %s:%d refused", ip, port);
            *status = CONNECT_STATUS_REFUSED;
            *error_code = (uint32_t)errno;
        } else {
            LOG_ERROR("Connection to %s:%d failed: %s", ip, port, strerror(errno));
            *status = CONNECT_STATUS_ERROR;
            *error_code = (uint32_t)errno;
        }
        close(sock_fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }
    
    /* Create socket entry */
    socket_entry_t *entry = malloc(sizeof(socket_entry_t));
    if (NULL == entry) {
        LOG_ERROR("Failed to allocate socket entry");
        close(sock_fd);
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    
    entry->t = TRANSPORT__create(sock_fd);
    if (NULL == entry->t) {
        LOG_ERROR("Failed to create transport");
        close(sock_fd);
        FREE(entry);
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__NET__INVALID_SOCKET;
        FAIL(E__NET__INVALID_SOCKET);
    }
    
    entry->t->is_incoming = 0;
    strncpy(entry->t->client_ip, ip, INET_ADDRSTRLEN - 1);
    entry->t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    entry->t->client_port = port;
    entry->net = net;
    entry->next = NULL;
    
    /* Add to clients list */
    if (0 != pthread_mutex_lock(&net->clients_mutex)) {
        LOG_ERROR("Failed to lock mutex");
        TRANSPORT__destroy(entry->t);
        FREE(entry);
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__NET__THREAD_CREATE_FAILED;
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }
    
    socket_entry_t *tail = net->clients;
    if (NULL == tail) {
        net->clients = entry;
    } else {
        while (NULL != tail->next) {
            tail = tail->next;
        }
        tail->next = entry;
    }
    
    pthread_mutex_unlock(&net->clients_mutex);
    
    /* Create thread */
    if (0 != pthread_create(&entry->thread, NULL, socket_thread_func, entry)) {
        LOG_ERROR("Failed to create socket thread");
        pthread_mutex_lock(&net->clients_mutex);
        /* Remove from list */
        socket_entry_t **current = &net->clients;
        while (*current != entry) {
            current = &(*current)->next;
        }
        *current = entry->next;
        pthread_mutex_unlock(&net->clients_mutex);
        
        TRANSPORT__destroy(entry->t);
        FREE(entry);
        *status = CONNECT_STATUS_ERROR;
        *error_code = E__NET__THREAD_CREATE_FAILED;
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }
    
    LOG_INFO("Connected to peer %s:%d", ip, port);

    if (NULL != out_fd) {
        *out_fd = sock_fd;
    }

l_cleanup:
    return rc;
}

err_t NETWORK__disconnect_from_peer(network_t *net, uint32_t node_id,
                                     int *status, uint32_t *error_code) {
    err_t rc = E__SUCCESS;
    
    if (NULL == net || NULL == status || NULL == error_code) {
        *status = DISCONNECT_STATUS_ERROR;
        *error_code = E__INVALID_ARG_NULL_POINTER;
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    
    *status = DISCONNECT_STATUS_SUCCESS;
    *error_code = 0;
    
    LOG_INFO("Disconnecting from peer %u", node_id);
    
    /* Find the transport */
    transport_t *t = NETWORK__get_transport(net, node_id);
    if (NULL == t) {
        LOG_WARNING("No direct connection to node %u", node_id);
        *status = DISCONNECT_STATUS_NOT_CONNECTED;
        *error_code = E__ROUTING__NODE_NOT_FOUND;
        FAIL(E__ROUTING__NODE_NOT_FOUND);
    }
    
    /* Close the transport */
    NETWORK__close_transport(net, t);
    
    /* Remove from clients list */
    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t **current = &net->clients;
    while (*current != NULL) {
        if ((*current)->t == t) {
            socket_entry_t *to_remove = *current;
            *current = (*current)->next;
            
            /* Wait for thread to finish */
            pthread_join(to_remove->thread, NULL);
            
            TRANSPORT__destroy(to_remove->t);
            FREE(to_remove);
            break;
        }
        current = &(*current)->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);
    
    /* Update routing */
    ROUTING__handle_disconnect(node_id);
    TUNNEL__handle_disconnect(node_id);
    
    LOG_INFO("Disconnected from peer %u", node_id);
    
l_cleanup:
    return rc;
}
