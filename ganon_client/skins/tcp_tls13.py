import asyncio
import ssl
import struct
from typing import Optional

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


class TcpTls13Skin(NetworkSkinImpl):
    """TCP + TLS 1.3 via stdlib ssl.

    Connects with TLS 1.3 forced on both ends, ALPN "tls13-ganon",
    no certificate verification (ganon's NODE_INIT is the real auth).

    Wire frame format (identical to tcp-plain, inside the TLS stream):
        [4 bytes] big-endian payload length
        [N bytes] serialized protocol_msg_t + data
    """

    _ALPN = "tls13-ganon"

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self._reader = reader
        self._writer = writer

    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "TcpTls13Skin":
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.set_alpn_protocols([cls._ALPN])

        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(ip, port, ssl=ctx),
                timeout=timeout,
            )
        except asyncio.TimeoutError:
            raise ConnectionError(f"Connect to {ip}:{port} timed out after {timeout}s")
        except OSError as e:
            raise ConnectionError(f"Failed to connect to {ip}:{port}: {e}") from e

        return cls(reader, writer)

    # ------------------------------------------------------------------ #
    # I/O                                                                  #
    # ------------------------------------------------------------------ #

    async def send(self, plaintext: bytes) -> None:
        frame = struct.pack(">I", len(plaintext)) + plaintext
        try:
            self._writer.write(frame)
            await self._writer.drain()
        except OSError as e:
            raise ConnectionError(f"Failed to send frame: {e}") from e

    async def recv(self) -> Optional[bytes]:
        try:
            len_bytes = await self._reader.readexactly(4)
        except (asyncio.IncompleteReadError, OSError, ConnectionResetError):
            return None
        if not len_bytes:
            return None

        payload_len = struct.unpack(">I", len_bytes)[0]
        if payload_len < 36 or payload_len > 300_000:
            return None

        try:
            payload = await self._reader.readexactly(payload_len)
        except (asyncio.IncompleteReadError, OSError, ConnectionResetError):
            return None

        return payload

    # ------------------------------------------------------------------ #
    # Teardown                                                             #
    # ------------------------------------------------------------------ #

    def shutdown(self) -> None:
        try:
            self._writer.transport.abort()
        except OSError:
            pass

    def close(self) -> None:
        try:
            self._writer.close()
        except OSError:
            pass


register_skin(NetworkSkin.TCP_TLS13, TcpTls13Skin)
