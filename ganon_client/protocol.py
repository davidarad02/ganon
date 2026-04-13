import construct as ct
from enum import IntEnum

GANON_PROTOCOL_MAGIC = b"GNN\0"


class MsgType(IntEnum):
    FIRST = 0
    LAST = 1


ProtocolHeader = ct.Struct(
    "magic" / ct.PaddedString(4, "ascii"),
    "node_id" / ct.Int32ub,
    "message_id" / ct.Int32ub,
    "type" / ct.Int32ub,
    "data_length" / ct.Int32ub,
)

ProtocolMessage = ct.Struct(
    "header" / ProtocolHeader,
    "data" / ct.Bytes(ct.this.header.data_length),
)
