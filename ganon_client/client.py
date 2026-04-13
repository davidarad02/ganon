import logging
import socket
import struct
import threading
import time
from datetime import datetime
from typing import Callable, Optional

from ganon_client.protocol import DEFAULT_TTL, GANON_PROTOCOL_MAGIC, MsgType, ProtocolHeader
from ganon_client.routing import RoutingTable
from ganon_client.transport import Transport

LOG_LEVEL_TRACE = 0
LOG_LEVEL_DEBUG = 1
LOG_LEVEL_INFO = 2

COLOR_RESET = "\033[0m"
COLOR_RED = "\033[31m"
COLOR_YELLOW = "\033[33m"
COLOR_CYAN = "\033[36m"
COLOR_WHITE = "\033[37m"
COLOR_BOLD = "\033[1m"

LEVEL_COLORS = {
    "ERROR": COLOR_RED,
    "WARN": COLOR_YELLOW,
    "INFO": COLOR_CYAN,
    "DEBUG": COLOR_WHITE,
    "TRACE": COLOR_RESET,
}

LEVEL_WIDTH = 5


class GanonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        level = record.levelname
        if level == "WARNING":
            level = "WARN"
        color = LEVEL_COLORS.get(level, COLOR_RESET)

        timestamp = datetime.fromtimestamp(record.created).strftime("%Y-%m-%d %H:%M:%S")
        microseconds = int(record.created % 1 * 1000000)

        if len(level) <= 5:
            level_padded = level.ljust(5)
        else:
            level_padded = level

        if record.exc_info:
            exc_text = self.formatException(record.exc_info)
            message = record.getMessage() + "\n" + exc_text
        else:
            message = record.getMessage()

        filename = record.filename
        lineno = record.lineno

        return (
            f"{COLOR_BOLD}{timestamp}.{microseconds:06d}{COLOR_RESET} "
            f"[{COLOR_BOLD}{color}{level_padded}{COLOR_RESET}] "
            f"{message} "
            f"[{COLOR_BOLD}{filename}:{lineno}{COLOR_RESET}]"
        )


class GanonClient:
    def __init__(
        self,
        ip: str,
        port: int,
        node_id: int,
        connect_timeout: int = 5,
        reconnect_retries: int = 5,
        reconnect_delay: int = 5,
        log_level: int = LOG_LEVEL_INFO,
    ):
        self.ip = ip
        self.port = port
        self.node_id = node_id
        self.connect_timeout = connect_timeout
        self.reconnect_retries = reconnect_retries
        self.reconnect_delay = reconnect_delay
        self.log_level = log_level

        self._sock: Optional[socket.socket] = None
        self._running = False
        self._lock = threading.Lock()
        self._recv_thread: Optional[threading.Thread] = None
        self._reconnecting = False
        self._peer_node_id: Optional[int] = None

        self._routing_table = RoutingTable()

        self._on_data_received: Optional[Callable[[bytes], None]] = None
        self._on_disconnected: Optional[Callable[[], None]] = None
        self._on_reconnected: Optional[Callable[[], None]] = None

        self._logger = logging.getLogger("ganon_client")
        self._logger.setLevel(logging.DEBUG)

        if not self._logger.handlers:
            handler = logging.StreamHandler()
            handler.setFormatter(GanonFormatter())
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

    def set_on_data_received(self, callback: Callable[[bytes], None]):
        self._on_data_received = callback

    def set_on_disconnected(self, callback: Callable[[], None]):
        self._on_disconnected = callback

    def set_on_reconnected(self, callback: Callable[[], None]):
        self._on_reconnected = callback

    def _connect(self) -> Optional[socket.socket]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.connect_timeout)

        try:
            self._info("Connecting to %s:%s...", self.ip, self.port)
            sock.connect((self.ip, self.port))
            self._info("Connected to %s:%s", self.ip, self.port)
            sock.settimeout(None)
            return sock
        except socket.timeout:
            self._warning("Connect to %s:%d timed out", self.ip, self.port)
            sock.close()
            return None
        except socket.error as e:
            self._warning("Failed to connect to %s:%d: %s", self.ip, self.port, e)
            sock.close()
            return None

    def _handle_node_init(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        self._debug("Received NODE_INIT from node %d (orig_src=%d, msg_id=%d, ttl=%d, data_len=%d)", src_node_id, orig_src_node_id, message_id, ttl, len(data))

    def _handle_peer_info(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        self._debug("Received PEER_INFO from node %d (orig_src=%d, msg_id=%d, ttl=%d, data_len=%d)", src_node_id, orig_src_node_id, message_id, ttl, len(data))

        self._peer_node_id = src_node_id
        self._debug("Set peer_node_id to %d (direct connection)", src_node_id)

        peer_count = len(data) // 4
        peer_list = []
        for i in range(peer_count):
            peer_id = struct.unpack(">I", data[i*4:(i+1)*4])[0]
            peer_list.append(peer_id)
            self._routing_table.add_via_hop(peer_id, src_node_id)
            self._debug("  - peer %d: node %d", i, peer_id)

        self._debug("Routing table after PEER_INFO:")
        self._routing_table.log_table()

    def _process(self, header: dict, data: bytes):
        orig_src_node_id = header["orig_src_node_id"]
        src_node_id = header["src_node_id"]
        dst_node_id = header["dst_node_id"]
        message_id = header["message_id"]
        msg_type = MsgType(header["type"])
        data_length = header["data_length"]
        ttl = header["ttl"]

        self._info("Protocol: orig_src=%d, src=%d, dst=%d, msg_id=%d, type=%s, ttl=%d, data_len=%d", orig_src_node_id, src_node_id, dst_node_id, message_id, msg_type.name, ttl, data_length)

        if msg_type == MsgType.NODE_INIT:
            self._handle_node_init(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.PEER_INFO:
            self._handle_peer_info(orig_src_node_id, src_node_id, message_id, ttl, data)
        else:
            self._warning("Unknown message type: %d", header["type"])

    def _protocol_loop(self):
        header_size = 32
        transport = Transport(self._sock)
        while self._running:
            header_data = transport.recv_all(header_size)
            if header_data is None:
                self._handle_disconnect()
                return

            header = ProtocolHeader.parse(header_data)

            if header.magic != "GNN":
                self._warning("Invalid magic: expected %s, got %.4s", GANON_PROTOCOL_MAGIC, header.magic)
                self._handle_disconnect()
                return

            data_length = header.data_length
            data = b""
            if data_length > 0:
                data = transport.recv_all(data_length)
                if data is None:
                    self._handle_disconnect()
                    return

            self._process(header, data)

    def _handle_disconnect(self):
        with self._lock:
            if self._sock:
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    self._sock.close()
                except OSError:
                    pass
                self._sock = None

        if self._running and not self._reconnecting:
            self._reconnecting = True
            self._do_reconnect()
            self._reconnecting = False

    def _do_reconnect(self):
        retry = 0
        unlimited = self.reconnect_retries < 0

        while self._running:
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
                with self._lock:
                    self._sock = sock
                self._info("Reconnected to %s:%d", self.ip, self.port)
                if self._on_reconnected:
                    self._on_reconnected()
                self._start_recv_thread()
                return

            if not unlimited and retry >= self.reconnect_retries - 1:
                self._warning("All reconnect attempts failed, giving up on %s:%d", self.ip, self.port)
                self._running = False
                if self._on_disconnected:
                    self._on_disconnected()
                return

            self._warning(
                "Reconnect attempt %d failed, retrying in %ds",
                retry + 1,
                self.reconnect_delay,
            )
            time.sleep(self.reconnect_delay)
            retry += 1

    def _start_recv_thread(self):
        self._recv_thread = threading.Thread(target=self._protocol_loop, daemon=True)
        self._recv_thread.start()

    def _send_node_init(self):
        if self._sock is None:
            return
        
        header = b'GNN\x00'
        header += struct.pack(">I", self.node_id)  # orig_src_node_id
        header += struct.pack(">I", self.node_id)  # src_node_id
        header += struct.pack(">I", 0)              # dst_node_id (0 for broadcast)
        header += struct.pack(">I", 0)              # message_id
        header += struct.pack(">I", 0)              # type (NODE_INIT = 0)
        header += struct.pack(">I", 0)              # data_length
        header += struct.pack(">I", DEFAULT_TTL)   # ttl
        
        self._sock.sendall(header)

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
            self._send_node_init()
            self._start_recv_thread()
            return True

    def disconnect(self):
        with self._lock:
            self._running = False
            self._reconnecting = False

            if self._sock is not None:
                self._info("Disconnecting from %s:%d", self.ip, self.port)
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self._sock.close()
                self._sock = None

            if self._recv_thread is not None:
                self._recv_thread.join(timeout=2)
                self._recv_thread = None

    def is_connected(self) -> bool:
        with self._lock:
            return self._sock is not None and self._running

    def reconnect(self) -> bool:
        with self._lock:
            if self._running:
                self._running = False
                if self._sock:
                    try:
                        self._sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    self._sock.close()
                    self._sock = None

        self._running = True
        self._do_reconnect()
        return self._sock is not None

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
