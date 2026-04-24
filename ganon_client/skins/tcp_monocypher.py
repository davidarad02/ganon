import asyncio
import os
import socket
import struct
from typing import Optional

import monocypher.bindings as mc

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


class TcpMonocypherSkin(NetworkSkinImpl):
    """TCP + X25519 + BLAKE2b KDF + XChaCha20-Poly1305 skin.

    Wire frame format (post-handshake):
        [4  bytes] big-endian payload length (nonce + mac + ciphertext)
        [24 bytes] nonce (16 zero bytes || 8-byte little-endian counter)
        [16 bytes] Poly1305 MAC
        [N  bytes] ciphertext of plaintext
    """

    _NONCE_SIZE = 24
    _TAG_SIZE = 16

    def __init__(self, sock: socket.socket, send_key: bytes, recv_key: bytes):
        self._sock = sock
        self._send_key = send_key
        self._recv_key = recv_key
        self._send_nonce = 0
        self._recv_nonce = 0

    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "TcpMonocypherSkin":
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
    def _do_handshake(cls, sock: socket.socket) -> "TcpMonocypherSkin":
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
    def _make_nonce(counter: int) -> bytes:
        nonce = bytearray(24)
        nonce[16:24] = counter.to_bytes(8, "little")
        return bytes(nonce)

    async def send(self, plaintext: bytes) -> None:
        nonce = self._make_nonce(self._send_nonce)
        self._send_nonce += 1
        mac, ciphertext = mc.crypto_lock(self._send_key, nonce, plaintext)
        payload = nonce + mac + ciphertext
        frame = struct.pack(">I", len(payload)) + payload
        loop = asyncio.get_running_loop()
        try:
            await loop.sock_sendall(self._sock, frame)
        except OSError as e:
            raise ConnectionError(f"Failed to send encrypted frame: {e}") from e

    async def recv(self) -> Optional[bytes]:
        loop = asyncio.get_running_loop()
        try:
            len_bytes = await loop.sock_recv(self._sock, 4)
        except (OSError, ConnectionResetError):
            return None
        if not len_bytes:
            return None

        payload_len = struct.unpack(">I", len_bytes)[0]
        min_payload = self._NONCE_SIZE + self._TAG_SIZE
        if payload_len < min_payload or payload_len > 300_000:
            return None

        try:
            payload = await loop.sock_recv(self._sock, payload_len)
        except (OSError, ConnectionResetError):
            return None
        if not payload:
            return None

        nonce = payload[: self._NONCE_SIZE]
        mac = payload[self._NONCE_SIZE : self._NONCE_SIZE + self._TAG_SIZE]
        ciphertext = payload[self._NONCE_SIZE + self._TAG_SIZE :]

        counter = int.from_bytes(nonce[16:24], "little")
        if counter != self._recv_nonce:
            raise ValueError(
                f"Replay detected: expected {self._recv_nonce}, got {counter}"
            )
        self._recv_nonce += 1

        return mc.crypto_unlock(self._recv_key, mac, nonce, ciphertext)

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


register_skin(NetworkSkin.TCP_MONOCYPHER, TcpMonocypherSkin)
