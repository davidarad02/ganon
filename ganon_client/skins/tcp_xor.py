import asyncio
import os
import socket
import struct
from typing import Optional

import monocypher.bindings as mc

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


class TcpXorSkin(NetworkSkinImpl):
    """TCP + X25519 handshake + repeating-key XOR obfuscation.

    Uses the same ephemeral key exchange as TcpMonocypherSkin, but instead
    of XChaCha20-Poly1305 it simply XORs each byte with the derived 32-byte
    directional key (key[i % 32]).  This provides per-connection obfuscation
    (different from source, different per link) but NOT cryptographic security.

    Wire frame format (post-handshake):
        [4 bytes] big-endian payload length
        [N bytes] XOR-obfuscated plaintext
    """

    def __init__(self, sock: socket.socket, send_key: bytes, recv_key: bytes):
        self._sock = sock
        self._send_key = send_key
        self._recv_key = recv_key

    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "TcpXorSkin":
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

        # Handshake uses sync monocypher bindings, run in executor
        sock.setblocking(True)
        try:
            instance = await loop.run_in_executor(None, cls._do_handshake, sock)
        except ConnectionError:
            sock.close()
            raise
        sock.setblocking(False)
        return instance

    @classmethod
    def _do_handshake(cls, sock: socket.socket) -> "TcpXorSkin":
        private_key = os.urandom(32)
        my_pub = mc.crypto_x25519_public_key(private_key)

        sock.sendall(my_pub)
        peer_pub = cls._recv_all_sync(sock, 32)
        if peer_pub is None:
            raise ConnectionError("Handshake failed: peer closed connection")

        shared = mc.crypto_x25519(private_key, peer_pub)
        send_key = mc.crypto_blake2b(b"S", key=shared, hash_size=32)
        recv_key = mc.crypto_blake2b(b"R", key=shared, hash_size=32)
        return cls(sock, send_key, recv_key)

    @staticmethod
    def _recv_all_sync(sock: socket.socket, size: int) -> Optional[bytes]:
        data = b""
        while len(data) < size:
            try:
                chunk = sock.recv(size - len(data))
            except (socket.timeout, OSError):
                return None
            if not chunk:
                return None
            data += chunk
        return data

    # ------------------------------------------------------------------ #
    # I/O                                                                  #
    # ------------------------------------------------------------------ #

    @staticmethod
    def _xor_buf(data: bytes, key: bytes) -> bytes:
        key_len = len(key)
        result = bytearray(data)
        # Process in chunks to reduce Python-level loop overhead.
        extended = key * (len(result) // key_len + 1)
        for i in range(len(result)):
            result[i] ^= extended[i]
        return bytes(result)

    async def send(self, plaintext: bytes) -> None:
        obf = self._xor_buf(plaintext, self._send_key)
        frame = struct.pack(">I", len(obf)) + obf
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

        return self._xor_buf(payload, self._recv_key)

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


register_skin(NetworkSkin.TCP_XOR, TcpXorSkin)
