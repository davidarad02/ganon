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
#include "protocol.h"
#include "routing.h"
#include "session.h"
#include "transport.h"

int g_node_id = -1;

static ssize_t send_wrapper(int fd, const uint8_t *buf, size_t len) {
    return send(fd, buf, len, 0);
}

static void log_sent_packet(const uint8_t *header, int fd) {
    protocol_msg_t *msg = (protocol_msg_t *)header;
    uint32_t orig_src = PROTOCOL_FIELD_FROM_NETWORK(msg->orig_src_node_id);
    uint32_t src = PROTOCOL_FIELD_FROM_NETWORK(msg->src_node_id);
    uint32_t dst = PROTOCOL_FIELD_FROM_NETWORK(msg->dst_node_id);
    uint32_t msg_id = PROTOCOL_FIELD_FROM_NETWORK(msg->message_id);
    uint32_t type = PROTOCOL_FIELD_FROM_NETWORK(msg->type);
    uint32_t data_len = PROTOCOL_FIELD_FROM_NETWORK(msg->data_length);
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(msg->ttl);
    LOG_TRACE("Sent packet: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%d, ttl=%u, data_len=%u, fd=%d", orig_src, src, dst, msg_id, type, ttl, data_len, fd);
}

static err_t send_raw_packet(int fd, uint8_t *header, const uint8_t *data, size_t data_len) {
    err_t rc = E__SUCCESS;
    transport_t *t = TRANSPORT__create(fd);
    if (NULL == t) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    rc = TRANSPORT__send_one(t, header, PROTOCOL_HEADER_SIZE);
    FAIL_IF(E__SUCCESS != rc, rc);
    log_sent_packet(header, fd);

    if (NULL != data && data_len > 0) {
        rc = TRANSPORT__send_one(t, data, data_len);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

    TRANSPORT__destroy(t);

l_cleanup:
    return rc;
}

static void broadcast_to_others(network_t *net, int exclude_fd, uint32_t sender_node_id, uint8_t *header, const uint8_t *data, size_t data_len) {
    (void)sender_node_id;
    if (NULL == net || NULL == header) {
        return;
    }

    protocol_msg_t *msg = (protocol_msg_t *)header;
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(msg->ttl);
    if (ttl == 0) {
        return;
    }

    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)g_node_id);
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(ttl - 1);

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *client = net->clients;
    while (NULL != client) {
        if (client->fd != exclude_fd && 0 != client->peer_node_id) {
            uint8_t hdr[PROTOCOL_HEADER_SIZE];
            memcpy(hdr, header, PROTOCOL_HEADER_SIZE);
            err_t rc = send_raw_packet(client->fd, hdr, data, data_len);
            if (E__SUCCESS != rc) {
                LOG_WARNING("Failed to broadcast to fd %d", client->fd);
            } else {
                LOG_DEBUG("Broadcast from node %u to node %u (fd=%d, ttl=%u)", sender_node_id, client->peer_node_id, client->fd, ttl - 1);
            }
        }
        client = client->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);
}

static void broadcast_peer_info_to_others(network_t *net, int exclude_fd, uint32_t src_node_id, uint32_t *peer_list, size_t peer_count) {
    if (NULL == net || 0 == peer_count) {
        return;
    }

    uint8_t *peer_data = malloc(peer_count * sizeof(uint32_t));
    if (NULL == peer_data) {
        LOG_ERROR("Failed to allocate peer data for broadcast");
        return;
    }

    for (size_t i = 0; i < peer_count; i++) {
        ((uint32_t *)peer_data)[i] = PROTOCOL_FIELD_TO_NETWORK(peer_list[i]);
    }

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *client = net->clients;
    while (NULL != client) {
        if (client->fd != exclude_fd && 0 != client->peer_node_id) {
            transport_t *t = TRANSPORT__create(client->fd);
            if (NULL != t) {
                err_t rc = SESSION__send_packet(t, src_node_id, 0, MSG__PEER_INFO, peer_data, peer_count * sizeof(uint32_t));
                if (E__SUCCESS != rc) {
                    LOG_WARNING("Failed to broadcast PEER_INFO to fd %d", client->fd);
                } else {
                    LOG_DEBUG("Broadcast PEER_INFO (via node %u) to node %u: %zu peers", src_node_id, client->peer_node_id, peer_count);
                }
                TRANSPORT__destroy(t);
            }
        }
        client = client->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);

    free(peer_data);
}

static void broadcast_node_disconnect(network_t *net, int exclude_fd, uint32_t disconnected_node_id) {
    if (NULL == net) {
        return;
    }

    uint8_t header[PROTOCOL_HEADER_SIZE];
    memset(header, 0, sizeof(header));

    protocol_msg_t *msg = (protocol_msg_t *)header;
    memcpy(msg->magic, GANON_PROTOCOL_MAGIC, 4);
    msg->orig_src_node_id = PROTOCOL_FIELD_TO_NETWORK(disconnected_node_id);
    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)g_node_id);
    msg->dst_node_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->message_id = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->type = PROTOCOL_FIELD_TO_NETWORK((uint32_t)MSG__NODE_DISCONNECT);
    msg->data_length = PROTOCOL_FIELD_TO_NETWORK(0);
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(DEFAULT_TTL);

    pthread_mutex_lock(&net->clients_mutex);
    socket_entry_t *client = net->clients;
    while (NULL != client) {
        if (client->fd != exclude_fd && 0 != client->peer_node_id) {
            uint8_t hdr[PROTOCOL_HEADER_SIZE];
            memcpy(hdr, header, PROTOCOL_HEADER_SIZE);
            err_t rc = send_raw_packet(client->fd, hdr, NULL, 0);
            if (E__SUCCESS != rc) {
                LOG_WARNING("Failed to broadcast NODE_DISCONNECT to fd %d", client->fd);
            } else {
                LOG_DEBUG("Broadcast NODE_DISCONNECT for node %u to node %u", disconnected_node_id, client->peer_node_id);
            }
        }
        client = client->next;
    }
    pthread_mutex_unlock(&net->clients_mutex);
}

static err_t forward_message(network_t *net, uint8_t *header, uint8_t *data, size_t data_len) {
    err_t rc = E__SUCCESS;

    protocol_msg_t *msg = (protocol_msg_t *)header;
    uint32_t dst_node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->dst_node_id);
    uint32_t orig_src_node_id = PROTOCOL_FIELD_FROM_NETWORK(msg->orig_src_node_id);
    uint32_t ttl = PROTOCOL_FIELD_FROM_NETWORK(msg->ttl);

    (void)orig_src_node_id;

    if (dst_node_id == 0) {
        return E__SUCCESS;
    }

    if ((uint32_t)g_node_id == dst_node_id) {
        return E__SUCCESS;
    }

    if (ttl == 0) {
        LOG_DEBUG("TTL 0, dropping forwarded message to node %u", dst_node_id);
        return E__SUCCESS;
    }

    msg->src_node_id = PROTOCOL_FIELD_TO_NETWORK((uint32_t)g_node_id);
    msg->ttl = PROTOCOL_FIELD_TO_NETWORK(ttl - 1);

    size_t total_len = PROTOCOL_HEADER_SIZE + data_len;
    uint8_t *buf = malloc(total_len);
    if (NULL == buf) {
        LOG_ERROR("Failed to allocate buffer for forwarding");
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    memcpy(buf, header, PROTOCOL_HEADER_SIZE);
    if (NULL != data && 0 < data_len) {
        memcpy(buf + PROTOCOL_HEADER_SIZE, data, data_len);
    }

    rc = ROUTING__send_to_node(&net->routing_table, dst_node_id, buf, total_len, send_wrapper);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to forward message to node %u", dst_node_id);
    } else {
        LOG_DEBUG("Forwarded message to node %u (ttl=%u)", dst_node_id, ttl - 1);
    }

    free(buf);

l_cleanup:
    return rc;
}

static err_t send_peer_info(network_t *net, int fd, uint32_t src_node_id, uint32_t dst_node_id) {
    err_t rc = E__SUCCESS;

    pthread_mutex_lock(&net->clients_mutex);

    size_t peer_count = 0;
    socket_entry_t *client = net->clients;
    while (NULL != client) {
        if (client->fd != fd && 0 != client->peer_node_id) {
            peer_count++;
        }
        client = client->next;
    }

    uint8_t *peer_data = NULL;
    if (peer_count > 0) {
        peer_data = malloc(peer_count * sizeof(uint32_t));
        if (NULL == peer_data) {
            LOG_ERROR("Failed to allocate peer data");
            pthread_mutex_unlock(&net->clients_mutex);
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }

        peer_count = 0;
        client = net->clients;
        while (NULL != client) {
            if (client->fd != fd && 0 != client->peer_node_id) {
                ((uint32_t *)peer_data)[peer_count] = PROTOCOL_FIELD_TO_NETWORK(client->peer_node_id);
                peer_count++;
            }
            client = client->next;
        }
    }

    pthread_mutex_unlock(&net->clients_mutex);

    transport_t *t = TRANSPORT__create(fd);
    if (NULL == t) {
        LOG_ERROR("Failed to create transport for fd %d", fd);
        FREE(peer_data);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    rc = SESSION__send_packet(t, src_node_id, dst_node_id, MSG__PEER_INFO, peer_data, peer_count * sizeof(uint32_t));
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to send PEER_INFO to fd %d", fd);
    } else {
        LOG_DEBUG("Sent PEER_INFO to node %u with %zu peers", src_node_id, peer_count);
    }

    TRANSPORT__destroy(t);
    FREE(peer_data);

l_cleanup:
    return rc;
}

static void send_node_init(int fd, uint32_t node_id) {
    transport_t *t = TRANSPORT__create(fd);
    if (NULL == t) {
        LOG_ERROR("Failed to create transport for fd %d", fd);
        return;
    }

    err_t rc = SESSION__send_packet(t, node_id, 0, MSG__NODE_INIT, NULL, 0);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to send NODE_INIT to fd %d", fd);
    } else {
        LOG_DEBUG("Sent NODE_INIT to fd %d (node_id=%u)", fd, node_id);
    }

    TRANSPORT__destroy(t);
}

static void send_connection_rejected(int fd, uint32_t node_id) {
    transport_t *t = TRANSPORT__create(fd);
    if (NULL == t) {
        LOG_ERROR("Failed to create transport for fd %d", fd);
        return;
    }

    err_t rc = SESSION__send_packet(t, node_id, 0, MSG__CONNECTION_REJECTED, NULL, 0);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Failed to send CONNECTION_REJECTED to fd %d", fd);
    } else {
        LOG_DEBUG("Sent CONNECTION_REJECTED to fd %d (rejected peer, our node_id=%u)", fd, node_id);
    }

    TRANSPORT__destroy(t);

    struct timespec ts = {0, 100000};
    nanosleep(&ts, NULL);
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

static void *socket_thread_func(void *arg) {
    socket_entry_t *entry = (socket_entry_t *)arg;
    network_t *net = entry->net;

    if (entry->is_incoming) {
        LOG_INFO("Socket connected (fd=%d) from %s:%d", entry->fd, entry->client_ip, entry->client_port);
    } else {
        LOG_INFO("Socket connected (fd=%d) to %s:%d", entry->fd, entry->client_ip, entry->client_port);
    }

    transport_t *t = TRANSPORT__create(entry->fd);
    if (NULL == t) {
        LOG_ERROR("Failed to create transport for fd %d", entry->fd);
        shutdown(entry->fd, SHUT_RDWR);
        close(entry->fd);
        pthread_mutex_lock(&net->clients_mutex);
        socket_entry_remove_locked(net, entry);
        pthread_mutex_unlock(&net->clients_mutex);
        FREE(entry);
        return NULL;
    }

    if (0 != entry->is_incoming) {
        uint32_t prev_peer_node_id = 0;
        uint8_t header_buffer[PROTOCOL_HEADER_SIZE];
        memset(header_buffer, 0, sizeof(header_buffer));
        while (true) {
            uint32_t *learned_peers = NULL;
            size_t learned_count = 0;
            uint8_t *data = NULL;
            size_t data_len = 0;
            err_t rc = SESSION__process(&net->routing_table, entry->fd, t, &entry->peer_node_id, header_buffer, sizeof(header_buffer), &learned_peers, &learned_count, &data, &data_len);
            if (E__SUCCESS != rc) {
                if (E__SESSION__CONNECTION_REJECTED == rc) {
                    LOG_WARNING("Rejecting duplicate connection (peer claims node_id=%u, already connected)", entry->peer_node_id);
                    send_connection_rejected(entry->fd, (uint32_t)g_node_id);
                }
                break;
            }

            protocol_msg_t *hdr = (protocol_msg_t *)header_buffer;
            msg_type_t msg_type = (msg_type_t)PROTOCOL_FIELD_FROM_NETWORK(hdr->type);
            uint32_t dst_node_id = PROTOCOL_FIELD_FROM_NETWORK(hdr->dst_node_id);

            if (msg_type == MSG__PEER_INFO && NULL != learned_peers && 0 != learned_count) {
                LOG_DEBUG("Propagating %zu learned peers from PEER_INFO (via node %u) to other direct peers", learned_count, entry->peer_node_id);
                broadcast_peer_info_to_others(net, entry->fd, (uint32_t)g_node_id, learned_peers, learned_count);
                free(learned_peers);
                learned_peers = NULL;
            }

            if (dst_node_id != 0 && (uint32_t)g_node_id != dst_node_id && msg_type != MSG__NODE_INIT && msg_type != MSG__PEER_INFO) {
                LOG_DEBUG("Forwarding message from node %u to node %u", entry->peer_node_id, dst_node_id);
                forward_message(net, header_buffer, data, data_len);
            }

            if (prev_peer_node_id != entry->peer_node_id && 0 != entry->peer_node_id) {
                LOG_INFO("Node %u connected (fd=%d), broadcasting to network and sending peer info", entry->peer_node_id, entry->fd);
                broadcast_to_others(net, entry->fd, entry->peer_node_id, header_buffer, NULL, 0);
                send_node_init(entry->fd, (uint32_t)g_node_id);
                send_peer_info(net, entry->fd, (uint32_t)g_node_id, entry->peer_node_id);
                prev_peer_node_id = entry->peer_node_id;
                memset(header_buffer, 0, sizeof(header_buffer));
            }

            free(data);
        }
        if (0 != entry->peer_node_id) {
            LOG_INFO("Node %u disconnected (fd=%d)", entry->peer_node_id, entry->fd);
            broadcast_node_disconnect(net, entry->fd, entry->peer_node_id);
            ROUTING__remove(&net->routing_table, entry->peer_node_id);
            ROUTING__remove_via_node(&net->routing_table, entry->peer_node_id);
        }
        TRANSPORT__destroy(t);
        shutdown(entry->fd, SHUT_RDWR);
        close(entry->fd);
        pthread_mutex_lock(&net->clients_mutex);
        socket_entry_remove_locked(net, entry);
        pthread_mutex_unlock(&net->clients_mutex);
        FREE(entry);
        return NULL;
    }

    while (true) {
        uint32_t *learned_peers = NULL;
        size_t learned_count = 0;
        uint8_t *data = NULL;
        size_t data_len = 0;
        err_t session_rc = SESSION__process(&net->routing_table, entry->fd, t, &entry->peer_node_id, NULL, 0, &learned_peers, &learned_count, &data, &data_len);
        if (E__SUCCESS != session_rc) {
            LOG_WARNING("Session ended for %s:%d", entry->client_ip, entry->client_port);
            if (0 != entry->peer_node_id) {
                broadcast_node_disconnect(net, entry->fd, entry->peer_node_id);
                ROUTING__remove(&net->routing_table, entry->peer_node_id);
                ROUTING__remove_via_node(&net->routing_table, entry->peer_node_id);
            }
        }

        if (NULL != learned_peers && 0 != learned_count) {
            LOG_DEBUG("Propagating %zu learned peers from PEER_INFO (via node %u) to other direct peers", learned_count, entry->peer_node_id);
            broadcast_peer_info_to_others(net, entry->fd, (uint32_t)g_node_id, learned_peers, learned_count);
            free(learned_peers);
            learned_peers = NULL;
        }

        free(data);

        if (E__SUCCESS != session_rc) {
            TRANSPORT__destroy(t);
            shutdown(entry->fd, SHUT_RDWR);
            close(entry->fd);

            if (E__SESSION__CONNECTION_REJECTED == session_rc) {
                LOG_WARNING("Connection rejected, abandoning");
                pthread_mutex_lock(&net->clients_mutex);
                socket_entry_remove_locked(net, entry);
                pthread_mutex_unlock(&net->clients_mutex);
                FREE(entry);
                return NULL;
            }

            int reconnected = 0;
            int retry = 0;
            while (true) {
                if (0 > net->reconnect_retries) {
                    LOG_INFO("Reconnecting to %s:%d (attempt %d)...", entry->client_ip, entry->client_port, retry + 1);
                } else {
                    LOG_INFO("Reconnecting to %s:%d (attempt %d/%d)...", entry->client_ip, entry->client_port,
                             retry + 1, net->reconnect_retries);
                }

                if (0 > net->reconnect_retries || retry < net->reconnect_retries - 1) {
                    sleep((unsigned int)net->reconnect_delay);
                }

                int new_fd = connect_to_addr(entry->client_ip, entry->client_port, net->connect_timeout);
                if (0 > new_fd) {
                    if (0 > net->reconnect_retries) {
                        LOG_WARNING("Reconnect attempt %d failed, retrying in %ds", retry + 1, net->reconnect_delay);
                    } else if (retry < net->reconnect_retries - 1) {
                        LOG_WARNING("Reconnect attempt %d failed, retrying in %ds", retry + 1, net->reconnect_delay);
                    } else {
                        LOG_WARNING("Reconnect attempt %d failed", retry + 1);
                    }
                    retry++;
                    continue;
                }

                LOG_INFO("Reconnected to %s:%d (fd=%d)", entry->client_ip, entry->client_port, new_fd);
                entry->fd = new_fd;
                entry->peer_node_id = 0;
                reconnected = 1;
                break;
            }
            if (0 == reconnected) {
                LOG_WARNING("All reconnect attempts failed, giving up on %s:%d", entry->client_ip, entry->client_port);
                break;
            }

            t = TRANSPORT__create(entry->fd);
            if (NULL == t) {
                LOG_ERROR("Failed to create transport for fd %d", entry->fd);
                pthread_mutex_lock(&net->clients_mutex);
                socket_entry_remove_locked(net, entry);
                pthread_mutex_unlock(&net->clients_mutex);
                FREE(entry);
                return NULL;
            }
        }
    }

    if (0 != entry->peer_node_id) {
        LOG_INFO("Node %u disconnected (fd=%d)", entry->peer_node_id, entry->fd);
        broadcast_node_disconnect(net, entry->fd, entry->peer_node_id);
        ROUTING__remove(&net->routing_table, entry->peer_node_id);
        ROUTING__remove_via_node(&net->routing_table, entry->peer_node_id);
    }

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

        entry->fd = client_fd;
        entry->net = net;
        entry->next = NULL;
        entry->is_incoming = 1;
        inet_ntop(AF_INET, &client_addr.sin_addr, entry->client_ip, INET_ADDRSTRLEN);
        entry->client_port = ntohs(client_addr.sin_port);

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

    int fd = connect_to_addr(addr->ip, addr->port, connect_timeout);
    if (0 > fd) {
        LOG_WARNING("Failed to connect to %s:%d, continuing without it", addr->ip, addr->port);
        FREE(arg);
        return NULL;
    }

    socket_entry_t *entry = malloc(sizeof(socket_entry_t));
    if (NULL == entry) {
        LOG_ERROR("Failed to allocate socket entry");
        close(fd);
        FREE(arg);
        return NULL;
    }

    entry->fd = fd;
    entry->net = net;
    entry->next = NULL;
    entry->is_incoming = 0;
    strncpy(entry->client_ip, addr->ip, INET_ADDRSTRLEN - 1);
    entry->client_ip[INET_ADDRSTRLEN - 1] = '\0';
    entry->client_port = addr->port;

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

    send_node_init(fd, (uint32_t)g_node_id);

    if (0 != pthread_create(&entry->thread, NULL, socket_thread_func, entry)) {
        LOG_ERROR("Failed to create socket thread");
        shutdown(fd, SHUT_RDWR);
        close(fd);
        pthread_mutex_lock(&net->clients_mutex);
        socket_entry_remove_locked(net, entry);
        pthread_mutex_unlock(&net->clients_mutex);
        FREE(entry);
        FREE(arg);
        return NULL;
    }

    pthread_detach(entry->thread);

    FREE(arg);
    return NULL;
}

err_t NETWORK__init(network_t *net, const args_t *args) {
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

    if (0 != pthread_mutex_init(&net->clients_mutex, NULL)) {
        LOG_ERROR("Failed to initialize mutex");
        FAIL(E__NET__THREAD_CREATE_FAILED);
    }

    rc = ROUTING__init(&net->routing_table);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Failed to initialize routing table");
        pthread_mutex_destroy(&net->clients_mutex);
        FAIL(rc);
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
                LOG_ERROR("Failed to create connect thread for %s:%d",
                          targ->addr.ip, targ->addr.port);
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
        shutdown(iter->fd, SHUT_RDWR);
        close(iter->fd);
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

    ROUTING__destroy(&net->routing_table);

    LOG_INFO("Network shutdown complete");

l_cleanup:
    return rc;
}
