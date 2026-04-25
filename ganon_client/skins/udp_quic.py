"""
udp-quic skin (Python client side).

Uses aioquic for the QUIC transport.  Mirrors the C skin's framing:
  [4 bytes big-endian frame length][frame bytes]

TLS: self-signed certificate for the server is NOT verified (ganon
authentication is handled at the protocol layer via node IDs).

Requires: aioquic  (`pip install aioquic`)
"""

import asyncio
import ssl
import struct
from typing import Optional

from aioquic.asyncio import connect as quic_connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import HandshakeCompleted, StreamDataReceived

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


# ── Protocol implementation ────────────────────────────────────────────────── #

class _GanonQuicProtocol(QuicConnectionProtocol):
    """Per-connection QUIC protocol handler for ganon."""

    ALPN = "ganon"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._stream_id: Optional[int] = None
        self._rx_buf = bytearray()
        self._rx_ready: asyncio.Queue = asyncio.Queue()
        self._handshake_done: asyncio.Event = asyncio.Event()

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            self._stream_id = self._quic.get_next_available_stream_id()
            print(f"[QUIC CLIENT] Handshake Completed, assigned stream {self._stream_id}")
            self._handshake_done.set()

        elif isinstance(event, StreamDataReceived):
            print(f"[QUIC CLIENT] StreamDataReceived({event.stream_id}): {len(event.data)} bytes")
            self._rx_buf += event.data
            self._drain_frames()

    def _drain_frames(self):
        while len(self._rx_buf) >= 4:
            frame_len = struct.unpack_from(">I", self._rx_buf)[0]
            print(f"[QUIC CLIENT] drain: buf_len={len(self._rx_buf)}, frame_len={frame_len}, hex={self._rx_buf[:16].hex()}")
            if frame_len < 36 or frame_len > 300_000:
                print(f"[QUIC CLIENT] drain: closing due to invalid frame_len={frame_len}")
                self._rx_buf.clear()
                return
            total = 4 + frame_len
            if len(self._rx_buf) < total:
                print(f"[QUIC CLIENT] drain: need {total} bytes, have {len(self._rx_buf)}")
                break
            frame = bytes(self._rx_buf[4:total])
            del self._rx_buf[:total]
            print(f"[QUIC CLIENT] drain: delivering frame of {len(frame)} bytes")
            self._rx_ready.put_nowait(frame)

    async def wait_connected(self, timeout: Optional[float] = None) -> None:
        if timeout is not None:
            await asyncio.wait_for(self._handshake_done.wait(), timeout=timeout)
        else:
            await self._handshake_done.wait()

    def send_frame(self, plaintext: bytes) -> None:
        header = struct.pack(">I", len(plaintext))
        self._quic.send_stream_data(self._stream_id, header + plaintext)
        self.transmit()

    async def recv_frame(self) -> Optional[bytes]:
        try:
            return await self._rx_ready.get()
        except asyncio.CancelledError:
            return None


# ── Skin implementation ────────────────────────────────────────────────────── #

class UdpQuicSkin(NetworkSkinImpl):
    """QUIC transport skin — UDP with TLS 1.3, no server cert verification."""

    def __init__(self, protocol: _GanonQuicProtocol):
        self._proto = protocol

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "UdpQuicSkin":
        config = QuicConfiguration(
            alpn_protocols=[_GanonQuicProtocol.ALPN],
            is_client=True,
            verify_mode=ssl.CERT_NONE,
            server_name=ip,
        )

        try:
            ctx = quic_connect(
                ip,
                port,
                configuration=config,
                create_protocol=_GanonQuicProtocol,
            )
            protocol = await ctx.__aenter__()
            await protocol.wait_connected(timeout=timeout)
            skin = cls(protocol)
            skin._ctx = ctx
            return skin
        except asyncio.TimeoutError:
            raise ConnectionError(
                f"QUIC connect to {ip}:{port} timed out after {timeout}s"
            )
        except Exception as e:
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
        try:
            self._proto.close()
        except Exception:
            pass

    def close(self) -> None:
        try:
            self._proto.close()
        except Exception:
            pass


register_skin(NetworkSkin.UDP_QUIC, UdpQuicSkin)
