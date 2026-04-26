"""
udp-quic skin (Python client side).

Uses aioquic for the QUIC transport.  Mirrors the C skin's framing:
  [4 bytes big-endian frame length][frame bytes]

ALPN: "ganon" (matches the C skin).
TLS: server certificate is NOT verified — ganon authentication is at the
protocol layer via node IDs.

Requires: aioquic  (pip install aioquic)
"""

import asyncio
import contextlib
import ssl
import struct
from typing import Optional

from aioquic.asyncio import connect as quic_connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import HandshakeCompleted, StreamDataReceived, ConnectionTerminated

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


# ── Protocol implementation ─────────────────────────────────────────────── #

class _GanonQuicProtocol(QuicConnectionProtocol):
    ALPN = "ganon"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._stream_id: Optional[int] = None
        self._rx_buf = bytearray()
        self._rx_ready: asyncio.Queue = asyncio.Queue()
        self._handshake_done: asyncio.Event = asyncio.Event()
        self._closed = False

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            self._stream_id = self._quic.get_next_available_stream_id()
            self._handshake_done.set()

        elif isinstance(event, StreamDataReceived):
            self._rx_buf += event.data
            self._drain_frames()

        elif isinstance(event, ConnectionTerminated):
            self._closed = True
            # Wake any waiting recv so it can return None
            self._rx_ready.put_nowait(None)

    def _drain_frames(self):
        while len(self._rx_buf) >= 4:
            frame_len = struct.unpack_from(">I", self._rx_buf)[0]
            if frame_len < 36 or frame_len > 300_000:
                self._rx_buf.clear()
                self._rx_ready.put_nowait(None)
                return
            total = 4 + frame_len
            if len(self._rx_buf) < total:
                break
            frame = bytes(self._rx_buf[4:total])
            del self._rx_buf[:total]
            self._rx_ready.put_nowait(frame)

    async def wait_connected(self, timeout: Optional[float] = None) -> None:
        if timeout is not None:
            await asyncio.wait_for(self._handshake_done.wait(), timeout=timeout)
        else:
            await self._handshake_done.wait()

    def send_frame(self, plaintext: bytes) -> None:
        if self._stream_id is None or self._closed:
            raise ConnectionError("QUIC connection not ready")
        self._quic.send_stream_data(
            self._stream_id, struct.pack(">I", len(plaintext)) + plaintext
        )
        self.transmit()

    async def recv_frame(self) -> Optional[bytes]:
        try:
            return await self._rx_ready.get()
        except asyncio.CancelledError:
            return None


# ── Skin implementation ─────────────────────────────────────────────────── #

class UdpQuicSkin(NetworkSkinImpl):
    """QUIC transport skin using ngtcp2-compatible ALPN 'ganon'."""

    def __init__(self, protocol: _GanonQuicProtocol, exit_stack):
        self._proto = protocol
        self._exit_stack = exit_stack

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "UdpQuicSkin":
        config = QuicConfiguration(
            alpn_protocols=[_GanonQuicProtocol.ALPN],
            is_client=True,
            verify_mode=ssl.CERT_NONE,
            server_name=ip,
        )

        exit_stack = contextlib.AsyncExitStack()
        try:
            ctx = quic_connect(
                ip,
                port,
                configuration=config,
                create_protocol=_GanonQuicProtocol,
            )
            protocol = await exit_stack.enter_async_context(ctx)
            await protocol.wait_connected(timeout=timeout)
            return cls(protocol, exit_stack)
        except asyncio.TimeoutError:
            await exit_stack.aclose()
            raise ConnectionError(
                f"QUIC connect to {ip}:{port} timed out after {timeout}s"
            )
        except Exception as e:
            await exit_stack.aclose()
            raise ConnectionError(
                f"QUIC connect to {ip}:{port} failed: {e}"
            ) from e

    async def send(self, plaintext: bytes) -> None:
        try:
            self._proto.send_frame(plaintext)
        except Exception as e:
            raise ConnectionError(f"QUIC send failed: {e}") from e

    async def recv(self) -> Optional[bytes]:
        try:
            return await self._proto.recv_frame()
        except Exception:
            return None

    def shutdown(self) -> None:
        with contextlib.suppress(Exception):
            self._proto.close()

    def close(self) -> None:
        with contextlib.suppress(Exception):
            self._proto.close()


register_skin(NetworkSkin.UDP_QUIC, UdpQuicSkin)
