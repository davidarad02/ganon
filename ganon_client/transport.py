import hashlib
import os
import socket
import struct
from typing import Optional

import monocypher.bindings as mc


class Transport:
    """Transport wrapper that encrypts all traffic using Monocypher XChaCha20.

    Wire format (post-handshake):
        [4 bytes] big-endian payload length (nonce + mac + ciphertext)
        [24 bytes] nonce (16 zero bytes + 8-byte LE counter)
        [16 bytes] Poly1305 authentication tag
        [N bytes]  ciphertext of plaintext

    This matches the C implementation exactly (Monocypher crypto_aead_lock).
    """

    _NONCE_SIZE = 24
    _TAG_SIZE = 16

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._established = False
        self._send_key: Optional[bytes] = None
        self._recv_key: Optional[bytes] = None
        self._send_nonce = 0
        self._recv_nonce = 0

    # ------------------------------------------------------------------
    # Raw socket helpers (used only during handshake)
    # ------------------------------------------------------------------

    def _recv_raw(self, size: int) -> Optional[bytes]:
        data = b""
        while len(data) < size:
            try:
                chunk = self._sock.recv(size - len(data))
            except (socket.timeout, OSError):
                return None
            if not chunk:
                return None
            data += chunk
        return data

    def _send_raw(self, data: bytes) -> bool:
        try:
            self._sock.sendall(data)
            return True
        except OSError:
            return False

    # ------------------------------------------------------------------
    # Encryption handshake
    # ------------------------------------------------------------------

    def do_handshake(self, is_initiator: bool) -> bool:
        """Perform X25519 key exchange and derive ChaCha20-Poly1305 keys."""
        private_key = os.urandom(32)
        my_pub = mc.crypto_x25519_public_key(private_key)

        if is_initiator:
            if not self._send_raw(my_pub):
                return False
            peer_pub = self._recv_raw(32)
            if peer_pub is None:
                return False
        else:
            peer_pub = self._recv_raw(32)
            if peer_pub is None:
                return False
            if not self._send_raw(my_pub):
                return False

        shared = mc.crypto_x25519(private_key, peer_pub)

        # Derive directional keys (same as C: BLAKE2b with single-byte contexts)
        # Note: monocypher-py crypto_blake2b(msg, key=...) takes msg as the
        # *message* and key as the *key*.  The C crypto_blake2b_keyed() has
        # arguments (hash, hash_size, key, key_size, message, message_size).
        # To match the C code we must pass the single-byte context as the
        # *message* and the shared secret as the *key*.
        if is_initiator:
            self._send_key = mc.crypto_blake2b(b"S", key=shared, hash_size=32)
            self._recv_key = mc.crypto_blake2b(b"R", key=shared, hash_size=32)
        else:
            self._recv_key = mc.crypto_blake2b(b"S", key=shared, hash_size=32)
            self._send_key = mc.crypto_blake2b(b"R", key=shared, hash_size=32)

        self._send_nonce = 0
        self._recv_nonce = 0
        self._established = True
        return True

    # ------------------------------------------------------------------
    # Encrypted send / recv
    # ------------------------------------------------------------------

    def send_encrypted(self, plaintext: bytes) -> bool:
        """Encrypt *plaintext* and send it as a length-prefixed frame."""
        if not self._established:
            raise RuntimeError("Encryption not established")

        nonce = self._make_nonce(self._send_nonce)
        self._send_nonce += 1

        mac, ciphertext = mc.crypto_lock(self._send_key, nonce, plaintext)

        payload = nonce + mac + ciphertext
        length_prefix = struct.pack(">I", len(payload))

        return self._send_raw(length_prefix + payload)

    def recv_decrypted(self) -> Optional[bytes]:
        """Read a length-prefixed encrypted frame and return the plaintext."""
        if not self._established:
            raise RuntimeError("Encryption not established")

        len_bytes = self._recv_raw(4)
        if len_bytes is None:
            return None

        payload_len = struct.unpack(">I", len_bytes)[0]
        min_payload = self._NONCE_SIZE + self._TAG_SIZE
        if payload_len < min_payload or payload_len > 200_000:
            return None

        payload = self._recv_raw(payload_len)
        if payload is None:
            return None

        nonce = payload[: self._NONCE_SIZE]
        mac = payload[self._NONCE_SIZE : self._NONCE_SIZE + self._TAG_SIZE]
        ciphertext = payload[self._NONCE_SIZE + self._TAG_SIZE :]

        # Replay protection
        counter = int.from_bytes(nonce[16:24], "little")
        if counter != self._recv_nonce:
            raise ValueError(
                f"Replay detected: expected {self._recv_nonce}, got {counter}"
            )
        self._recv_nonce += 1

        plaintext = mc.crypto_unlock(self._recv_key, mac, nonce, ciphertext)
        if plaintext is None:
            return None

        return plaintext

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def shutdown(self, how: int = socket.SHUT_RDWR) -> None:
        self._sock.shutdown(how)

    def close(self) -> None:
        self._sock.close()

    @staticmethod
    def _make_nonce(counter: int) -> bytes:
        nonce = bytearray(24)
        nonce[16:24] = counter.to_bytes(8, "little")
        return bytes(nonce)
