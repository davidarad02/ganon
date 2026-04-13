import logging
import socket
import struct
import threading
from typing import Optional

LOG_LEVEL_TRACE = 0
LOG_LEVEL_DEBUG = 1
LOG_LEVEL_INFO = 2

g_log_level = LOG_LEVEL_INFO


class GanonClient:
    def __init__(
        self,
        ip: str,
        port: int,
        connect_timeout: int = 5,
        reconnect_retries: int = 5,
        reconnect_delay: int = 5,
        log_level: int = LOG_LEVEL_INFO,
    ):
        self.ip = ip
        self.port = port
        self.connect_timeout = connect_timeout
        self.reconnect_retries = reconnect_retries
        self.reconnect_delay = reconnect_delay
        self.log_level = log_level

        self._sock: Optional[socket.socket] = None
        self._running = False
        self._lock = threading.Lock()

        self._logger = logging.getLogger("ganon_client")
        self._logger.setLevel(logging.DEBUG)

        if not self._logger.handlers:
            handler = logging.StreamHandler()
            handler.setFormatter(
                logging.Formatter(
                    "%(asctime)s [%(levelname)s] %(name)s: %(message)s",
                    datefmt="%Y-%m-%d %H:%M:%S",
                )
            )
            self._logger.addHandler(handler)

    def _log(self, level: int, msg: str, *args, **kwargs):
        if level >= self.log_level:
            level_map = {
                LOG_LEVEL_TRACE: logging.DEBUG - 1,
                LOG_LEVEL_DEBUG: logging.DEBUG,
                LOG_LEVEL_INFO: logging.INFO,
            }
            self._logger.log(level_map.get(level, logging.INFO), msg, *args, **kwargs)

    def _trace(self, msg: str, *args, **kwargs):
        self._log(LOG_LEVEL_TRACE, msg, *args, **kwargs)

    def _debug(self, msg: str, *args, **kwargs):
        self._log(LOG_LEVEL_DEBUG, msg, *args, **kwargs)

    def _info(self, msg: str, *args, **kwargs):
        self._log(LOG_LEVEL_INFO, msg, *args, **kwargs)

    def _warning(self, msg: str, *args, **kwargs):
        self._logger.warning(msg, *args, **kwargs)

    def _error(self, msg: str, *args, **kwargs):
        self._logger.error(msg, *args, **kwargs)

    def _connect(self) -> Optional[socket.socket]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.connect_timeout)

        try:
            self._info("Connecting to %s:%s...", self.ip, self.port)
            sock.connect((self.ip, self.port))
            self._info("Connected to %s:%s", self.ip, self.port)
            return sock
        except socket.timeout:
            self._warning("Connect to %s:%d timed out", self.ip, self.port)
            sock.close()
            return None
        except socket.error as e:
            self._warning("Failed to connect to %s:%d: %s", self.ip, self.port, e)
            sock.close()
            return None

    def connect(self) -> bool:
        with self._lock:
            if self._sock is not None:
                self._warning("Already connected to %s:%d", self.ip, self.port)
                return True

            sock = self._connect()
            if sock is None:
                return False

            self._sock = sock
            self._running = True
            return True

    def disconnect(self):
        with self._lock:
            if self._sock is not None:
                self._info("Disconnecting from %s:%d", self.ip, self.port)
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self._sock.close()
                self._sock = None
            self._running = False

    def is_connected(self) -> bool:
        with self._lock:
            return self._sock is not None

    def reconnect(self) -> bool:
        with self._lock:
            self._info("Reconnecting to %s:%d...", self.ip, self.port)

            if self._sock is not None:
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self._sock.close()
                self._sock = None

            retry = 0
            unlimited = self.reconnect_retries < 0

            while True:
                if not unlimited:
                    self._info(
                        "Reconnecting to %s:%d (attempt %d/%d)...",
                        self.ip,
                        self.port,
                        retry + 1,
                        self.reconnect_retries,
                    )
                else:
                    self._info(
                        "Reconnecting to %s:%d (attempt %d)...",
                        self.ip,
                        self.port,
                        retry + 1,
                    )

                sock = self._connect()
                if sock is not None:
                    self._sock = sock
                    self._running = True
                    self._info("Reconnected to %s:%d", self.ip, self.port)
                    return True

                if not unlimited and retry >= self.reconnect_retries - 1:
                    self._warning("All reconnect attempts failed, giving up on %s:%d", self.ip, self.port)
                    return False

                self._warning(
                    "Reconnect attempt %d failed, retrying in %ds",
                    retry + 1,
                    self.reconnect_delay,
                )
                retry += 1

    def send(self, data: bytes) -> int:
        with self._lock:
            if self._sock is None:
                raise ConnectionError("Not connected to %s:%d" % (self.ip, self.port))
            return self._sock.send(data)

    def recv(self, bufsize: int = 4096) -> bytes:
        with self._lock:
            if self._sock is None:
                raise ConnectionError("Not connected to %s:%d" % (self.ip, self.port))
            return self._sock.recv(bufsize)

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()
        return False

    def __del__(self):
        self.disconnect()
