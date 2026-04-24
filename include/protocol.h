#ifndef GANON_PROTOCOL_H
#define GANON_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "err.h"

#define GANON_PROTOCOL_MAGIC "GNN\0"
#define PROTOCOL_HEADER_SIZE sizeof(protocol_msg_t)
#define DEFAULT_TTL 16

typedef enum {
    MSG__NODE_INIT = 0,
    MSG__CONNECTION_REJECTED = 1,
    MSG__RREQ = 2,
    MSG__RREP = 3,
    MSG__RERR = 4,
    MSG__USER_DATA = 5,
    MSG__PING = 6,
    MSG__PONG = 7,
    MSG__TUNNEL_OPEN = 8,
    MSG__TUNNEL_CONN_OPEN = 9,
    MSG__TUNNEL_CONN_ACK = 10,
    MSG__TUNNEL_DATA = 11,
    MSG__TUNNEL_CONN_CLOSE = 12,
    MSG__TUNNEL_CLOSE = 13,
    MSG__CONNECT_CMD = 14,
    MSG__CONNECT_RESPONSE = 15,
    MSG__DISCONNECT_CMD = 16,
    MSG__DISCONNECT_RESPONSE = 17,
    MSG__EXEC_CMD = 18,
    MSG__EXEC_RESPONSE = 19,
    MSG__FILE_UPLOAD = 20,
    MSG__FILE_UPLOAD_RESPONSE = 21,
    MSG__FILE_DOWNLOAD = 22,
    MSG__FILE_DOWNLOAD_RESPONSE = 23,
} msg_type_t;

typedef struct {
    uint8_t magic[4];
    uint32_t orig_src_node_id;
    uint32_t src_node_id;
    uint32_t dst_node_id;
    uint32_t message_id;
    uint32_t type;
    uint32_t data_length;
    uint32_t ttl;
    uint32_t channel_id;
} protocol_msg_t;

bool PROTOCOL__validate_magic(IN const uint8_t *buf);

err_t PROTOCOL__unserialize(IN const uint8_t *buf, IN size_t len, OUT protocol_msg_t *msg, OUT uint8_t **data, OUT size_t *data_len);
err_t PROTOCOL__serialize(IN const protocol_msg_t *msg, IN const uint8_t *data, OUT uint8_t *buf, IN size_t buf_len, OUT size_t *bytes_written);

/* Connect/Disconnect command payload structures */
typedef struct __attribute__((packed)) {
    uint32_t request_id;  /* correlation id echoed back in response */
    char target_ip[64];
    uint32_t target_port;
} connect_cmd_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t request_id;       /* matches the request */
    uint32_t status;           /* 0 = success, 1 = refused, 2 = timeout, 3 = other error */
    uint32_t error_code;       /* Implementation-specific error code */
    uint32_t connected_node_id;/* node id of the peer we connected to (0 if unknown) */
} connect_response_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t node_a;      /* First node to disconnect (initiator) */
    uint32_t node_b;      /* Second node to disconnect (target) */
} disconnect_cmd_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t status;      /* 0 = success, 1 = not connected, 2 = other error */
    uint32_t error_code;
} disconnect_response_payload_t;

/* Status codes for connect response */
#define CONNECT_STATUS_SUCCESS   0
#define CONNECT_STATUS_REFUSED   1
#define CONNECT_STATUS_TIMEOUT   2
#define CONNECT_STATUS_ERROR     3

/* Status codes for disconnect response */
#define DISCONNECT_STATUS_SUCCESS        0
#define DISCONNECT_STATUS_NOT_CONNECTED  1
#define DISCONNECT_STATUS_ERROR          2

/* ---- Exec command payloads ---- */

/* EXEC_CMD: data is a null-terminated command string preceded by a 4-byte
 * request_id (network byte order) for correlation. */

/* EXEC_RESPONSE: data layout:
 *   [4 bytes] request_id (matches the request)
 *   [4 bytes] exit_code
 *   [4 bytes] stdout_len
 *   [4 bytes] stderr_len
 *   [N bytes] stdout data
 *   [M bytes] stderr data
 */
typedef struct __attribute__((packed)) {
    uint32_t request_id;
    uint32_t exit_code;
    uint32_t stdout_len;
    uint32_t stderr_len;
    /* stdout and stderr data follow immediately */
} exec_response_header_t;

/* ---- File transfer payloads ---- */

/* FILE_UPLOAD: data layout:
 *   [4 bytes] request_id
 *   [256 bytes] remote path (null-padded string)
 *   [N bytes] file data
 */

/* FILE_UPLOAD_RESPONSE: data layout:
 *   [4 bytes] request_id
 *   [4 bytes] status (0 = success)
 *   [256 bytes] error message (null-padded, empty on success)
 */
typedef struct __attribute__((packed)) {
    uint32_t request_id;
    uint32_t status;
    char error_msg[256];
} file_upload_response_payload_t;

/* FILE_DOWNLOAD: data layout:
 *   [4 bytes] request_id
 *   [N bytes] path string (null-terminated)
 */

/* FILE_DOWNLOAD_RESPONSE: data layout:
 *   [4 bytes] request_id
 *   [4 bytes] status (0 = success)
 *   [N bytes] file data or error message
 */

/* Status codes for file operations */
#define FILE_STATUS_SUCCESS          0
#define FILE_STATUS_NOT_FOUND        1
#define FILE_STATUS_NO_SPACE         2
#define FILE_STATUS_READ_ONLY        3
#define FILE_STATUS_PERMISSION       4
#define FILE_STATUS_OTHER            5

#endif /* #ifndef GANON_PROTOCOL_H */
