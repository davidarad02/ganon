import construct as ct
from enum import IntEnum

GANON_PROTOCOL_MAGIC = b"GNN\x00"
DEFAULT_TTL = 16


class MsgType(IntEnum):
    NODE_INIT = 0
    CONNECTION_REJECTED = 1
    RREQ = 2
    RREP = 3
    RERR = 4
    USER_DATA = 5
    PING = 6
    PONG = 7
    TUNNEL_OPEN = 8
    TUNNEL_CONN_OPEN = 9
    TUNNEL_CONN_ACK = 10
    TUNNEL_DATA = 11
    TUNNEL_CONN_CLOSE = 12
    TUNNEL_CLOSE = 13
    CONNECT_CMD = 14
    CONNECT_RESPONSE = 15
    DISCONNECT_CMD = 16
    DISCONNECT_RESPONSE = 17
    EXEC_CMD = 18
    EXEC_RESPONSE = 19
    FILE_UPLOAD = 20
    FILE_UPLOAD_RESPONSE = 21
    FILE_DOWNLOAD = 22
    FILE_DOWNLOAD_RESPONSE = 23


ProtocolHeader = ct.Struct(
    "magic" / ct.PaddedString(4, "ascii"),
    "orig_src_node_id" / ct.Int32ub,
    "src_node_id" / ct.Int32ub,
    "dst_node_id" / ct.Int32ub,
    "message_id" / ct.Int32ub,
    "type" / ct.Int32ub,
    "data_length" / ct.Int32ub,
    "ttl" / ct.Int32ub,
    "channel_id" / ct.Int32ub,
)

ProtocolMessage = ct.Struct(
    "header" / ProtocolHeader,
    "data" / ct.Bytes(ct.this.header.data_length),
)

TUNNEL_PROTO_TCP = 0
TUNNEL_PROTO_UDP = 1

TunnelOpenPayload = ct.Struct(
    "tunnel_id"   / ct.Int32ub,
    "dst_node_id" / ct.Int32ub,
    "src_port"    / ct.Int16ub,
    "remote_port" / ct.Int16ub,
    "protocol"    / ct.Byte,
    "pad"         / ct.Bytes(3),
    "src_host"    / ct.PaddedString(64, "ascii"),
    "remote_host" / ct.PaddedString(256, "ascii"),
)

TunnelClosePayload = ct.Struct(
    "tunnel_id" / ct.Int32ub,
    "flags" / ct.Int32ub,  # 0 = soft close (default), 1 = force close
)

# Connect command payload
ConnectCmdPayload = ct.Struct(
    "request_id" / ct.Int32ub,
    "target_ip" / ct.PaddedString(64, "ascii"),
    "target_port" / ct.Int32ub,
    "skin_id" / ct.Int32ub,  # 0 = use default skin on the receiving node
)

# Connect response payload
ConnectResponsePayload = ct.Struct(
    "request_id" / ct.Int32ub,  # matches the request
    "status" / ct.Int32ub,  # 0 = success, 1 = refused, 2 = timeout, 3 = other error
    "error_code" / ct.Int32ub,  # Implementation-specific error code
    "connected_node_id" / ct.Int32ub,  # node id of the peer we connected to (0 if unknown)
)

# Disconnect command payload
DisconnectCmdPayload = ct.Struct(
    "node_a" / ct.Int32ub,  # First node to disconnect (0 = use originator)
    "node_b" / ct.Int32ub,  # Second node to disconnect (0 = target target_ip:target_port)
)

# Disconnect response payload
DisconnectResponsePayload = ct.Struct(
    "status" / ct.Int32ub,  # 0 = success, 1 = not connected, 2 = other error
    "error_code" / ct.Int32ub,
)

# Connect response status codes
CONNECT_STATUS_SUCCESS = 0
CONNECT_STATUS_REFUSED = 1
CONNECT_STATUS_TIMEOUT = 2
CONNECT_STATUS_ERROR = 3

# File operation status codes
FILE_STATUS_SUCCESS = 0
FILE_STATUS_NOT_FOUND = 1
FILE_STATUS_NO_SPACE = 2
FILE_STATUS_READ_ONLY = 3
FILE_STATUS_PERMISSION = 4
FILE_STATUS_OTHER = 5

# File chunk size for chunked transfers (256 KB)
# Must fit within a single encrypted frame: max plaintext ~299,656 bytes
FILE_CHUNK_SIZE = 256 * 1024  # 262,144 bytes

# File upload payload (chunked)
# Layout: request_id(4) + path(256) + chunk_index(4) + total_chunks(4) + file_data
FileUploadPayload = ct.Struct(
    "request_id" / ct.Int32ub,
    "path" / ct.PaddedString(256, "ascii"),
    "chunk_index" / ct.Int32ub,   # 0-based chunk index
    "total_chunks" / ct.Int32ub,  # total chunks (1 = single upload, backwards compat)
)

# File download request payload (chunked)
# Layout: request_id(4) + path(null-term) + offset(4) + length(4)
FileDownloadPayload = ct.Struct(
    "request_id" / ct.Int32ub,
    "path" / ct.CString("ascii"),
    "offset" / ct.Int32ub,  # byte offset to start reading (0 = start of file)
    "length" / ct.Int32ub,  # bytes to read (0 = entire file, backwards compat)
)

# File download response header (chunked)
# Layout: request_id(4) + status(4) + total_size(4) + data
FileDownloadResponseHeader = ct.Struct(
    "request_id" / ct.Int32ub,
    "status" / ct.Int32ub,
    "total_size" / ct.Int32ub,  # total file size on disk (0 if error)
)
