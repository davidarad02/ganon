import construct as ct
from enum import IntEnum

GANON_PROTOCOL_MAGIC = b"GNN\x00"
DEFAULT_TTL = 16


class MsgType(IntEnum):
    NODE_INIT = 0
    PEER_INFO = 1
    NODE_DISCONNECT = 2
    CONNECTION_REJECTED = 3


ProtocolHeader = ct.Struct(
    "magic" / ct.PaddedString(4, "ascii"),
    "orig_src_node_id" / ct.Int32ub,
    "src_node_id" / ct.Int32ub,
    "dst_node_id" / ct.Int32ub,
    "message_id" / ct.Int32ub,
    "type" / ct.Int32ub,
    "data_length" / ct.Int32ub,
    "ttl" / ct.Int32ub,
)

ProtocolMessage = ct.Struct(
    "header" / ProtocolHeader,
    "data" / ct.Bytes(ct.this.header.data_length),
)
