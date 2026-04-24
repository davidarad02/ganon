#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "routing.h"
#include "session.h"
#include "transport.h"
#include "tunnel.h"

#define MAX_EXEC_OUTPUT (1024 * 1024)  /* 1 MiB cap per stream */

static session_t g_session;
static uint32_t g_msg_seq_id = 0;

/* ---- pending connect responses (CONNECT_CMD -> wait for NODE_INIT) ---- */

#define MAX_PENDING_CONNECTS 8
#define PENDING_CONNECT_TIMEOUT_SEC 30

typedef struct {
    int active;
    int fd;
    uint32_t request_id;
    uint32_t requester;
    time_t timestamp;
} pending_connect_t;

static pending_connect_t g_pending_connects[MAX_PENDING_CONNECTS];
static pthread_mutex_t g_pending_connect_mutex = PTHREAD_MUTEX_INITIALIZER;

static void pending_connect_add(int fd, uint32_t request_id, uint32_t requester) {
    pthread_mutex_lock(&g_pending_connect_mutex);
    time_t now = time(NULL);
    /* Evict stale entries */
    for (int i = 0; i < MAX_PENDING_CONNECTS; i++) {
        if (g_pending_connects[i].active && (now - g_pending_connects[i].timestamp) > PENDING_CONNECT_TIMEOUT_SEC) {
            g_pending_connects[i].active = 0;
        }
    }
    /* Find free slot */
    for (int i = 0; i < MAX_PENDING_CONNECTS; i++) {
        if (!g_pending_connects[i].active) {
            g_pending_connects[i].active = 1;
            g_pending_connects[i].fd = fd;
            g_pending_connects[i].request_id = request_id;
            g_pending_connects[i].requester = requester;
            g_pending_connects[i].timestamp = now;
            pthread_mutex_unlock(&g_pending_connect_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_pending_connect_mutex);
    LOG_WARNING("Pending connect table full, dropping tracking for fd=%d", fd);
}

static int pending_connect_remove(int fd, uint32_t *out_request_id, uint32_t *out_requester) {
    int found = 0;
    pthread_mutex_lock(&g_pending_connect_mutex);
    for (int i = 0; i < MAX_PENDING_CONNECTS; i++) {
        if (g_pending_connects[i].active && g_pending_connects[i].fd == fd) {
            *out_request_id = g_pending_connects[i].request_id;
            *out_requester = g_pending_connects[i].requester;
            g_pending_connects[i].active = 0;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_connect_mutex);
    return found;
}

static void send_connect_response(session_t *s, uint32_t dst, uint32_t request_id,
                                   int status, uint32_t error_code, uint32_t connected_node_id) {
    connect_response_payload_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.request_id = htonl(request_id);
    resp.status = htonl((uint32_t)status);
    resp.error_code = htonl(error_code);
    resp.connected_node_id = htonl(connected_node_id);

    protocol_msg_t response_msg;
    memset(&response_msg, 0, sizeof(response_msg));
    memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
    response_msg.orig_src_node_id = (uint32_t)s->node_id;
    response_msg.src_node_id = (uint32_t)s->node_id;
    response_msg.dst_node_id = dst;
    response_msg.message_id = SESSION__get_next_msg_id();
    response_msg.type = MSG__CONNECT_RESPONSE;
    response_msg.data_length = sizeof(resp);
    response_msg.ttl = DEFAULT_TTL;
    response_msg.channel_id = 0;

    ROUTING__route_message(&response_msg, (const uint8_t *)&resp, 0);
}

session_t *SESSION__get_session(void) {
    return &g_session;
}

uint32_t SESSION__get_next_msg_id(void) {
    return __atomic_add_fetch(&g_msg_seq_id, 1, __ATOMIC_SEQ_CST);
}

static err_t SESSION__send_node_init(IN transport_t *t) {
    err_t rc = E__SUCCESS;
    session_t *s = SESSION__get_session();
    protocol_msg_t msg;

    memset(&msg, 0, sizeof(msg));
    memcpy(msg.magic, GANON_PROTOCOL_MAGIC, 4);
    msg.orig_src_node_id = (uint32_t)s->node_id;
    msg.src_node_id = (uint32_t)s->node_id;
    msg.dst_node_id = 0;
    msg.message_id = SESSION__get_next_msg_id();
    msg.type = MSG__NODE_INIT;
    msg.data_length = 0;
    msg.ttl = 1;

    rc = TRANSPORT__send_msg(t, &msg, NULL);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    return rc;
}

static err_t SESSION__handle_node_init(IN session_t *s, IN transport_t *t, IN uint32_t orig_src, IN uint32_t src) {
    err_t rc = E__SUCCESS;

    LOG_DEBUG("Received NODE_INIT from node %u (orig_src=%u)", src, orig_src);

    if (orig_src == src) {
        transport_t *old_t = NETWORK__get_transport(s->net, src);
        if (NULL != old_t && old_t != t) {
            LOG_WARNING("Node %u reconnected, closing old session", src);
            // Mark old session as no longer owning this node_id to prevent other 
            // threads from trying to use the transport we are about to close.
            old_t->node_id = 0;
            close(old_t->fd);
            old_t->fd = -1;
        }
        rc = ROUTING__add_direct(&s->routing_table, src, t->fd);
        FAIL_IF(E__SUCCESS != rc, rc);
        TRANSPORT__set_node_id(t, src);
        LOG_INFO("Node %u connected (direct)", src);
        
        if (t->is_incoming) {
            SESSION__send_node_init(t);
        }
        
        // When a new neighbor connects, rediscover paths for our active routes
        // to allow the new neighbor to participate in load-balancing.
        ROUTING__rediscover_active_routes(&s->routing_table);

        /* If this connection was initiated by a CONNECT_CMD, send the deferred
         * response now that we know the peer's node id. */
        {
            uint32_t req_id = 0;
            uint32_t req_requester = 0;
            if (pending_connect_remove(t->fd, &req_id, &req_requester)) {
                send_connect_response(s, req_requester, req_id,
                                      CONNECT_STATUS_SUCCESS, 0, src);
                LOG_INFO("CONNECT_CMD deferred success to node %u (req=%u): connected_node=%u",
                         req_requester, req_id, src);
            }
        }
    }

l_cleanup:
    return rc;
}

static err_t SESSION__handle_connection_rejected(IN session_t *s, IN uint32_t src) {
    err_t rc = E__SUCCESS;
    (void)s;
    (void)src;
    LOG_DEBUG("Received CONNECTION_REJECTED from node %u", src);
    rc = E__SESSION__CONNECTION_REJECTED;
    goto l_cleanup;
l_cleanup:
    return rc;
}

static err_t SESSION__handle_connect_cmd(IN session_t *s, IN transport_t *t, IN const protocol_msg_t *msg,
                                          IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    (void)t;

    if (data_len < sizeof(connect_cmd_payload_t)) {
        LOG_WARNING("CONNECT_CMD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }

    const connect_cmd_payload_t *p = (const connect_cmd_payload_t *)data;
    uint32_t request_id = ntohl(p->request_id);
    char ip[65];
    strncpy(ip, p->target_ip, 64);
    ip[64] = '\0';
    uint32_t port = ntohl(p->target_port);

    LOG_INFO("Received CONNECT_CMD from node %u (req=%u): connect to %s:%u",
             msg->orig_src_node_id, request_id, ip, port);

    int status = CONNECT_STATUS_SUCCESS;
    uint32_t error_code = 0;
    int new_fd = -1;

    err_t connect_rc = NETWORK__connect_to_peer(s->net, ip, (int)port, &status, &error_code, &new_fd);
    if (E__SUCCESS != connect_rc) {
        /* Immediate failure (TCP refused, timeout, etc.) – send response now */
        send_connect_response(s, msg->orig_src_node_id, request_id, status, error_code, 0);
        LOG_INFO("CONNECT_CMD immediate failure to node %u (req=%u): status=%d",
                 msg->orig_src_node_id, request_id, status);
        goto l_cleanup;
    }

    /* TCP connect succeeded.  Defer the response until the peer sends NODE_INIT
     * so we can include its node id.  Track the pending response by fd. */
    pending_connect_add(new_fd, request_id, msg->orig_src_node_id);
    LOG_INFO("CONNECT_CMD deferred response for fd=%d (req=%u)", new_fd, request_id);

l_cleanup:
    return rc;
}

static err_t SESSION__handle_disconnect_cmd(IN session_t *s, IN transport_t *t, IN const protocol_msg_t *msg,
                                             IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    (void)t;
    
    if (data_len < sizeof(disconnect_cmd_payload_t)) {
        LOG_WARNING("DISCONNECT_CMD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }
    
    const disconnect_cmd_payload_t *p = (const disconnect_cmd_payload_t *)data;
    uint32_t node_a = ntohl(p->node_a);
    uint32_t node_b = ntohl(p->node_b);
    
    LOG_INFO("Received DISCONNECT_CMD from node %u: disconnect %u from %u",
             msg->orig_src_node_id, node_a, node_b);
    
    /* Check if we are node_a (the initiator) */
    if ((uint32_t)s->node_id != node_a) {
        LOG_WARNING("DISCONNECT_CMD received but we are not node_a (%u != %u)", 
                    s->node_id, node_a);
        /* Forward to node_a if we have a route */
        /* For now, just report error */
        int status = DISCONNECT_STATUS_ERROR;
        uint32_t error_code = E__SESSION__INVALID_MESSAGE;
        
        disconnect_response_payload_t resp;
        resp.status = htonl((uint32_t)status);
        resp.error_code = htonl(error_code);
        
        protocol_msg_t response_msg;
        memset(&response_msg, 0, sizeof(response_msg));
        memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
        response_msg.orig_src_node_id = (uint32_t)s->node_id;
        response_msg.src_node_id = (uint32_t)s->node_id;
        response_msg.dst_node_id = msg->orig_src_node_id;
        response_msg.message_id = SESSION__get_next_msg_id();
        response_msg.type = MSG__DISCONNECT_RESPONSE;
        response_msg.data_length = sizeof(resp);
        response_msg.ttl = DEFAULT_TTL;
        response_msg.channel_id = 0;
        
        ROUTING__route_message(&response_msg, (const uint8_t *)&resp, 0);
        goto l_cleanup;
    }
    
    /* We are node_a, perform the disconnect */
    int status = DISCONNECT_STATUS_SUCCESS;
    uint32_t error_code = 0;
    
    err_t disconnect_rc = NETWORK__disconnect_from_peer(s->net, node_b, &status, &error_code);
    if (E__SUCCESS != disconnect_rc) {
        status = DISCONNECT_STATUS_ERROR;
        error_code = (uint32_t)disconnect_rc;
    }
    
    /* Send response back to originator */
    disconnect_response_payload_t resp;
    resp.status = htonl((uint32_t)status);
    resp.error_code = htonl(error_code);
    
    protocol_msg_t response_msg;
    memset(&response_msg, 0, sizeof(response_msg));
    memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
    response_msg.orig_src_node_id = (uint32_t)s->node_id;
    response_msg.src_node_id = (uint32_t)s->node_id;
    response_msg.dst_node_id = msg->orig_src_node_id;
    response_msg.message_id = SESSION__get_next_msg_id();
    response_msg.type = MSG__DISCONNECT_RESPONSE;
    response_msg.data_length = sizeof(resp);
    response_msg.ttl = DEFAULT_TTL;
    response_msg.channel_id = 0;
    
    ROUTING__route_message(&response_msg, (const uint8_t *)&resp, 0);
    
    LOG_INFO("DISCONNECT_CMD response sent to node %u: status=%d",
             msg->orig_src_node_id, status);

l_cleanup:
    return rc;
}

/* ---- exec / file helpers ---- */

static void send_response_to_node(session_t *s, uint32_t dst_node_id, uint32_t msg_type,
                                   const uint8_t *data, size_t data_len) {
    protocol_msg_t response_msg;
    memset(&response_msg, 0, sizeof(response_msg));
    memcpy(response_msg.magic, GANON_PROTOCOL_MAGIC, 4);
    response_msg.orig_src_node_id = (uint32_t)s->node_id;
    response_msg.src_node_id = (uint32_t)s->node_id;
    response_msg.dst_node_id = dst_node_id;
    response_msg.message_id = SESSION__get_next_msg_id();
    response_msg.type = msg_type;
    response_msg.data_length = (uint32_t)data_len;
    response_msg.ttl = DEFAULT_TTL;
    response_msg.channel_id = 0;
    ROUTING__route_message(&response_msg, data, 0);
}

static int exec_command(const char *cmd, uint8_t **out_stdout, size_t *out_stdout_len,
                        uint8_t **out_stderr, size_t *out_stderr_len) {
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t pid;
    int status = 0;

    *out_stdout = NULL;
    *out_stdout_len = 0;
    *out_stderr = NULL;
    *out_stderr_len = 0;

    if (0 != pipe(stdout_pipe) || 0 != pipe(stderr_pipe)) {
        LOG_ERROR("pipe() failed: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (0 > pid) {
        LOG_ERROR("fork() failed: %s", strerror(errno));
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return -1;
    }

    if (0 == pid) {
        /* Child */
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    /* Read stdout */
    {
        uint8_t buf[4096];
        size_t total = 0;
        ssize_t n;
        while ((n = read(stdout_pipe[0], buf, sizeof(buf))) > 0) {
            if (total + (size_t)n > MAX_EXEC_OUTPUT) {
                size_t cap = MAX_EXEC_OUTPUT - total;
                if (cap > 0) {
                    uint8_t *tmp = realloc(*out_stdout, total + cap);
                    if (tmp) {
                        memcpy(tmp + total, buf, cap);
                        *out_stdout = tmp;
                        total += cap;
                    }
                }
                break;
            }
            uint8_t *tmp = realloc(*out_stdout, total + (size_t)n);
            if (NULL == tmp) {
                break;
            }
            *out_stdout = tmp;
            memcpy(*out_stdout + total, buf, (size_t)n);
            total += (size_t)n;
        }
        *out_stdout_len = total;
    }
    close(stdout_pipe[0]);

    /* Read stderr */
    {
        uint8_t buf[4096];
        size_t total = 0;
        ssize_t n;
        while ((n = read(stderr_pipe[0], buf, sizeof(buf))) > 0) {
            if (total + (size_t)n > MAX_EXEC_OUTPUT) {
                size_t cap = MAX_EXEC_OUTPUT - total;
                if (cap > 0) {
                    uint8_t *tmp = realloc(*out_stderr, total + cap);
                    if (tmp) {
                        memcpy(tmp + total, buf, cap);
                        *out_stderr = tmp;
                        total += cap;
                    }
                }
                break;
            }
            uint8_t *tmp = realloc(*out_stderr, total + (size_t)n);
            if (NULL == tmp) {
                break;
            }
            *out_stderr = tmp;
            memcpy(*out_stderr + total, buf, (size_t)n);
            total += (size_t)n;
        }
        *out_stderr_len = total;
    }
    close(stderr_pipe[0]);

    if (0 > waitpid(pid, &status, 0)) {
        LOG_ERROR("waitpid() failed: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static err_t SESSION__handle_exec_cmd(IN session_t *s, IN const protocol_msg_t *msg,
                                       IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    uint32_t request_id = 0;
    const char *cmd = NULL;
    uint8_t *stdout_buf = NULL;
    size_t stdout_len = 0;
    uint8_t *stderr_buf = NULL;
    size_t stderr_len = 0;
    int exit_code = -1;
    uint8_t *response = NULL;

    if (data_len < 5) {
        LOG_WARNING("EXEC_CMD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }

    request_id = ntohl(*(const uint32_t *)data);
    cmd = (const char *)(data + 4);

    /* Verify null-termination within bounds */
    size_t cmd_len = strnlen(cmd, data_len - 4);
    if (cmd_len >= data_len - 4) {
        LOG_WARNING("EXEC_CMD command not null-terminated");
        FAIL(E__SESSION__INVALID_MESSAGE);
    }

    LOG_INFO("EXEC_CMD from node %u (req=%u): %s", msg->orig_src_node_id, request_id, cmd);

    exit_code = exec_command(cmd, &stdout_buf, &stdout_len, &stderr_buf, &stderr_len);

    size_t response_len = sizeof(exec_response_header_t) + stdout_len + stderr_len;
    response = malloc(response_len);
    if (NULL == response) {
        LOG_ERROR("Failed to allocate exec response buffer");
        exit_code = -1;
        stdout_len = 0;
        stderr_len = 0;
        response_len = sizeof(exec_response_header_t);
        response = malloc(response_len);
        if (NULL == response) {
            goto l_cleanup;
        }
    }

    exec_response_header_t *hdr = (exec_response_header_t *)response;
    hdr->request_id = htonl(request_id);
    hdr->exit_code = htonl((uint32_t)exit_code);
    hdr->stdout_len = htonl((uint32_t)stdout_len);
    hdr->stderr_len = htonl((uint32_t)stderr_len);

    if (stdout_len > 0) {
        memcpy(response + sizeof(exec_response_header_t), stdout_buf, stdout_len);
    }
    if (stderr_len > 0) {
        memcpy(response + sizeof(exec_response_header_t) + stdout_len, stderr_buf, stderr_len);
    }

    send_response_to_node(s, msg->orig_src_node_id, MSG__EXEC_RESPONSE, response, response_len);

l_cleanup:
    FREE(stdout_buf);
    FREE(stderr_buf);
    FREE(response);
    return rc;
}

static err_t SESSION__handle_file_upload(IN session_t *s, IN const protocol_msg_t *msg,
                                          IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    uint32_t request_id = 0;
    const char *path = NULL;
    const uint8_t *file_data = NULL;
    size_t file_data_len = 0;
    uint32_t status = FILE_STATUS_SUCCESS;
    const char *error_msg = "";
    file_upload_response_payload_t resp;

    if (data_len < 261) {
        LOG_WARNING("FILE_UPLOAD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }

    request_id = ntohl(*(const uint32_t *)data);
    path = (const char *)(data + 4);

    /* Ensure path is null-terminated within the 256-byte field */
    size_t path_len = strnlen(path, 256);
    if (path_len >= 256) {
        LOG_WARNING("FILE_UPLOAD path not null-terminated");
        status = FILE_STATUS_OTHER;
        error_msg = "Invalid path format";
        goto send_response;
    }

    file_data = data + 4 + 256;
    if (data_len > 4 + 256) {
        file_data_len = data_len - (4 + 256);
    }

    LOG_INFO("FILE_UPLOAD from node %u (req=%u): %s (%zu bytes)",
             msg->orig_src_node_id, request_id, path, file_data_len);

    FILE *fp = fopen(path, "wb");
    if (NULL == fp) {
        switch (errno) {
        case ENOSPC:
            status = FILE_STATUS_NO_SPACE;
            error_msg = "No space left on device";
            break;
        case EROFS:
            status = FILE_STATUS_READ_ONLY;
            error_msg = "Read-only file system";
            break;
        case EACCES:
        case EPERM:
            status = FILE_STATUS_PERMISSION;
            error_msg = "Permission denied";
            break;
        default:
            status = FILE_STATUS_OTHER;
            error_msg = strerror(errno);
            break;
        }
        LOG_WARNING("Failed to open %s for writing: %s", path, error_msg);
        goto send_response;
    }

    if (file_data_len > 0) {
        size_t written = fwrite(file_data, 1, file_data_len, fp);
        if (written != file_data_len) {
            if (errno == ENOSPC) {
                status = FILE_STATUS_NO_SPACE;
                error_msg = "No space left on device";
            } else {
                status = FILE_STATUS_OTHER;
                error_msg = strerror(errno);
            }
            LOG_WARNING("Failed to write to %s: %s", path, error_msg);
            fclose(fp);
            /* Remove partially written file */
            unlink(path);
            goto send_response;
        }
    }

    if (0 != fclose(fp)) {
        if (errno == ENOSPC) {
            status = FILE_STATUS_NO_SPACE;
            error_msg = "No space left on device";
        } else {
            status = FILE_STATUS_OTHER;
            error_msg = strerror(errno);
        }
        LOG_WARNING("Failed to close %s: %s", path, error_msg);
        unlink(path);
        goto send_response;
    }

    LOG_INFO("FILE_UPLOAD complete: %s", path);

l_cleanup:
send_response:
    memset(&resp, 0, sizeof(resp));
    resp.request_id = htonl(request_id);
    resp.status = htonl(status);
    strncpy(resp.error_msg, error_msg, sizeof(resp.error_msg) - 1);
    resp.error_msg[sizeof(resp.error_msg) - 1] = '\0';

    send_response_to_node(s, msg->orig_src_node_id, MSG__FILE_UPLOAD_RESPONSE,
                          (const uint8_t *)&resp, sizeof(resp));

    return rc;
}

static err_t SESSION__handle_file_download(IN session_t *s, IN const protocol_msg_t *msg,
                                            IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    uint32_t request_id = 0;
    const char *path = NULL;
    uint32_t status = FILE_STATUS_SUCCESS;
    const char *error_msg = "";
    uint8_t *file_buf = NULL;
    long file_size = 0;
    uint8_t *response = NULL;
    size_t response_len = 0;

    if (data_len < 5) {
        LOG_WARNING("FILE_DOWNLOAD payload too small (%zu bytes)", data_len);
        FAIL(E__SESSION__INVALID_MESSAGE);
    }

    request_id = ntohl(*(const uint32_t *)data);
    path = (const char *)(data + 4);

    size_t path_len = strnlen(path, data_len - 4);
    if (path_len >= data_len - 4) {
        LOG_WARNING("FILE_DOWNLOAD path not null-terminated");
        status = FILE_STATUS_OTHER;
        error_msg = "Invalid path format";
        goto send_response;
    }

    LOG_INFO("FILE_DOWNLOAD from node %u (req=%u): %s",
             msg->orig_src_node_id, request_id, path);

    FILE *fp = fopen(path, "rb");
    if (NULL == fp) {
        switch (errno) {
        case ENOENT:
            status = FILE_STATUS_NOT_FOUND;
            error_msg = "File not found";
            break;
        case EACCES:
        case EPERM:
            status = FILE_STATUS_PERMISSION;
            error_msg = "Permission denied";
            break;
        default:
            status = FILE_STATUS_OTHER;
            error_msg = strerror(errno);
            break;
        }
        LOG_WARNING("Failed to open %s for reading: %s", path, error_msg);
        goto send_response;
    }

    if (0 != fseek(fp, 0, SEEK_END)) {
        status = FILE_STATUS_OTHER;
        error_msg = "Failed to determine file size";
        fclose(fp);
        goto send_response;
    }

    file_size = ftell(fp);
    if (0 > file_size) {
        status = FILE_STATUS_OTHER;
        error_msg = "Failed to determine file size";
        fclose(fp);
        goto send_response;
    }

    rewind(fp);

    if (file_size > 0) {
        file_buf = malloc((size_t)file_size);
        if (NULL == file_buf) {
            status = FILE_STATUS_OTHER;
            error_msg = "Failed to allocate memory for file";
            fclose(fp);
            goto send_response;
        }

        size_t read_total = fread(file_buf, 1, (size_t)file_size, fp);
        if ((long)read_total != file_size) {
            status = FILE_STATUS_OTHER;
            error_msg = "Failed to read file";
            fclose(fp);
            goto send_response;
        }
    }

    fclose(fp);

l_cleanup:
send_response:
    response_len = 8 + (status == FILE_STATUS_SUCCESS ? (size_t)file_size : strlen(error_msg) + 1);
    response = malloc(response_len);
    if (NULL == response) {
        LOG_ERROR("Failed to allocate download response buffer");
        FREE(file_buf);
        return rc;
    }

    *(uint32_t *)response = htonl(request_id);
    *(uint32_t *)(response + 4) = htonl(status);

    if (status == FILE_STATUS_SUCCESS) {
        if (file_size > 0) {
            memcpy(response + 8, file_buf, (size_t)file_size);
        }
    } else {
        memcpy(response + 8, error_msg, strlen(error_msg) + 1);
    }

    send_response_to_node(s, msg->orig_src_node_id, MSG__FILE_DOWNLOAD_RESPONSE,
                          response, response_len);

    FREE(file_buf);
    FREE(response);
    return rc;
}

err_t SESSION__init(INOUT session_t *s, IN int node_id) {
    err_t rc = E__SUCCESS;

    if (NULL == s) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    memset(s, 0, sizeof(session_t));
    s->node_id = node_id;
    s->net = NULL;

    rc = ROUTING__init(&s->routing_table);
    if (E__SUCCESS != rc) {
        LOG_ERROR("Failed to initialize routing table");
        FAIL(rc);
    }

l_cleanup:
    return rc;
}

void SESSION__destroy(IN session_t *s) {
    if (NULL == s) {
        return;
    }
    ROUTING__destroy(&s->routing_table);
}

void SESSION__set_network(IN session_t *s, IN network_t *net) {
    if (NULL == s) {
        return;
    }
    s->net = net;
}

network_t *SESSION__get_network(IN session_t *s) {
    if (NULL == s) {
        return NULL;
    }
    return s->net;
}

int SESSION__get_node_id(IN session_t *s) {
    if (NULL == s) {
        return -1;
    }
    return s->node_id;
}

routing_table_t *SESSION__get_routing_table(IN session_t *s) {
    if (NULL == s) {
        return NULL;
    }
    return &s->routing_table;
}

void SESSION__on_connected(IN transport_t *t) {
    session_t *s = SESSION__get_session();
    if (NULL == s || NULL == t) {
        return;
    }

    LOG_INFO("Connection established with %s:%d", t->client_ip, t->client_port);

    if (!t->is_incoming) {
        SESSION__send_node_init(t);
    }
}

void SESSION__on_message(IN transport_t *t, IN const protocol_msg_t *msg, IN const uint8_t *data, IN size_t data_len) {
    err_t rc = E__SUCCESS;
    session_t *s = SESSION__get_session();

    if (NULL == s || NULL == t || NULL == msg) {
        goto l_cleanup;
    }
    (void)data_len;
    (void)data;

    if (!PROTOCOL__validate_magic(msg->magic)) {
        LOG_WARNING("Invalid magic from %s:%d", t->client_ip, t->client_port);
        goto l_cleanup;
    }

    uint32_t orig_src = msg->orig_src_node_id;
    uint32_t src = msg->src_node_id;
    msg_type_t type = (msg_type_t)msg->type;

    switch (type) {
    case MSG__NODE_INIT:
        rc = SESSION__handle_node_init(s, t, orig_src, src);
        break;
    case MSG__CONNECTION_REJECTED:
        rc = SESSION__handle_connection_rejected(s, src);
        break;
    case MSG__USER_DATA:
        LOG_INFO("Received USER_DATA from node %u! Length: %zu", orig_src, data_len);
        break;
    case MSG__PING:
        {
            protocol_msg_t pong_msg;
            LOG_INFO("Received PING from node %u, sending PONG replica", orig_src);
            memset(&pong_msg, 0, sizeof(pong_msg));
            memcpy(pong_msg.magic, GANON_PROTOCOL_MAGIC, 4);
            pong_msg.orig_src_node_id = (uint32_t)s->node_id;
            pong_msg.src_node_id = (uint32_t)s->node_id;
            pong_msg.dst_node_id = orig_src;
            pong_msg.message_id = SESSION__get_next_msg_id();
            pong_msg.type = MSG__PONG;
            pong_msg.data_length = (uint32_t)data_len;
            pong_msg.ttl = DEFAULT_TTL;
            pong_msg.channel_id = msg->channel_id;

            ROUTING__route_message(&pong_msg, data, 0);
        }
        break;
    case MSG__PONG:
        LOG_INFO("Received PONG from node %u! Length: %zu", orig_src, data_len);
        break;
    case MSG__RREQ:
    case MSG__RREP:
    case MSG__RERR:
        break;
    case MSG__TUNNEL_OPEN:
    case MSG__TUNNEL_CONN_OPEN:
    case MSG__TUNNEL_CONN_ACK:
    case MSG__TUNNEL_DATA:
    case MSG__TUNNEL_CONN_CLOSE:
    case MSG__TUNNEL_CLOSE:
        TUNNEL__on_message(t, msg, data, data_len);
        break;
    case MSG__CONNECT_CMD:
        rc = SESSION__handle_connect_cmd(s, t, msg, data, data_len);
        break;
    case MSG__DISCONNECT_CMD:
        rc = SESSION__handle_disconnect_cmd(s, t, msg, data, data_len);
        break;
    case MSG__CONNECT_RESPONSE:
    case MSG__DISCONNECT_RESPONSE:
        /* These are handled by the waiting client or can be logged */
        LOG_INFO("Received %s from node %u",
                 (type == MSG__CONNECT_RESPONSE) ? "CONNECT_RESPONSE" : "DISCONNECT_RESPONSE",
                 orig_src);
        break;
    case MSG__EXEC_CMD:
        rc = SESSION__handle_exec_cmd(s, msg, data, data_len);
        break;
    case MSG__FILE_UPLOAD:
        rc = SESSION__handle_file_upload(s, msg, data, data_len);
        break;
    case MSG__FILE_DOWNLOAD:
        rc = SESSION__handle_file_download(s, msg, data, data_len);
        break;
    case MSG__EXEC_RESPONSE:
    case MSG__FILE_UPLOAD_RESPONSE:
    case MSG__FILE_DOWNLOAD_RESPONSE:
        /* Responses are consumed by the waiting client */
        LOG_DEBUG("Received %s from node %u",
                  (type == MSG__EXEC_RESPONSE) ? "EXEC_RESPONSE" :
                  (type == MSG__FILE_UPLOAD_RESPONSE) ? "FILE_UPLOAD_RESPONSE" : "FILE_DOWNLOAD_RESPONSE",
                  orig_src);
        break;
    default:
        LOG_WARNING("Unknown message type: %d", type);
        break;
    }

    if (E__SESSION__CONNECTION_REJECTED == rc) {
        LOG_WARNING("Connection rejected for node %u", src);
    }

l_cleanup:
    (void)rc;
}

void SESSION__on_disconnected(IN transport_t *t) {
    session_t *s = SESSION__get_session();
    uint32_t node_id = TRANSPORT__get_node_id(t);

    if (NULL == s) {
        return;
    }

    /* If there was a pending CONNECT_CMD waiting for NODE_INIT on this fd,
     * send an error response because the connection failed before handshake. */
    {
        uint32_t req_id = 0;
        uint32_t req_requester = 0;
        if (pending_connect_remove(t->fd, &req_id, &req_requester)) {
            send_connect_response(s, req_requester, req_id,
                                  CONNECT_STATUS_ERROR, E__SESSION__CONNECTION_REJECTED, 0);
            LOG_INFO("Pending connect on fd=%d failed: connection dropped before NODE_INIT", t->fd);
        }
    }

    if (0 == node_id) {
        return;
    }

    LOG_INFO("Node %u disconnected", node_id);
    ROUTING__handle_disconnect(node_id);
    TUNNEL__handle_disconnect(node_id);
}
