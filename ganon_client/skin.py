from abc import ABC, abstractmethod
from enum import IntEnum
from typing import Optional


class NetworkSkin(IntEnum):
    TCP_MONOCYPHER = 1
    TCP_PLAIN = 2
    TCP_XOR = 3
    TCP_CHACHA20 = 4
    SSH = 5
    UDP_QUIC = 6


class NetworkSkinImpl(ABC):
    """Abstract base class for all network skin implementations.

    A skin owns the full vertical slice: TCP connect, handshake,
    per-message framing, encryption, and teardown.
    """

    @classmethod
    @abstractmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "NetworkSkinImpl":
        """Connect to ip:port, complete handshake, return a ready instance."""

    @abstractmethod
    async def send(self, plaintext: bytes) -> None:
        """Encrypt and send plaintext as one framed message."""

    @abstractmethod
    async def recv(self) -> Optional[bytes]:
        """Receive and decrypt one framed message. Returns None on disconnect."""

    @abstractmethod
    def shutdown(self) -> None:
        """Initiate socket shutdown (SHUT_RDWR). Safe to call from sync context."""

    @abstractmethod
    def close(self) -> None:
        """Close the underlying socket. Safe to call from sync context."""


_SKIN_REGISTRY: dict = {}


def register_skin(skin: NetworkSkin, impl_class: type) -> None:
    _SKIN_REGISTRY[skin] = impl_class


def get_skin_impl(skin: NetworkSkin) -> Optional[type]:
    return _SKIN_REGISTRY.get(skin)
