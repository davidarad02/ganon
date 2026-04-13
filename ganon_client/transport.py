import socket
from typing import Optional


class Transport:
    def __init__(self, sock: socket.socket):
        self._sock = sock

    def recv(self, bufsize: int) -> Optional[bytes]:
        try:
            data = self._sock.recv(bufsize)
            if not data:
                return None
            return data
        except socket.timeout:
            return b""
        except OSError:
            return None

    def recv_all(self, size: int) -> Optional[bytes]:
        data = b""
        while len(data) < size:
            chunk = self.recv(size - len(data))
            if chunk is None:
                return None
            if chunk == b"":
                return None
            data += chunk
        return data

    def send(self, data: bytes) -> Optional[int]:
        try:
            return self._sock.send(data)
        except OSError:
            return None

    def send_all(self, data: bytes) -> bool:
        while len(data) > 0:
            sent = self.send(data)
            if sent is None:
                return False
            data = data[sent:]
        return True
