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

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (-1 == flags) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

    struct timeval timeout;
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;
    if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
        LOG_WARN("Failed to set SO_RCVTIMEO: %s", strerror(errno));
    }
    if (0 != setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        LOG_WARN("Failed to set SO_SNDTIMEO: %s", strerror(errno));
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

    LOG_DEBUG("Connecting to %s:%d...", ip, port);
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

    LOG_INFO("Connected to %s:%d", ip, port);
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

static void *client_thread_func(void *arg) {
    socket_entry_t *entry = (socket_entry_t *)arg;
    network_t *net = entry->net;
    int fd = entry->fd;
    char buffer[NETWORK_BUFFER_SIZE];

    LOG_INFO("Client connected (fd=%d) from %s:%d", fd, entry->client_ip, entry->client_port);

    while (1) {
        ssize_t bytes_read = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (0 > bytes_read) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("recv failed on fd %d: %s", fd, strerror(errno));
            break;
        } else if (0 == bytes_read) {
            LOG_DEBUG("Client disconnected (fd=%d)", fd);
            break;
        }

        buffer[bytes_read] = '\0';
        LOG_TRACE("Received %zd bytes from fd %d", bytes_read, fd);
    }

    close(fd);
    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_remove_locked(net, entry);
    pthread_mutex_unlock(&net->clients_mutex);
    free(entry);
    return NULL;
}

static void *outgoing_thread_func(void *arg) {
    socket_entry_t *entry = (socket_entry_t *)arg;
    network_t *net = entry->net;
    int fd = entry->fd;
    char buffer[NETWORK_BUFFER_SIZE];

    LOG_INFO("Outgoing connection established (fd=%d)", fd);

    while (1) {
        ssize_t bytes_read = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (0 > bytes_read) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("recv failed on fd %d: %s", fd, strerror(errno));
            break;
        } else if (0 == bytes_read) {
            LOG_DEBUG("Outgoing connection closed (fd=%d)", fd);
            break;
        }

        buffer[bytes_read] = '\0';
        LOG_TRACE("Received %zd bytes from outgoing fd %d", bytes_read, fd);
    }

    close(fd);
    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_remove_locked(net, entry);
    pthread_mutex_unlock(&net->clients_mutex);
    free(entry);
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

        entry->fd = client_fd;
        entry->net = net;
        entry->next = NULL;
        inet_ntop(AF_INET, &client_addr.sin_addr, entry->client_ip, INET_ADDRSTRLEN);
        entry->client_port = ntohs(client_addr.sin_port);

        if (0 != pthread_mutex_lock(&net->clients_mutex)) {
            LOG_ERROR("Failed to lock mutex");
            close(client_fd);
            free(entry);
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

        if (0 != pthread_create(&entry->thread, NULL, client_thread_func, entry)) {
            LOG_ERROR("Failed to create client thread");
            close(client_fd);
            free(entry);
            pthread_mutex_unlock(&net->clients_mutex);
            continue;
        }

        pthread_mutex_unlock(&net->clients_mutex);
    }

    LOG_INFO("Accept thread stopped");
    return NULL;
}

static void *connect_thread_func(void *arg) {
    network_t *net = (network_t *)arg;
    addr_t *addr = net->connect_addrs;

    int fd = connect_to_addr(addr->ip, addr->port, NETWORK_CONNECT_TIMEOUT_SEC);
    if (0 > fd) {
        LOG_WARNING("Failed to connect to %s:%d, continuing without it", addr->ip, addr->port);
        free(arg);
        return NULL;
    }

    socket_entry_t *entry = malloc(sizeof(socket_entry_t));
    if (NULL == entry) {
        LOG_ERROR("Failed to allocate socket entry");
        close(fd);
        free(arg);
        return NULL;
    }

    entry->fd = fd;
    entry->net = net;
    entry->next = NULL;

    if (0 != pthread_mutex_lock(&net->clients_mutex)) {
        LOG_ERROR("Failed to lock mutex");
        close(fd);
        free(entry);
        free(arg);
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

    outgoing_thread_func(entry);

    if (0 != pthread_mutex_lock(&net->clients_mutex)) {
        return NULL;
    }

    socket_entry_t **prev = &net->clients;
    while (NULL != *prev) {
        if (*prev == entry) {
            *prev = entry->next;
            break;
        }
        prev = &(*prev)->next;
    }

    pthread_mutex_unlock(&net->clients_mutex);

    free(arg);
    return NULL;
}

err_t network_init(network_t *net, const args_t *args) {
    err_t rc = E__SUCCESS;

    if (NULL == net || NULL == args) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    memset(net, 0, sizeof(network_t));
    net->listen_addr = args->listen_addr;
    net->connect_addrs = (addr_t *)args->connect_addrs;
    net->connect_count = args->connect_count;

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
            addr_t *addr_copy = malloc(sizeof(addr_t));
            if (NULL == addr_copy) {
                LOG_ERROR("Failed to allocate addr copy for connect thread");
                continue;
            }
            *addr_copy = net->connect_addrs[i];

            network_t *net_copy = malloc(sizeof(network_t));
            if (NULL == net_copy) {
                LOG_ERROR("Failed to allocate network copy for connect thread");
                free(addr_copy);
                continue;
            }
            memcpy(net_copy, net, sizeof(network_t));

            if (0 != pthread_create(&net->connect_threads[i], NULL, connect_thread_func, net_copy)) {
                LOG_ERROR("Failed to create connect thread for %s:%d",
                          addr_copy->ip, addr_copy->port);
                free(addr_copy);
                free(net_copy);
                continue;
            }
            net->connect_thread_count++;
        }
    }

l_cleanup:
    return rc;
}

err_t network_shutdown(network_t *net) {
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

    socket_entry_t *current = net->clients;
    while (NULL != current) {
        socket_entry_t *next = current->next;
        if (0 != current->thread) {
            pthread_join(current->thread, NULL);
        }
        close(current->fd);
        free(current);
        current = next;
    }
    net->clients = NULL;

    pthread_mutex_unlock(&net->clients_mutex);
    pthread_mutex_destroy(&net->clients_mutex);

    for (int i = 0; i < net->connect_thread_count; i++) {
        if (0 != net->connect_threads[i]) {
            pthread_detach(net->connect_threads[i]);
        }
    }
    free(net->connect_threads);

    LOG_INFO("Network shutdown complete");

l_cleanup:
    return rc;
}
