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
