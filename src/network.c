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
#include <pthread.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "transport.h"

int g_node_id = -1;

static err_t send_raw(int fd, const uint8_t *buf, size_t len) {
    err_t rc = E__SUCCESS;

    LOG_TRACE("send_raw called: fd=%d, len=%zu", fd, len);

    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t n = send(fd, buf + total_sent, len - total_sent, 0);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("send failed on fd %d: %s", fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        LOG_TRACE("send returned %zd on fd %d", n, fd);
        total_sent += (size_t)n;
    }

l_cleanup:
    return rc;
}

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

void NETWORK__set_send_fn(network_t *net, network_send_fn_t fn, void *ctx) {
    if (NULL == net) {
        return;
    }
    net->send_fn = fn;
    net->send_ctx = ctx;
}

err_t NETWORK__send_to(network_t *net, uint32_t node_id, const uint8_t *buf, size_t len) {
    if (NULL == net || NULL == buf) {
        return E__INVALID_ARG_NULL_POINTER;
    }

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *entry = net->clients;

    if (node_id == 0) {
        while (NULL != entry) {
            if (entry->t->fd >= 0) {
                send_raw(entry->t->fd, buf, len);
            }
            entry = entry->next;
        }
        pthread_mutex_unlock(&net->clients_mutex);
        return E__SUCCESS;
    }

    while (NULL != entry) {
        if (entry->t->node_id == node_id) {
            err_t rc = send_raw(entry->t->fd, buf, len);
            pthread_mutex_unlock(&net->clients_mutex);
            return rc;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);

    LOG_WARNING("NETWORK__send_to: no transport found for node_id=%u", node_id);
    return E__NET__SOCKET_CONNECT_FAILED;
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

    LOG_DEBUG("Calling connected_cb for fd=%d", t->fd);

    if (NULL != net->connected_cb) {
        net->connected_cb(net->session_ctx, t);
    }

    LOG_DEBUG("Starting recv loop for fd=%d", t->fd);

    while (true) {
        LOG_TRACE("About to call TRANSPORT__recv_msg on fd=%d", t->fd);
        protocol_msg_t msg;
        uint8_t *data = NULL;
        err_t rc = TRANSPORT__recv_msg(t, &msg, &data);
        if (E__SUCCESS != rc) {
            LOG_DEBUG("TRANSPORT__recv_msg returned error on fd=%d, closing connection", t->fd);
            break;
        }

        if (NULL != net->message_cb) {
            size_t total_len = PROTOCOL_HEADER_SIZE + msg.data_length;
            uint8_t *buf = malloc(total_len);
            if (NULL != buf) {
                memcpy(buf, &msg, PROTOCOL_HEADER_SIZE);
                if (NULL != data && msg.data_length > 0) {
                    memcpy(buf + PROTOCOL_HEADER_SIZE, data, msg.data_length);
                }
                net->message_cb(net->session_ctx, t, buf, total_len);
                free(buf);
            }
        }

        free(data);
    }

    LOG_INFO("Connection %s:%d closed", t->client_ip, t->client_port);

    if (NULL != net->disconnected_cb) {
        net->disconnected_cb(net->session_ctx, t);
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

static void *connect_thread_func(void *arg) {
    connect_thread_arg_t *targ = (connect_thread_arg_t *)arg;
    addr_t *addr = &targ->addr;
    int connect_timeout = targ->connect_timeout;
    network_t *net = targ->net;

    while (true) {
        int fd = connect_to_addr(addr->ip, addr->port, connect_timeout);
        if (0 > fd) {
            if (targ->reconnect_retries >= 0) {
                LOG_WARNING("Failed to connect to %s:%d, giving up", addr->ip, addr->port);
                FREE(arg);
                return NULL;
            }
            LOG_WARNING("Failed to connect to %s:%d, retrying in %ds...", addr->ip, addr->port, targ->reconnect_delay);
            sleep((unsigned int)targ->reconnect_delay);
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
        strncpy(entry->t->client_ip, addr->ip, INET_ADDRSTRLEN - 1);
        entry->t->client_ip[INET_ADDRSTRLEN - 1] = '\0';
        entry->t->client_port = addr->port;
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

        if (0 != pthread_create(&entry->thread, NULL, socket_thread_func, entry)) {
            LOG_ERROR("Failed to create socket thread");
            pthread_mutex_lock(&net->clients_mutex);
            socket_entry_remove_locked(net, entry);
            pthread_mutex_unlock(&net->clients_mutex);
            close(fd);
            FREE(entry);
            FREE(arg);
            return NULL;
        }

        pthread_detach(entry->thread);
        FREE(arg);
        return NULL;
    }
}

err_t NETWORK__init(network_t *net, const args_t *args, int node_id, network_message_cb_t msg_cb, network_disconnected_cb_t disc_cb, network_connected_cb_t conn_cb, void *ctx) {
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
    net->session_ctx = ctx;

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

            if (0 != pthread_create(&net->connect_threads[i], NULL, connect_thread_func, targ)) {
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

    if (0 != pthread_mutex_lock(&net->clients_mutex)) {
        LOG_ERROR("Failed to lock mutex during shutdown");
        FAIL(E__NET__INVALID_SOCKET);
    }

    socket_entry_t *head = net->clients;
    socket_entry_t *iter = head;
    net->clients = NULL;

    while (NULL != iter) {
        socket_entry_t *next = iter->next;
        TRANSPORT__destroy(iter->t);
        iter = next;
    }

    pthread_mutex_unlock(&net->clients_mutex);

    while (NULL != head) {
        socket_entry_t *next = head->next;
        if (0 != head->thread) {
            pthread_join(head->thread, NULL);
        }
        head = next;
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

void NETWORK__broadcast_to_all(network_t *net, const uint8_t *buf, size_t len, uint32_t exclude_node_id) {
    if (NULL == net || NULL == buf) {
        return;
    }

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *entry = net->clients;
    while (NULL != entry) {
        if (entry->t->node_id != 0 && entry->t->node_id != exclude_node_id) {
            send_raw(entry->t->fd, buf, len);
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);
}
