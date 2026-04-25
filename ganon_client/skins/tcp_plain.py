import asyncio
import socket
import struct
from typing import Optional

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


class TcpPlainSkin(NetworkSkinImpl):
    """TCP with no encryption — length-prefixed raw protocol frames.

    Wire frame format:
        [4 bytes] big-endian payload length
        [N bytes] plaintext serialized protocol_msg_t + data
    """

    def __init__(self, sock: socket.socket):
        self._sock = sock

    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "TcpPlainSkin":
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            await asyncio.wait_for(
                loop.sock_connect(sock, (ip, port)),
                timeout=timeout,
            )
        except asyncio.TimeoutError:
            sock.close()
            raise ConnectionError(f"Connect to {ip}:{port} timed out after {timeout}s")
        except OSError as e:
            sock.close()
            raise ConnectionError(f"Failed to connect to {ip}:{port}: {e}") from e
        return cls(sock)

    # ------------------------------------------------------------------ #
    # I/O                                                                  #
    # ------------------------------------------------------------------ #

    async def send(self, plaintext: bytes) -> None:
        frame = struct.pack(">I", len(plaintext)) + plaintext
        loop = asyncio.get_running_loop()
        try:
            await loop.sock_sendall(self._sock, frame)
        except OSError as e:
            raise ConnectionError(f"Failed to send frame: {e}") from e

    async def recv(self) -> Optional[bytes]:
        loop = asyncio.get_running_loop()
        try:
            len_bytes = await loop.sock_recv(self._sock, 4)
        except (OSError, ConnectionResetError):
            return None
        if not len_bytes:
            return None

        payload_len = struct.unpack(">I", len_bytes)[0]
        if payload_len < 36 or payload_len > 300_000:
            return None

        try:
            payload = await loop.sock_recv(self._sock, payload_len)
        except (OSError, ConnectionResetError):
            return None
        if not payload:
            return None

        return payload

    # ------------------------------------------------------------------ #
    # Teardown                                                             #
    # ------------------------------------------------------------------ #

    def shutdown(self) -> None:
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass


register_skin(NetworkSkin.TCP_PLAIN, TcpPlainSkin)
