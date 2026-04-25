#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "transport.h"

transport_t *TRANSPORT__alloc_base(int fd, const skin_ops_t *skin) {
    transport_t *t = calloc(1, sizeof(transport_t));
    if (NULL == t) {
        LOG_ERROR("Failed to allocate transport");
        return NULL;
    }

    t->fd          = fd;
    t->is_incoming = 0;
    t->client_ip[0] = '\0';
    t->client_port = 0;
    t->node_id     = 0;
    t->ctx         = NULL;
    t->skin                = skin;
    t->skin_ctx            = NULL;

    return t;
}

void TRANSPORT__free_base(transport_t *t) {
    if (NULL == t) {
        return;
    }
    free(t);
}

void TRANSPORT__destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    if (NULL != t->skin && NULL != t->skin->transport_destroy) {
        t->skin->transport_destroy(t);
    } else if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
    TRANSPORT__free_base(t);
}

err_t TRANSPORT__recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    if (NULL == t || NULL == t->skin || NULL == t->skin->recv_msg) {
        return E__INVALID_ARG_NULL_POINTER;
    }
    return t->skin->recv_msg(t, msg, data);
}

err_t TRANSPORT__send_msg(transport_t *t, const protocol_msg_t *msg, const uint8_t *data) {
    if (NULL == t || NULL == t->skin || NULL == t->skin->send_msg) {
        return E__INVALID_ARG_NULL_POINTER;
    }
    return t->skin->send_msg(t, msg, data);
}

int TRANSPORT__get_fd(transport_t *t) {
    if (NULL == t) return -1;
    return t->fd;
}

uint32_t TRANSPORT__get_node_id(transport_t *t) {
    if (NULL == t) return 0;
    return t->node_id;
}

void TRANSPORT__set_node_id(transport_t *t, uint32_t node_id) {
    if (NULL == t) return;
    t->node_id = node_id;
}

err_t TRANSPORT__send_to_node_id(network_t *net, uint32_t node_id,
                                   const protocol_msg_t *msg, const uint8_t *data) {
    if (NULL == net || NULL == msg) {
        return E__INVALID_ARG_NULL_POINTER;
    }
    transport_t *t = NETWORK__get_transport(net, node_id);
    if (NULL == t) {
        LOG_WARNING("TRANSPORT__send_to_node_id: no transport for node_id=%u", node_id);
        return E__NET__SOCKET_CONNECT_FAILED;
    }
    return TRANSPORT__send_msg(t, msg, data);
}
