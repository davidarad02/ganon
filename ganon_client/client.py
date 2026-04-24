import logging
import socket
import struct
import threading
import time
from datetime import datetime
from typing import Callable, Optional
from functools import wraps

def require_connection(func):
    @wraps(func)
    def wrapper(self, *args, **kwargs):
        if self._sock is None or not self._running:
            raise ConnectionError("Client is not connected to the network. Please call connect() first.")
        return func(self, *args, **kwargs)
    return wrapper

from ganon_client.protocol import (
    DEFAULT_TTL, GANON_PROTOCOL_MAGIC, MsgType, ProtocolHeader,
    TUNNEL_PROTO_TCP, TUNNEL_PROTO_UDP, TunnelOpenPayload, TunnelClosePayload,
    ConnectCmdPayload, ConnectResponsePayload, DisconnectCmdPayload, DisconnectResponsePayload,
    FILE_STATUS_SUCCESS, FILE_STATUS_NOT_FOUND, FILE_STATUS_NO_SPACE,
    FILE_STATUS_READ_ONLY, FILE_STATUS_PERMISSION, FILE_STATUS_OTHER,
)
from ganon_client.routing import RouteType, RoutingTable
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


class Tunnel:
    """Represents an active tunnel created via GanonClient.create_tunnel()."""

    def __init__(self, client: "GanonClient", tunnel_id: int, src_node_id: int,
                 dst_node_id: int, src_host: str, src_port: int,
                 remote_host: str, remote_port: int, protocol: int):
        self._client = client
        self.tunnel_id = tunnel_id
        self._src_node_id = src_node_id
        self._dst_node_id = dst_node_id
        self._src_host = src_host
        self._src_port = src_port
        self._remote_host = remote_host
        self._remote_port = remote_port
        self._protocol = protocol
        self._up = True

    @property
    def is_up(self) -> bool:
        return self._up and self._client.is_connected()

    def close(self, force: bool = False):
        """Close the tunnel.
        
        Args:
            force: If True, immediately terminate all existing connections.
                   If False (default), stop accepting new connections but allow
                   existing connections to continue until they close naturally.
        """
        if not self._up:
            return
        self._up = False
        flags = 1 if force else 0
        payload = TunnelClosePayload.build({"tunnel_id": self.tunnel_id, "flags": flags})
        self._client._send_protocol_message(self._src_node_id, struct.pack(">I", MsgType.TUNNEL_CLOSE.value), payload)
        self._client._remove_tunnel(self.tunnel_id)

    def _mark_closed(self):
        self._up = False

    def __repr__(self) -> str:
        state = "up" if self.is_up else "down"
        return (f"Tunnel(id={self.tunnel_id}, "
                f"{self._src_host}:{self._src_port} -> "
                f"node{self._dst_node_id}:{self._remote_host}:{self._remote_port}, "
                f"{state})")


class NodeClient:
    """Bound client that targets a specific node for all commands.

    Obtained via ``GanonClient.node(node_id)``.  Every method that normally
    takes a *target_node_id* as its first argument is pre-filled with the
    bound node id, so you can write::

        nc = c.node(30)
        nc.run_command("uptime")
        nc.upload_file("/local/file.txt", "/remote/file.txt")
        nc.ping()

    For ``create_tunnel`` the bound node id is used as *src_node_id*, so
    you only specify the destination side::

        nc.create_tunnel(11, "0.0.0.0", 8000, "127.0.0.1", 9000, "tcp")
    """

    def __init__(self, client: "GanonClient", node_id: int):
        self._client = client
        self.node_id = node_id

    def run_command(self, cmd: str, timeout: float = 30.0) -> dict:
        return self._client.run_command(self.node_id, cmd, timeout)

    def run(self, cmd: str, timeout: float = 30.0) -> bytes:
        return self._client.run(self.node_id, cmd, timeout)

    def upload_file(self, local_path: str, remote_path: str,
                    timeout: float = 60.0) -> dict:
        return self._client.upload_file(self.node_id, local_path, remote_path, timeout)

    def download_file(self, remote_path: str, local_path: str,
                      timeout: float = 60.0) -> dict:
        return self._client.download_file(self.node_id, remote_path, local_path, timeout)

    def ping(self, timeout: float = 5.0, channel_id: int = 0) -> float:
        return self._client.ping(self.node_id, timeout, channel_id)

    def send_to_node(self, data: bytes, channel_id: int = 0) -> bool:
        return self._client.send_to_node(self.node_id, data, channel_id)

    def create_tunnel(self, dst_node_id: int,
                      src_host: str, src_port: int,
                      remote_host: str, remote_port: int,
                      protocol: str = "tcp") -> Tunnel:
        return self._client.create_tunnel(
            self.node_id, dst_node_id,
            src_host, src_port, remote_host, remote_port, protocol
        )

    def connect_to_node(self, ip: str, port: int, timeout: float = 10.0) -> dict:
        return self._client.connect_to_node(ip, port, self.node_id, timeout)

    def disconnect_nodes(self, node_b: int) -> dict:
        return self._client.disconnect_nodes(self.node_id, node_b)

    def __getattr__(self, name):
        """Proxy everything else to the underlying client."""
        return getattr(self._client, name)

    def __repr__(self) -> str:
        return f"NodeClient(node_id={self.node_id}, client={self._client!r})"


class GanonClient:
    def __init__(
        self,
        ip: str,
        port: int,
        node_id: int,
        connect_timeout: int = 5,
        reconnect_retries: int = 5,
        reconnect_delay: int = 5,
        log_level: int = LOG_LEVEL_DEBUG,
        reorder_timeout: int = 100,
        reorder: bool = False,
    ):
        self.ip = ip
        self.port = port
        self.node_id = node_id
        self.connect_timeout = connect_timeout
        self.reconnect_retries = reconnect_retries
        self.reconnect_delay = reconnect_delay
        self.log_level = log_level
        self.reorder_timeout = reorder_timeout
        self.reorder = reorder  # False = process packets immediately (default)

        self._sock: Optional[Transport] = None
        self._running = False
        self._lock = threading.RLock()
        self._recv_thread: Optional[threading.Thread] = None
        self._reconnecting = False
        self._peer_node_id: Optional[int] = None

        self._routing_table = RoutingTable()

        self._on_data_received: Optional[Callable[[bytes], None]] = None
        self._on_disconnected: Optional[Callable[[], None]] = None
        self._on_reconnected: Optional[Callable[[], None]] = None

        self._pending_pings = {}
        self._ping_lock = threading.Lock()

        self._pending_execs = {}     # request_id -> (event, result_dict)
        self._pending_uploads = {}   # request_id -> (event, result_dict)
        self._pending_downloads = {} # request_id -> (event, result_dict)
        self._rpc_lock = threading.Lock()
        self._rpc_counter = 0

        self._tunnels: dict = {}
        self._tunnel_id_counter = 0
        self._tunnel_lock = threading.Lock()
        
        self._msg_seq = 0  # Will start from 1 on first message (pre-increment)
        
        self._seen_msgs = set() # (orig_src, msg_id)
        self._expected_msg_id = {} # orig_src -> expected_id
        self._reorder_buffer = {} # orig_src -> {msg_id -> (header, data, ts)}
        
        # Pending messages buffer for route discovery
        self._pending_lock = threading.Lock()
        self._pending_messages: dict = {}  # dst_node_id -> list of buffered messages
        self._reorder_thread: Optional[threading.Thread] = None
        self._lb_lock = threading.Lock()

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

    def _connect(self) -> Optional[Transport]:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.connect_timeout)

        try:
            self._info("Connecting to %s:%s...", self.ip, self.port)
            sock.connect((self.ip, self.port))
            self._info("Connected to %s:%s", self.ip, self.port)
            sock.settimeout(None)
        except socket.timeout:
            self._warning("Connect to %s:%d timed out", self.ip, self.port)
            sock.close()
            return None
        except socket.error as e:
            self._warning("Failed to connect to %s:%d: %s", self.ip, self.port, e)
            sock.close()
            return None

        # Transport-layer encryption handshake (mandatory)
        transport = Transport(sock)
        self._info("Starting encryption handshake...")
        if not transport.do_handshake(is_initiator=True):
            self._error("Encryption handshake failed")
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
            return None
        self._info("Encryption handshake complete")
        return transport

    def _handle_node_init(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        self._debug("Received NODE_INIT from node %d (orig_src=%d, msg_id=%d, ttl=%d, data_len=%d)", src_node_id, orig_src_node_id, message_id, ttl, len(data))
        
        # Reset session state for this node
        with self._lb_lock:
            # Clear seen messages from this source
            self._seen_msgs = {item for item in self._seen_msgs if item[0] != orig_src_node_id}
            # Reset expected message ID
            if orig_src_node_id in self._expected_msg_id:
                del self._expected_msg_id[orig_src_node_id]
            # Clear reorder buffer for this source
            if orig_src_node_id in self._reorder_buffer:
                del self._reorder_buffer[orig_src_node_id]

        self._peer_node_id = src_node_id
        self._routing_table.add_direct(src_node_id, None)
        self._debug("Set peer_node_id to %d (direct connection)", src_node_id)
        self._debug("Routing table after NODE_INIT:")
        self._routing_table.log_table()

    def _send_rreq(self, target_node_id: int):
        """Send a route request (RREQ) to discover a path to target_node_id."""
        self._info("Sending RREQ to discover route to node %d", target_node_id)
        payload = struct.pack(">I", target_node_id)
        self._send_protocol_message(0, struct.pack(">I", MsgType.RREQ.value), payload, 
                                    channel_id=0, bypass_route_check=True)
    
    def _flush_pending_messages(self, node_id: int):
        """Flush buffered messages for node_id now that we have a route."""
        with self._pending_lock:
            if node_id not in self._pending_messages:
                return
            
            messages = self._pending_messages[node_id]
            self._info("Flushing %d buffered messages for node %d", len(messages), node_id)
            
            for msg in messages:
                self._send_protocol_message(node_id, msg['msg_type'], msg['data'], 
                                             msg['channel_id'], bypass_route_check=True)
            
            del self._pending_messages[node_id]
    
    def _handle_rreq(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        if len(data) >= 4:
            target_node_id = struct.unpack(">I", data[:4])[0]
            if target_node_id == self.node_id:
                self._info("Received RREQ for us from node %d! Sending RREP.", orig_src_node_id)
                self._send_protocol_message(orig_src_node_id, struct.pack(">I", MsgType.RREP.value), b"")
    
    def _handle_rrep(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        """Handle RREP - route discovered, flush any pending messages."""
        self._info("Received RREP from node %d via node %d (msg_id=%d) - route established!", 
                   orig_src_node_id, src_node_id, message_id)
        # Add the route
        hop_count = max(1, DEFAULT_TTL - ttl)
        self._routing_table.add_via_hop(orig_src_node_id, src_node_id, hop_count)
        self._debug("Routing table after RREP:")
        self._routing_table.log_table()
        # Flush any pending messages for this destination
        self._flush_pending_messages(orig_src_node_id)
    
    def _handle_rerr(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        count = len(data) // 4
        for i in range(count):
            lost_node = struct.unpack(">I", data[i*4:(i+1)*4])[0]
            if lost_node != self.node_id:
                # Optimized RERR: only remove if the reporter is our actual next-hop for this destination
                # or if it's the lost node itself (direct disconnect)
                routes = self._routing_table.get_all_routes(lost_node)
                is_via_reporter = False
                if lost_node == src_node_id:
                    # If the reporter reports itself lost, we only remove if it's not a direct connection
                    # (Direct connections are managed by socket lifecycle)
                    for route in routes:
                        if route.route_type == RouteType.VIA_HOP:
                            is_via_reporter = True
                            break
                else:
                    for route in routes:
                        if route.route_type == RouteType.VIA_HOP and route.next_hop_node_id == src_node_id:
                            is_via_reporter = True
                            break
                
                if is_via_reporter:
                    self._debug("Removing route to %d via %d due to RERR", lost_node, src_node_id)
                    self._routing_table.remove(lost_node)
                else:
                    self._debug("Ignoring RERR for %d from %d (not our next-hop)", lost_node, src_node_id)
        
        self._debug("Routing table after RERR:")
        self._routing_table.log_table()

    def _handle_user_data(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        if self._on_data_received:
            self._on_data_received(data)

    def _handle_ping(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes, channel_id: int = 0):
        self._info("Received PING from node %d. Echoing PONG.", orig_src_node_id)
        msg_type_bytes = struct.pack(">I", MsgType.PONG.value)
        self._send_protocol_message(orig_src_node_id, msg_type_bytes, data, channel_id=channel_id)
        
    def _handle_pong(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        self._info("Received PONG from node %d! Payload Length: %d", orig_src_node_id, len(data))
        with self._ping_lock:
            if data in self._pending_pings:
                self._pending_pings[data].set()

    def _handle_exec_response(self, orig_src_node_id: int, data: bytes):
        if len(data) < 16:
            self._warning("EXEC_RESPONSE too short (%d bytes)", len(data))
            return
        request_id = struct.unpack(">I", data[:4])[0]
        exit_code = struct.unpack(">i", data[4:8])[0]  # signed int for -1
        stdout_len = struct.unpack(">I", data[8:12])[0]
        stderr_len = struct.unpack(">I", data[12:16])[0]
        stdout_data = data[16:16+stdout_len]
        stderr_data = data[16+stdout_len:16+stdout_len+stderr_len]

        with self._rpc_lock:
            if request_id in self._pending_execs:
                event, result = self._pending_execs[request_id]
                result["exit_code"] = exit_code
                result["stdout"] = stdout_data
                result["stderr"] = stderr_data
                event.set()

    def _handle_file_upload_response(self, orig_src_node_id: int, data: bytes):
        if len(data) < 8:
            self._warning("FILE_UPLOAD_RESPONSE too short (%d bytes)", len(data))
            return
        request_id = struct.unpack(">I", data[:4])[0]
        status = struct.unpack(">I", data[4:8])[0]
        error_msg = data[8:].split(b"\x00")[0].decode("utf-8", errors="replace")

        with self._rpc_lock:
            if request_id in self._pending_uploads:
                event, result = self._pending_uploads[request_id]
                result["status"] = status
                result["error"] = error_msg if status != 0 else ""
                event.set()

    def _handle_file_download_response(self, orig_src_node_id: int, data: bytes):
        if len(data) < 8:
            self._warning("FILE_DOWNLOAD_RESPONSE too short (%d bytes)", len(data))
            return
        request_id = struct.unpack(">I", data[:4])[0]
        status = struct.unpack(">I", data[4:8])[0]

        with self._rpc_lock:
            if request_id in self._pending_downloads:
                event, result = self._pending_downloads[request_id]
                result["status"] = status
                if status == 0:
                    result["data"] = data[8:]
                    result["error"] = ""
                else:
                    result["data"] = b""
                    result["error"] = data[8:].split(b"\x00")[0].decode("utf-8", errors="replace")
                event.set()

    def _process(self, header: dict, data: bytes):
        orig_src_node_id = header["orig_src_node_id"]
        src_node_id = header["src_node_id"]
        dst_node_id = header["dst_node_id"]
        message_id = header["message_id"]
        msg_type = MsgType(header["type"])
        data_length = header["data_length"]
        ttl = header["ttl"]

        channel_id = header.get("channel_id", 0)
        self._info("Protocol RECV: orig_src=%d, src=%d, dst=%d, msg_id=%d, type=%s, ttl=%d, channel=%d, data_len=%d", orig_src_node_id, src_node_id, dst_node_id, message_id, msg_type.name, ttl, channel_id, data_length)

        # Learn paths from all passing messages
        if orig_src_node_id != self.node_id:
            if orig_src_node_id != src_node_id:
                hop_count = DEFAULT_TTL - ttl if DEFAULT_TTL > ttl else 1
                self._routing_table.add_via_hop(orig_src_node_id, src_node_id, hop_count)
            else:
                self._routing_table.add_direct(src_node_id, None)

        if message_id != 0 and self.reorder:
            with self._lb_lock:
                if (orig_src_node_id, message_id) in self._seen_msgs:
                    self._debug("Dropping duplicate message %d from %d", message_id, orig_src_node_id)
                    return
                self._seen_msgs.add((orig_src_node_id, message_id))
                if len(self._seen_msgs) > 1024:
                    # Very primitive cache eviction
                    self._seen_msgs.clear()
                
                expected = self._expected_msg_id.get(orig_src_node_id, 0)
                if expected == 0:
                    self._expected_msg_id[orig_src_node_id] = message_id + 1
                elif message_id == expected:
                    self._expected_msg_id[orig_src_node_id] = message_id + 1
                    self._dispatch_message(msg_type, orig_src_node_id, src_node_id, message_id, ttl, data, channel_id=channel_id)
                    
                    # Check buffer
                    self._flush_buffer(orig_src_node_id)
                    return
                elif (expected - message_id) & 0xFFFFFFFF < 0x7FFFFFFF:
                    self._debug("Dropping late message %d from %d (expected %d)", message_id, orig_src_node_id, expected)
                    return
                else:
                    self._debug("Buffering out-of-order message %d from %d (expected %d)", message_id, orig_src_node_id, expected)
                    buf = self._reorder_buffer.setdefault(orig_src_node_id, {})
                    buf[message_id] = (header, data, time.time())
                    
                    # Optional: handle timeout/flush here if needed
                    self._check_reorder_timeouts(orig_src_node_id)
                    return
        
        self._dispatch_message(msg_type, orig_src_node_id, src_node_id, message_id, ttl, data, channel_id=channel_id)

    def _reorder_loop(self):
        while self._running:
            with self._lb_lock:
                orig_sources = list(self._reorder_buffer.keys())
                for orig_src in orig_sources:
                    self._check_reorder_timeouts(orig_src)
            time.sleep(self.reorder_timeout / 2000.0) # sleep half the timeout

    def _check_reorder_timeouts(self, orig_src):
        now = time.time()
        buf = self._reorder_buffer.get(orig_src, {})
        if not buf:
            return

        expected = self._expected_msg_id.get(orig_src, 0)
        
        # Find oldest message in buffer
        msg_ids = sorted(buf.keys())
        any_timeout = False
        for m_id in msg_ids:
            header_b, data_b, ts = buf[m_id]
            if now - ts > self.reorder_timeout / 1000.0:
                any_timeout = True
                break
        
        if any_timeout:
            self._debug("Reorder buffer timeout for node %d, flushing all", orig_src)
            self._flush_buffer(orig_src, force=True)

    def _flush_buffer(self, orig_src, force=False):
        while True:
            buf = self._reorder_buffer.get(orig_src, {})
            msg_ids = sorted(buf.keys())
            if not msg_ids:
                break
                
            m_id = msg_ids[0]
            expected = self._expected_msg_id.get(orig_src, 0)
            if (expected - m_id) & 0xFFFFFFFF < 0x7FFFFFFF:
                # Drop late packet
                buf.pop(m_id)
                continue
            elif force or m_id == expected:
                header_b, data_b, _ = buf.pop(m_id)
                self._expected_msg_id[orig_src] = m_id + 1
                self._dispatch_message(MsgType(header_b["type"]), orig_src, header_b["src_node_id"], m_id, header_b["ttl"], data_b, channel_id=header_b.get("channel_id", 0))
            else:
                break

    def _dispatch_message(self, msg_type, orig_src_node_id, src_node_id, message_id, ttl, data, channel_id: int = 0):
        if msg_type == MsgType.NODE_INIT:
            self._handle_node_init(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.RREQ:
            self._handle_rreq(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.RERR:
            self._handle_rerr(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.USER_DATA:
            self._handle_user_data(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.PING:
            self._handle_ping(orig_src_node_id, src_node_id, message_id, ttl, data, channel_id=channel_id)
        elif msg_type == MsgType.PONG:
            self._handle_pong(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.RREP:
            self._handle_rrep(orig_src_node_id, src_node_id, message_id, ttl, data)
        elif msg_type == MsgType.EXEC_RESPONSE:
            self._handle_exec_response(orig_src_node_id, data)
        elif msg_type == MsgType.FILE_UPLOAD_RESPONSE:
            self._handle_file_upload_response(orig_src_node_id, data)
        elif msg_type == MsgType.FILE_DOWNLOAD_RESPONSE:
            self._handle_file_download_response(orig_src_node_id, data)
        elif msg_type == MsgType.CONNECTION_REJECTED:
            pass
        elif msg_type in (MsgType.TUNNEL_OPEN, MsgType.TUNNEL_CONN_OPEN, MsgType.TUNNEL_CONN_ACK,
                          MsgType.TUNNEL_DATA, MsgType.TUNNEL_CONN_CLOSE, MsgType.TUNNEL_CLOSE):
            pass  # Tunnel messages are handled by C nodes; Python client is pass-through
        else:
            self._warning("Unknown message type: %s", msg_type)

    def _protocol_loop(self):
        while self._running:
            plaintext = self._sock.recv_decrypted()
            if plaintext is None:
                self._handle_disconnect()
                return

            if len(plaintext) < 36:
                self._warning("Decrypted frame too short: %d bytes", len(plaintext))
                self._handle_disconnect()
                return

            header_data = plaintext[:36]
            data = plaintext[36:]

            header = ProtocolHeader.parse(header_data)

            if header.magic != "GNN":
                self._warning("Invalid magic: expected %s, got %.4s", GANON_PROTOCOL_MAGIC, header.magic)
                self._handle_disconnect()
                return

            if len(data) != header.data_length:
                self._warning("Data length mismatch: header says %d, got %d",
                              header.data_length, len(data))
                self._handle_disconnect()
                return

            self._process(header, data)

    def _close_all_tunnels(self):
        with self._tunnel_lock:
            for tunnel in list(self._tunnels.values()):
                tunnel._mark_closed()
            self._tunnels.clear()

    def _remove_tunnel(self, tunnel_id: int):
        with self._tunnel_lock:
            self._tunnels.pop(tunnel_id, None)

    def _handle_disconnect(self):
        self._close_all_tunnels()
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
        self._running = True # Ensure we keep trying

        while self._running:
            if not unlimited and retry >= self.reconnect_retries:
                self._warning("All reconnect attempts failed, giving up on %s:%d", self.ip, self.port)
                self._running = False
                if self._on_disconnected:
                    self._on_disconnected()
                return

            self._info("Reconnecting to %s:%d (attempt %d%s)...", 
                      self.ip, self.port, retry + 1, "" if unlimited else f"/{self.reconnect_retries}")

            sock = self._connect()
            if sock is not None:
                with self._lock:
                    self._sock = sock
                self._info("Reconnected to %s:%d", self.ip, self.port)
                self._setup_session()
                return

            self._warning("Reconnect attempt %d failed, retrying in %ds", retry + 1, self.reconnect_delay)
            time.sleep(self.reconnect_delay)
            retry += 1

    def _setup_session(self):
        self._send_node_init()
        if self._on_reconnected:
            self._on_reconnected()
        self._start_background_threads()

    def _send_node_init(self):
        if self._sock is None:
            return

        self._msg_seq += 1
        header = ProtocolHeader.build({
            "magic": "GNN",
            "orig_src_node_id": self.node_id,
            "src_node_id": self.node_id,
            "dst_node_id": 0,
            "message_id": self._msg_seq,
            "type": MsgType.NODE_INIT,
            "data_length": 0,
            "ttl": DEFAULT_TTL,
            "channel_id": 0,
        })

        self._sock.send_encrypted(header)

    def _start_background_threads(self):
        with self._lock:
            if self._recv_thread is None or not self._recv_thread.is_alive():
                self._recv_thread = threading.Thread(target=self._protocol_loop, daemon=True)
                self._recv_thread.start()
            if self.reorder and (self._reorder_thread is None or not self._reorder_thread.is_alive()) and self.reorder_timeout > 0:
                self._reorder_thread = threading.Thread(target=self._reorder_loop, daemon=True)
                self._reorder_thread.start()

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
            self._start_background_threads()
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
            
            if self._reorder_thread is not None:
                self._reorder_thread.join(timeout=2)
                self._reorder_thread = None

    def node(self, node_id: int) -> NodeClient:
        """Return a NodeClient bound to *node_id*.

        All node-targeted methods (run_command, run, upload_file,
        download_file, ping, send_to_node, create_tunnel, etc.) will
        automatically use this node id as the target.
        """
        return NodeClient(self, node_id)

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

    @require_connection
    def send(self, data: bytes) -> int:
        with self._lock:
            return self._sock.send(data)

    @require_connection
    def send_to_node(self, dst_node_id: int, data: bytes, channel_id: int = 0) -> bool:
        with self._lock:
            msg_type_bytes = struct.pack(">I", MsgType.USER_DATA.value)
            return self._send_protocol_message(dst_node_id, msg_type_bytes, data, channel_id=channel_id)

    @require_connection
    def ping(self, dst_node_id: int, timeout: float = 5.0, channel_id: int = 0) -> float:
        import os
        import time
        ping_data = os.urandom(16)
        event = threading.Event()

        with self._ping_lock:
            self._pending_pings[ping_data] = event

        self._info("Pinging node %d with %d bytes of random data...", dst_node_id, len(ping_data))
        msg_type_bytes = struct.pack(">I", MsgType.PING.value)
        
        start_time = time.time()

        if not self._send_protocol_message(dst_node_id, msg_type_bytes, ping_data, channel_id=channel_id):
            with self._ping_lock:
                if ping_data in self._pending_pings:
                    del self._pending_pings[ping_data]
            raise ConnectionError(f"Failed to transmit PING to node {dst_node_id}")

        # Block until PONG or timeout
        received = event.wait(timeout)

        with self._ping_lock:
            if ping_data in self._pending_pings:
                del self._pending_pings[ping_data]

        if received:
            elapsed_ms = (time.time() - start_time) * 1000.0
            return elapsed_ms
        else:
            raise TimeoutError(f"Ping to node {dst_node_id} timed out after {timeout} seconds (Node not reachable or doesn't exist)")

    @require_connection
    def create_tunnel(self, src_node_id: int, dst_node_id: int,
                      src_host: str, src_port: int,
                      remote_host: str, remote_port: int,
                      protocol: str = "tcp") -> Tunnel:
        proto_byte = TUNNEL_PROTO_UDP if protocol.lower() == "udp" else TUNNEL_PROTO_TCP

        with self._tunnel_lock:
            self._tunnel_id_counter += 1
            tunnel_id = self._tunnel_id_counter

        payload = TunnelOpenPayload.build({
            "tunnel_id":   tunnel_id,
            "dst_node_id": dst_node_id,
            "src_port":    src_port,
            "remote_port": remote_port,
            "protocol":    proto_byte,
            "pad":         b"\x00\x00\x00",
            "src_host":    src_host,
            "remote_host": remote_host,
        })

        self._send_protocol_message(
            src_node_id,
            struct.pack(">I", MsgType.TUNNEL_OPEN.value),
            payload,
            channel_id=0,
        )
        self._info("Tunnel %d: TUNNEL_OPEN sent to node %d (%s:%d -> node%d:%s:%d, %s)",
                   tunnel_id, src_node_id, src_host, src_port,
                   dst_node_id, remote_host, remote_port, protocol)

        tunnel = Tunnel(self, tunnel_id, src_node_id, dst_node_id,
                        src_host, src_port, remote_host, remote_port, proto_byte)
        with self._tunnel_lock:
            self._tunnels[tunnel_id] = tunnel
        return tunnel

    def _send_protocol_message(self, dst_node_id: int, msg_type: bytes, data: bytes, channel_id: int = 0, 
                                bypass_route_check: bool = False) -> bool:
        if self._sock is None:
            return False
        
        # Check if we have a route to destination (unless bypassing for RREQ etc.)
        if not bypass_route_check and dst_node_id != 0 and dst_node_id != self.node_id:
            route = self._routing_table.get_route(dst_node_id)
            if route is None:
                # No route - buffer message and trigger route discovery
                self._info("No route to node %d, buffering message and sending RREQ", dst_node_id)
                with self._pending_lock:
                    if dst_node_id not in self._pending_messages:
                        self._pending_messages[dst_node_id] = []
                    self._pending_messages[dst_node_id].append({
                        'msg_type': msg_type,
                        'data': data,
                        'channel_id': channel_id,
                        'timestamp': time.time()
                    })
                
                # Send RREQ to discover route
                self._send_rreq(dst_node_id)
                return True  # Message buffered, will be sent when route is established

        self._msg_seq += 1
        header = ProtocolHeader.build({
            "magic": "GNN",
            "orig_src_node_id": self.node_id,
            "src_node_id": self.node_id,
            "dst_node_id": dst_node_id,
            "message_id": self._msg_seq,
            "type": struct.unpack(">I", msg_type)[0],
            "data_length": len(data),
            "ttl": DEFAULT_TTL,
            "channel_id": channel_id,
        })

        try:
            self._sock.send_encrypted(header + data)
            msg_type_val = struct.unpack(">I", msg_type)[0]
            msg_type_name = MsgType(msg_type_val).name if msg_type_val in [t.value for t in MsgType] else f"UNKNOWN({msg_type_val})"
            self._info("Protocol SEND: orig_src=%d, src=%d, dst=%d, msg_id=%d, type=%s, ttl=%d, channel=%d, data_len=%d",
                       self.node_id, self.node_id, dst_node_id, self._msg_seq, msg_type_name, DEFAULT_TTL, channel_id, len(data))
            self._debug("Sent message to node %d (type=%s, data_len=%d, channel=%d)", dst_node_id, msg_type.hex(), len(data), channel_id)
            return True
        except Exception as e:
            self._warning("Failed to send to node %d: %s", dst_node_id, e)
            return False

    @require_connection
    def recv(self, bufsize: int = 4096) -> bytes:
        with self._lock:
            return self._sock.recv(bufsize)

    @require_connection
    def connect_to_node(self, ip: str, port: int, target_node_id: int = None, timeout: float = 10.0) -> dict:
        """Connect a node to a new peer.
        
        Args:
            ip: IP address of the peer to connect to
            port: Port of the peer to connect to
            target_node_id: Which node should perform the connection (default: local node)
            timeout: Maximum time to wait for response in seconds
            
        Returns:
            dict with keys: 'success' (bool), 'status' (str), 'error_code' (int)
            
        Raises:
            ConnectionError: If the client is not connected
            TimeoutError: If no response received within timeout
        """
        # Determine which node will execute the connect
        executor_node = target_node_id if target_node_id is not None else self.node_id
        
        payload = ConnectCmdPayload.build({
            "target_ip": ip,
            "target_port": port,
        })
        
        self._info("Requesting node %d to connect to %s:%d", executor_node, ip, port)
        
        # Send CONNECT_CMD message
        self._send_protocol_message(
            executor_node,
            struct.pack(">I", MsgType.CONNECT_CMD.value),
            payload,
            channel_id=0,
        )
        
        # Wait for response (in a real implementation, this would need proper response handling)
        # For now, we return a pending status
        return {"success": None, "status": "pending", "error_code": 0}
    
    @require_connection  
    def disconnect_nodes(self, node_a: int, node_b: int = None) -> dict:
        """Disconnect two nodes from each other.
        
        Args:
            node_a: First node to disconnect (or the local node if node_b is None)
            node_b: Second node to disconnect (if None, disconnect node_a from local node)
            
        Returns:
            dict with keys: 'success' (bool), 'status' (str), 'error_code' (int)
            
        Raises:
            ConnectionError: If the client is not connected
        """
        if node_b is None:
            # Disconnect local node from node_a
            executor_node = self.node_id
            target_node = node_a
        else:
            # Disconnect node_a from node_b (may require forwarding)
            executor_node = node_a
            target_node = node_b
            
        payload = DisconnectCmdPayload.build({
            "node_a": executor_node,
            "node_b": target_node,
        })
        
        self._info("Requesting disconnect between node %d and node %d", executor_node, target_node)
        
        # Send DISCONNECT_CMD message to executor_node (it will handle the disconnect)
        self._send_protocol_message(
            executor_node,
            struct.pack(">I", MsgType.DISCONNECT_CMD.value),
            payload,
            channel_id=0,
        )
        
        return {"success": None, "status": "pending", "error_code": 0}

    @require_connection
    def print_network_graph(self) -> None:
        """Print a visual graph of the network topology from this client's perspective.
        
        Properly handles loops and parallel routes. Shows all known paths to each node.
        """
        self._info("Network topology from node %d perspective:", self.node_id)
        print("\n" + "=" * 70)
        print(f"NETWORK GRAPH (Node {self.node_id} perspective)")
        print("=" * 70)
        
        # Get all routes from the routing table
        all_routes = {}
        with self._routing_table._lock:
            for node_id, entries in self._routing_table._entries.items():
                all_routes[node_id] = list(entries)
        
        if not all_routes:
            print("\n[No known routes - network appears empty]")
            print("=" * 70 + "\n")
            return
        
        # Build adjacency list showing who we connect to
        direct_peers = set()
        route_graph = {}  # node_id -> list of (next_hop, hop_count, route_type)
        
        for node_id, entries in all_routes.items():
            for entry in entries:
                if entry.route_type == RouteType.DIRECT:
                    direct_peers.add(node_id)
                
                if node_id not in route_graph:
                    route_graph[node_id] = []
                route_graph[node_id].append({
                    'next_hop': entry.next_hop_node_id,
                    'hops': entry.hop_count,
                    'type': entry.route_type,
                    'fd': entry.fd
                })
        
        # Print this node
        print(f"\n[{self.node_id}] (this node - you)")
        print("    |")
        
        # Print direct connections section
        if direct_peers:
            print("    |---[DIRECT CONNECTIONS]")
            for peer_id in sorted(direct_peers):
                print(f"    |\\")
                print(f"    | \\____[{peer_id}] (direct peer)")
                
                # Show what this peer can reach (excluding ourselves and already shown direct peers)
                if peer_id in route_graph:
                    reachable_via_peer = []
                    for route in route_graph[peer_id]:
                        target = route['next_hop']
                        # Avoid showing: ourselves, the peer itself, other direct peers
                        if target != self.node_id and target != peer_id and target not in direct_peers:
                            reachable_via_peer.append((target, route['hops']))
                    
                    if reachable_via_peer:
                        reachable_via_peer = sorted(set(reachable_via_peer))  # Remove duplicates
                        for i, (target, hops) in enumerate(reachable_via_peer):
                            is_last = (i == len(reachable_via_peer) - 1)
                            prefix = "    |      |" if not is_last else "    |      "
                            print(f"    |      |")
                            print(f"{prefix}\\____[{target}] ({hops} hops via {peer_id})")
        
        # Print indirect routes section
        indirect_nodes = {}
        for node_id, routes in route_graph.items():
            if node_id in direct_peers:
                continue
            # Collect all unique paths to this node
            paths = []
            for route in routes:
                next_hop = route['next_hop']
                hops = route['hops']
                # Build the path string
                if next_hop == node_id:
                    paths.append(f"[{node_id}] (self-loop?)")
                elif next_hop in direct_peers:
                    paths.append(f"via {next_hop} ({hops} hops)")
                else:
                    paths.append(f"via {next_hop} ({hops} hops, chained)")
            
            if paths:
                indirect_nodes[node_id] = paths
        
        if indirect_nodes:
            print("    |")
            print("    |---[INDIRECT ROUTES]")
            for node_id in sorted(indirect_nodes.keys()):
                paths = indirect_nodes[node_id]
                print(f"    |\\")
                print(f"    | \\____[{node_id}]")
                for i, path in enumerate(paths):
                    is_last = (i == len(paths) - 1)
                    prefix = "    |    |" if not is_last else "    |    "
                    print(f"    |    |")
                    print(f"{prefix}\\-> {path}")
        
        # Detect and report potential loops or parallel routes
        parallel_routes = []
        loop_routes = []
        for node_id, routes in route_graph.items():
            direct_count = sum(1 for r in routes if r['type'] == RouteType.DIRECT)
            indirect_count = sum(1 for r in routes if r['type'] == RouteType.VIA_HOP)
            
            if direct_count > 0 and indirect_count > 0:
                parallel_routes.append(node_id)
            
            # Check for loops (route where next_hop leads back to self or creates cycle)
            for route in routes:
                if route['next_hop'] == node_id and route['type'] == RouteType.VIA_HOP:
                    loop_routes.append((node_id, route['next_hop']))
        
        if parallel_routes or loop_routes:
            print("\n    |")
            print("    |---[NETWORK CHARACTERISTICS]")
            if parallel_routes:
                print("    |")
                print("    |  Parallel routes detected (direct + indirect):")
                for node_id in sorted(parallel_routes):
                    routes = route_graph.get(node_id, [])
                    direct = [r for r in routes if r['type'] == RouteType.DIRECT]
                    indirect = [r for r in routes if r['type'] == RouteType.VIA_HOP]
                    print(f"    |    Node {node_id}: {len(direct)} direct, {len(indirect)} indirect paths")
            if loop_routes:
                print("    |")
                print("    |  Potential routing loops detected:")
                for node_id, next_hop in sorted(set(loop_routes)):
                    print(f"    |    Node {node_id} -> Node {next_hop} (circular reference)")
        
        # Print summary
        total_nodes = len(all_routes)
        direct_count = len(direct_peers)
        indirect_count = len(indirect_nodes)
        
        print("\n" + "=" * 70)
        print(f"SUMMARY: {direct_count} direct peers, {indirect_count} reachable via hops, "
              f"{total_nodes} total known nodes")
        if parallel_routes:
            print(f"         {len(parallel_routes)} nodes have parallel routes")
        print("=" * 70 + "\n")

    def _alloc_request_id(self) -> int:
        with self._rpc_lock:
            self._rpc_counter += 1
            return self._rpc_counter

    @require_connection
    def run_command(self, target_node_id: int, cmd: str, timeout: float = 30.0) -> dict:
        """Execute a command on a remote node and return separated stdout/stderr.

        Args:
            target_node_id: Node to execute the command on.
            cmd: Shell command to execute.
            timeout: Maximum time to wait for response in seconds.

        Returns:
            dict with keys: 'exit_code' (int), 'stdout' (bytes), 'stderr' (bytes)

        Raises:
            TimeoutError: If no response received within timeout.
            ConnectionError: If the client is not connected.
        """
        request_id = self._alloc_request_id()
        event = threading.Event()
        result = {}

        with self._rpc_lock:
            self._pending_execs[request_id] = (event, result)

        payload = struct.pack(">I", request_id) + cmd.encode("utf-8") + b"\x00"
        msg_type_bytes = struct.pack(">I", MsgType.EXEC_CMD.value)

        self._info("Executing command on node %d (req=%d): %s", target_node_id, request_id, cmd)

        if not self._send_protocol_message(target_node_id, msg_type_bytes, payload, channel_id=0):
            with self._rpc_lock:
                self._pending_execs.pop(request_id, None)
            raise ConnectionError(f"Failed to send EXEC_CMD to node {target_node_id}")

        if not event.wait(timeout):
            with self._rpc_lock:
                self._pending_execs.pop(request_id, None)
            raise TimeoutError(f"Command execution on node {target_node_id} timed out after {timeout}s")

        with self._rpc_lock:
            self._pending_execs.pop(request_id, None)

        return {
            "exit_code": result.get("exit_code", -1),
            "stdout": result.get("stdout", b""),
            "stderr": result.get("stderr", b""),
        }

    @require_connection
    def run(self, target_node_id: int, cmd: str, timeout: float = 30.0) -> bytes:
        """Execute a command on a remote node and return undifferentiated output.

        This is equivalent to running with '2>&1' — stdout and stderr are merged.

        Args:
            target_node_id: Node to execute the command on.
            cmd: Shell command to execute.
            timeout: Maximum time to wait for response in seconds.

        Returns:
            Combined stdout and stderr as bytes.

        Raises:
            TimeoutError: If no response received within timeout.
            ConnectionError: If the client is not connected.
        """
        result = self.run_command(target_node_id, cmd, timeout)
        return result["stdout"] + result["stderr"]

    @require_connection
    def upload_file(self, target_node_id: int, local_path: str, remote_path: str,
                    timeout: float = 60.0) -> dict:
        """Upload a file to a remote node.

        Args:
            target_node_id: Node to upload the file to.
            local_path: Path to the local file.
            remote_path: Destination path on the remote node.
            timeout: Maximum time to wait for response in seconds.

        Returns:
            dict with keys: 'success' (bool), 'error' (str)

        Raises:
            TimeoutError: If no response received within timeout.
            ConnectionError: If the client is not connected.
            FileNotFoundError: If the local file does not exist.
        """
        with open(local_path, "rb") as f:
            file_data = f.read()

        request_id = self._alloc_request_id()
        event = threading.Event()
        result = {}

        with self._rpc_lock:
            self._pending_uploads[request_id] = (event, result)

        path_bytes = remote_path.encode("utf-8")
        if len(path_bytes) > 255:
            raise ValueError("remote_path must be <= 255 bytes")

        payload = struct.pack(">I", request_id)
        payload += path_bytes.ljust(256, b"\x00")
        payload += file_data

        msg_type_bytes = struct.pack(">I", MsgType.FILE_UPLOAD.value)

        self._info("Uploading %s -> node %d:%s (%d bytes, req=%d)",
                   local_path, target_node_id, remote_path, len(file_data), request_id)

        if not self._send_protocol_message(target_node_id, msg_type_bytes, payload, channel_id=0):
            with self._rpc_lock:
                self._pending_uploads.pop(request_id, None)
            raise ConnectionError(f"Failed to send FILE_UPLOAD to node {target_node_id}")

        if not event.wait(timeout):
            with self._rpc_lock:
                self._pending_uploads.pop(request_id, None)
            raise TimeoutError(f"File upload to node {target_node_id} timed out after {timeout}s")

        with self._rpc_lock:
            self._pending_uploads.pop(request_id, None)

        status = result.get("status", FILE_STATUS_OTHER)
        return {
            "success": status == FILE_STATUS_SUCCESS,
            "error": result.get("error", "Unknown error"),
        }

    @require_connection
    def download_file(self, target_node_id: int, remote_path: str, local_path: str,
                      timeout: float = 60.0) -> dict:
        """Download a file from a remote node.

        Args:
            target_node_id: Node to download the file from.
            remote_path: Path to the file on the remote node.
            local_path: Destination path on the local machine.
            timeout: Maximum time to wait for response in seconds.

        Returns:
            dict with keys: 'success' (bool), 'error' (str)

        Raises:
            TimeoutError: If no response received within timeout.
            ConnectionError: If the client is not connected.
        """
        request_id = self._alloc_request_id()
        event = threading.Event()
        result = {}

        with self._rpc_lock:
            self._pending_downloads[request_id] = (event, result)

        payload = struct.pack(">I", request_id) + remote_path.encode("utf-8") + b"\x00"
        msg_type_bytes = struct.pack(">I", MsgType.FILE_DOWNLOAD.value)

        self._info("Downloading node %d:%s -> %s (req=%d)",
                   target_node_id, remote_path, local_path, request_id)

        if not self._send_protocol_message(target_node_id, msg_type_bytes, payload, channel_id=0):
            with self._rpc_lock:
                self._pending_downloads.pop(request_id, None)
            raise ConnectionError(f"Failed to send FILE_DOWNLOAD to node {target_node_id}")

        if not event.wait(timeout):
            with self._rpc_lock:
                self._pending_downloads.pop(request_id, None)
            raise TimeoutError(f"File download from node {target_node_id} timed out after {timeout}s")

        with self._rpc_lock:
            self._pending_downloads.pop(request_id, None)

        status = result.get("status", FILE_STATUS_OTHER)
        if status == FILE_STATUS_SUCCESS:
            file_data = result.get("data", b"")
            with open(local_path, "wb") as f:
                f.write(file_data)
            return {"success": True, "error": ""}
        else:
            return {"success": False, "error": result.get("error", "Unknown error")}

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()
        return False

    def __del__(self):
        self.disconnect()
