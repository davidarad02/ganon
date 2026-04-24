import logging
import asyncio
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


class _AsyncBridge:
    """Bridges a coroutine running on the client's internal loop to the caller's async context."""

    def __init__(self, coro, loop):
        self._coro = coro
        self._loop = loop

    def __await__(self):
        future = asyncio.run_coroutine_threadsafe(self._coro, self._loop)
        wrapped = asyncio.wrap_future(future)
        return wrapped.__await__()


from ganon_client.protocol import (
    DEFAULT_TTL, GANON_PROTOCOL_MAGIC, MsgType, ProtocolHeader,
    TUNNEL_PROTO_TCP, TUNNEL_PROTO_UDP, TunnelOpenPayload, TunnelClosePayload,
    ConnectCmdPayload, ConnectResponsePayload, DisconnectCmdPayload, DisconnectResponsePayload,
    CONNECT_STATUS_SUCCESS, CONNECT_STATUS_REFUSED, CONNECT_STATUS_TIMEOUT, CONNECT_STATUS_ERROR,
    FILE_STATUS_SUCCESS, FILE_STATUS_NOT_FOUND, FILE_STATUS_NO_SPACE,
    FILE_STATUS_READ_ONLY, FILE_STATUS_PERMISSION, FILE_STATUS_OTHER,
    FILE_CHUNK_SIZE, FileUploadPayload, FileDownloadPayload, FileDownloadResponseHeader,
)
from ganon_client.routing import RouteType, RoutingTable
from ganon_client.skin import NetworkSkin, NetworkSkinImpl, get_skin_impl
import ganon_client.skins  # noqa: F401 — registers all skins

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


class ConnectToNodeError(ConnectionError):
    """Raised when a CONNECT_CMD fails at the remote node.

    Attributes:
        status: Human-readable status string ('refused', 'timeout', 'error').
        error_code: Raw error code returned by the remote node.
    """

    def __init__(self, message: str, status: str, error_code: int):
        super().__init__(message)
        self.status = status
        self.error_code = error_code


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
        self._client._sync(
            self._client._send_protocol_message(
                self._src_node_id,
                struct.pack(">I", MsgType.TUNNEL_CLOSE.value),
                payload,
                channel_id=0,
            )
        )
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

    def a_run_command(self, cmd: str, timeout: float = 30.0):
        return self._client.a_run_command(self.node_id, cmd, timeout)

    def run_command(self, cmd: str, timeout: float = 30.0) -> dict:
        return self._client.run_command(self.node_id, cmd, timeout)

    def a_run(self, cmd: str, timeout: float = 30.0):
        return self._client.a_run(self.node_id, cmd, timeout)

    def run(self, cmd: str, timeout: float = 30.0) -> bytes:
        return self._client.run(self.node_id, cmd, timeout)

    def a_upload_file(self, local_path: str, remote_path: str,
                      timeout: float = 60.0):
        return self._client.a_upload_file(self.node_id, local_path, remote_path, timeout)

    def upload_file(self, local_path: str, remote_path: str,
                    timeout: float = 60.0) -> dict:
        return self._client.upload_file(self.node_id, local_path, remote_path, timeout)

    def a_download_file(self, remote_path: str, local_path: str,
                        timeout: float = 60.0):
        return self._client.a_download_file(self.node_id, remote_path, local_path, timeout)

    def download_file(self, remote_path: str, local_path: str,
                      timeout: float = 60.0) -> dict:
        return self._client.download_file(self.node_id, remote_path, local_path, timeout)

    def a_ping(self, timeout: float = 5.0, channel_id: int = 0):
        return self._client.a_ping(self.node_id, timeout, channel_id)

    def ping(self, timeout: float = 5.0, channel_id: int = 0) -> float:
        return self._client.ping(self.node_id, timeout, channel_id)

    def a_send_to_node(self, data: bytes, channel_id: int = 0):
        return self._client.a_send_to_node(self.node_id, data, channel_id)

    def send_to_node(self, data: bytes, channel_id: int = 0) -> None:
        return self._client.send_to_node(self.node_id, data, channel_id)

    def a_create_tunnel(self, dst_node_id: int,
                        src_host: str, src_port: int,
                        remote_host: str, remote_port: int,
                        protocol: str = "tcp"):
        return self._client.a_create_tunnel(
            self.node_id, dst_node_id,
            src_host, src_port, remote_host, remote_port, protocol
        )

    def create_tunnel(self, dst_node_id: int,
                      src_host: str, src_port: int,
                      remote_host: str, remote_port: int,
                      protocol: str = "tcp") -> Tunnel:
        return self._client.create_tunnel(
            self.node_id, dst_node_id,
            src_host, src_port, remote_host, remote_port, protocol
        )

    def a_connect_to_node(self, ip: str, port: int, skin: NetworkSkin = None, timeout: float = 10.0):
        return self._client.a_connect_to_node(ip, port, self.node_id, skin=skin, timeout=timeout)

    def connect_to_node(self, ip: str, port: int, skin: NetworkSkin = None, timeout: float = 10.0) -> "NodeClient":
        return self._client.connect_to_node(ip, port, target_node_id=self.node_id, skin=skin, timeout=timeout)

    def a_disconnect_nodes(self, node_b: int):
        return self._client.a_disconnect_nodes(self.node_id, node_b)

    def disconnect_nodes(self, node_b: int) -> None:
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
        skin: NetworkSkin = NetworkSkin.TCP_MONOCYPHER,
        file_chunk_size: int = FILE_CHUNK_SIZE,
    ):
        self.ip = ip
        self.port = port
        self.node_id = node_id
        self.connect_timeout = connect_timeout
        self.reconnect_retries = reconnect_retries
        self.reconnect_delay = reconnect_delay
        self.log_level = log_level
        self.reorder_timeout = reorder_timeout
        self.reorder = reorder
        self._skin = skin
        self._file_chunk_size = file_chunk_size

        self._sock: Optional[NetworkSkinImpl] = None
        self._running = False
        self._lock = threading.RLock()
        self._reconnecting = False
        self._peer_node_id: Optional[int] = None

        self._loop = None
        self._loop_thread = None
        self._protocol_task = None
        self._reorder_task = None

        self._routing_table = RoutingTable()

        self._on_data_received: Optional[Callable[[bytes], None]] = None
        self._on_disconnected: Optional[Callable[[], None]] = None
        self._on_reconnected: Optional[Callable[[], None]] = None

        self._pending_pings = {}
        self._ping_lock = threading.Lock()

        self._pending_execs = {}
        self._pending_uploads = {}
        self._pending_downloads = {}
        self._pending_connects = {}
        self._rpc_lock = threading.Lock()
        self._rpc_counter = 0

        self._tunnels: dict = {}
        self._tunnel_id_counter = 0
        self._tunnel_lock = threading.Lock()
        
        self._msg_seq = 0
        
        self._seen_msgs = set()
        self._expected_msg_id = {}
        self._reorder_buffer = {}
        
        self._pending_lock = threading.Lock()
        self._pending_messages: dict = {}
        self._lb_lock = threading.Lock()

        self._logger = logging.getLogger("ganon_client")
        self._logger.setLevel(logging.DEBUG)

        if not self._logger.handlers:
            handler = logging.StreamHandler()
            handler.setFormatter(GanonFormatter())
            self._logger.addHandler(handler)

    def _start_event_loop(self):
        if self._loop is not None and self._loop.is_running():
            return
        self._loop = asyncio.new_event_loop()
        self._loop_thread = threading.Thread(target=self._run_loop, daemon=True)
        self._loop_thread.start()

    def _run_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def _sync(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self._loop).result()

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

    async def _a_open_skin(self) -> NetworkSkinImpl:
        impl_class = get_skin_impl(self._skin)
        if impl_class is None:
            raise ConnectionError(f"No implementation registered for skin {self._skin.name}")
        self._info("Connecting to %s:%s (skin=%s)...", self.ip, self.port, self._skin.name)
        transport = await impl_class.open(self.ip, self.port, self.connect_timeout)
        self._info("Connected and handshake complete: %s:%s", self.ip, self.port)
        return transport

    def _handle_node_init(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        self._debug("Received NODE_INIT from node %d (orig_src=%d, msg_id=%d, ttl=%d, data_len=%d)", src_node_id, orig_src_node_id, message_id, ttl, len(data))
        
        with self._lb_lock:
            self._seen_msgs = {item for item in self._seen_msgs if item[0] != orig_src_node_id}
            if orig_src_node_id in self._expected_msg_id:
                del self._expected_msg_id[orig_src_node_id]
            if orig_src_node_id in self._reorder_buffer:
                del self._reorder_buffer[orig_src_node_id]

        self._peer_node_id = src_node_id
        self._routing_table.add_direct(src_node_id, None)
        self._debug("Set peer_node_id to %d (direct connection)", src_node_id)
        self._debug("Routing table after NODE_INIT:")
        self._routing_table.log_table()

    async def _send_rreq(self, target_node_id: int):
        """Send a route request (RREQ) to discover a path to target_node_id."""
        self._info("Sending RREQ to discover route to node %d", target_node_id)
        payload = struct.pack(">I", target_node_id)
        await self._send_protocol_message(0, struct.pack(">I", MsgType.RREQ.value), payload, 
                                          channel_id=0, bypass_route_check=True)
    
    async def _flush_pending_messages(self, node_id: int):
        """Flush buffered messages for node_id now that we have a route."""
        with self._pending_lock:
            if node_id not in self._pending_messages:
                return
            
            messages = self._pending_messages[node_id]
            self._info("Flushing %d buffered messages for node %d", len(messages), node_id)
            del self._pending_messages[node_id]
        
        for msg in messages:
            await self._send_protocol_message(node_id, msg['msg_type'], msg['data'], 
                                               msg['channel_id'], bypass_route_check=True)
    
    def _handle_rreq(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        if len(data) >= 4:
            target_node_id = struct.unpack(">I", data[:4])[0]
            if target_node_id == self.node_id:
                self._info("Received RREQ for us from node %d! Sending RREP.", orig_src_node_id)
                asyncio.create_task(self._send_protocol_message(orig_src_node_id, struct.pack(">I", MsgType.RREP.value), b""))
    
    def _handle_rrep(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        """Handle RREP - route discovered, flush any pending messages."""
        self._info("Received RREP from node %d via node %d (msg_id=%d) - route established!", 
                   orig_src_node_id, src_node_id, message_id)
        hop_count = max(1, DEFAULT_TTL - ttl)
        self._routing_table.add_via_hop(orig_src_node_id, src_node_id, hop_count)
        self._debug("Routing table after RREP:")
        self._routing_table.log_table()
        asyncio.create_task(self._flush_pending_messages(orig_src_node_id))
    
    def _handle_rerr(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        count = len(data) // 4
        for i in range(count):
            lost_node = struct.unpack(">I", data[i*4:(i+1)*4])[0]
            if lost_node != self.node_id:
                routes = self._routing_table.get_all_routes(lost_node)
                is_via_reporter = False
                if lost_node == src_node_id:
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
        asyncio.create_task(self._send_protocol_message(orig_src_node_id, msg_type_bytes, data, channel_id=channel_id))
        
    def _handle_pong(self, orig_src_node_id: int, src_node_id: int, message_id: int, ttl: int, data: bytes):
        self._info("Received PONG from node %d! Payload Length: %d", orig_src_node_id, len(data))
        with self._ping_lock:
            if data in self._pending_pings:
                future, _ = self._pending_pings[data]
                future.set_result(None)

    def _handle_exec_response(self, orig_src_node_id: int, data: bytes):
        if len(data) < 16:
            self._warning("EXEC_RESPONSE too short (%d bytes)", len(data))
            return
        request_id = struct.unpack(">I", data[:4])[0]
        exit_code = struct.unpack(">i", data[4:8])[0]
        stdout_len = struct.unpack(">I", data[8:12])[0]
        stderr_len = struct.unpack(">I", data[12:16])[0]
        stdout_data = data[16:16+stdout_len]
        stderr_data = data[16+stdout_len:16+stdout_len+stderr_len]

        with self._rpc_lock:
            if request_id in self._pending_execs:
                future, result = self._pending_execs[request_id]
                result["exit_code"] = exit_code
                result["stdout"] = stdout_data
                result["stderr"] = stderr_data
                future.set_result(None)

    def _handle_file_upload_response(self, orig_src_node_id: int, data: bytes):
        if len(data) < 8:
            self._warning("FILE_UPLOAD_RESPONSE too short (%d bytes)", len(data))
            return
        request_id = struct.unpack(">I", data[:4])[0]
        status = struct.unpack(">I", data[4:8])[0]
        error_msg = data[8:].split(b"\x00")[0].decode("utf-8", errors="replace")

        with self._rpc_lock:
            if request_id in self._pending_uploads:
                future, result = self._pending_uploads[request_id]
                result["status"] = status
                result["error"] = error_msg if status != 0 else ""
                future.set_result(None)

    def _handle_file_download_response(self, orig_src_node_id: int, data: bytes):
        # Response format: request_id(4) + status(4) + total_size(4) + data
        if len(data) < 12:
            # Fallback for legacy format: request_id(4) + status(4) + data
            if len(data) < 8:
                self._warning("FILE_DOWNLOAD_RESPONSE too short (%d bytes)", len(data))
                return
            request_id = struct.unpack(">I", data[:4])[0]
            status = struct.unpack(">I", data[4:8])[0]
            total_size = 0
            payload = data[8:]
        else:
            request_id = struct.unpack(">I", data[:4])[0]
            status = struct.unpack(">I", data[4:8])[0]
            total_size = struct.unpack(">I", data[8:12])[0]
            payload = data[12:]

        with self._rpc_lock:
            if request_id in self._pending_downloads:
                future, result = self._pending_downloads[request_id]
                result["status"] = status
                result["total_size"] = total_size
                if status == 0:
                    result["data"] = payload
                    result["error"] = ""
                else:
                    result["data"] = b""
                    result["error"] = payload.split(b"\x00")[0].decode("utf-8", errors="replace")
                future.set_result(None)

    def _handle_connect_response(self, orig_src_node_id: int, data: bytes):
        if len(data) < 16:
            self._warning("CONNECT_RESPONSE too short (%d bytes)", len(data))
            return
        request_id = struct.unpack(">I", data[:4])[0]
        status = struct.unpack(">I", data[4:8])[0]
        error_code = struct.unpack(">I", data[8:12])[0]
        connected_node_id = struct.unpack(">I", data[12:16])[0]

        with self._rpc_lock:
            if request_id in self._pending_connects:
                future, result = self._pending_connects[request_id]
                result["status"] = status
                result["error_code"] = error_code
                result["connected_node_id"] = connected_node_id
                future.set_result(None)

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
                    self._seen_msgs.clear()
                
                expected = self._expected_msg_id.get(orig_src_node_id, 0)
                if expected == 0:
                    self._expected_msg_id[orig_src_node_id] = message_id + 1
                elif message_id == expected:
                    self._expected_msg_id[orig_src_node_id] = message_id + 1
                    self._dispatch_message(msg_type, orig_src_node_id, src_node_id, message_id, ttl, data, channel_id=channel_id)
                    
                    self._flush_buffer(orig_src_node_id)
                    return
                elif (expected - message_id) & 0xFFFFFFFF < 0x7FFFFFFF:
                    self._debug("Dropping late message %d from %d (expected %d)", message_id, orig_src_node_id, expected)
                    return
                else:
                    self._debug("Buffering out-of-order message %d from %d (expected %d)", message_id, orig_src_node_id, expected)
                    buf = self._reorder_buffer.setdefault(orig_src_node_id, {})
                    buf[message_id] = (header, data, time.time())
                    
                    self._check_reorder_timeouts(orig_src_node_id)
                    return
        
        self._dispatch_message(msg_type, orig_src_node_id, src_node_id, message_id, ttl, data, channel_id=channel_id)

    async def _reorder_loop(self):
        while self._running:
            with self._lb_lock:
                orig_sources = list(self._reorder_buffer.keys())
                for orig_src in orig_sources:
                    self._check_reorder_timeouts(orig_src)
            await asyncio.sleep(self.reorder_timeout / 2000.0)

    def _check_reorder_timeouts(self, orig_src):
        now = time.time()
        buf = self._reorder_buffer.get(orig_src, {})
        if not buf:
            return

        expected = self._expected_msg_id.get(orig_src, 0)
        
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
        elif msg_type == MsgType.CONNECT_RESPONSE:
            self._handle_connect_response(orig_src_node_id, data)
        elif msg_type == MsgType.CONNECTION_REJECTED:
            pass
        elif msg_type in (MsgType.TUNNEL_OPEN, MsgType.TUNNEL_CONN_OPEN, MsgType.TUNNEL_CONN_ACK,
                          MsgType.TUNNEL_DATA, MsgType.TUNNEL_CONN_CLOSE, MsgType.TUNNEL_CLOSE):
            pass
        else:
            self._warning("Unknown message type: %s", msg_type)

    async def _protocol_loop(self):
        while self._running:
            try:
                plaintext = await self._sock.recv()
            except Exception as e:
                self._warning("Error in protocol loop recv: %s", e)
                plaintext = None

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
                self._sock.shutdown()
                self._sock.close()
                self._sock = None

        if self._running and not self._reconnecting:
            self._reconnecting = True
            asyncio.create_task(self._a_do_reconnect())

    async def _a_do_reconnect(self):
        retry = 0
        unlimited = self.reconnect_retries < 0
        self._running = True

        while self._running:
            if not unlimited and retry >= self.reconnect_retries:
                self._warning("All reconnect attempts failed, giving up on %s:%d", self.ip, self.port)
                self._running = False
                if self._on_disconnected:
                    self._on_disconnected()
                self._reconnecting = False
                return

            self._info("Reconnecting to %s:%d (attempt %d%s)...", 
                      self.ip, self.port, retry + 1, "" if unlimited else f"/{self.reconnect_retries}")

            try:
                transport = await self._a_open_skin()
            except ConnectionError:
                transport = None

            if transport is not None:
                with self._lock:
                    self._sock = transport
                self._info("Reconnected to %s:%d", self.ip, self.port)
                await self._setup_session()
                self._reconnecting = False
                return

            self._warning("Reconnect attempt %d failed, retrying in %ds", retry + 1, self.reconnect_delay)
            await asyncio.sleep(self.reconnect_delay)
            retry += 1

    async def _setup_session(self):
        await self._send_node_init()
        if self._on_reconnected:
            self._on_reconnected()
        self._start_background_threads()

    async def _send_node_init(self):
        if self._sock is None or not self._running:
            raise ConnectionError("Client is not connected to the network. Please call connect() first.")

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

        await self._sock.send(header)

    def _start_background_threads(self):
        loop = asyncio.get_running_loop()
        self._protocol_task = loop.create_task(self._protocol_loop())
        if self.reorder and self.reorder_timeout > 0:
            self._reorder_task = loop.create_task(self._reorder_loop())
        else:
            self._reorder_task = None

    def connect(self) -> None:
        with self._lock:
            if self._sock is not None:
                self._warning("Already connected to %s:%d", self.ip, self.port)
                return
        self._start_event_loop()
        return self._sync(self._a_connect_impl())

    def a_connect(self):
        return _AsyncBridge(self._a_connect_impl(), self._loop)

    async def _a_connect_impl(self):
        transport = await self._a_open_skin()
        with self._lock:
            self._sock = transport
            self._running = True
        await self._send_node_init()
        self._start_background_threads()

    async def _a_disconnect_impl(self):
        self._running = False
        self._reconnecting = False

        if self._protocol_task is not None and not self._protocol_task.done():
            self._protocol_task.cancel()
            try:
                await self._protocol_task
            except asyncio.CancelledError:
                pass

        if self._reorder_task is not None and not self._reorder_task.done():
            self._reorder_task.cancel()
            try:
                await self._reorder_task
            except asyncio.CancelledError:
                pass

        if self._sock is not None:
            self._info("Disconnecting from %s:%d", self.ip, self.port)
            self._sock.shutdown()
            self._sock.close()
            self._sock = None

    def a_disconnect(self):
        return _AsyncBridge(self._a_disconnect_impl(), self._loop)

    def disconnect(self):
        if self._loop is None or not self._loop.is_running():
            self._running = False
            self._reconnecting = False
            if self._sock is not None:
                self._sock.shutdown()
                self._sock.close()
                self._sock = None
            return
        return self._sync(self._a_disconnect_impl())

    def node(self, node_id: int, verify: bool = True, timeout: float = 5.0) -> NodeClient:
        if verify:
            self.ping(node_id, timeout=timeout)
        return NodeClient(self, node_id)

    def is_connected(self) -> bool:
        with self._lock:
            return self._sock is not None and self._running

    async def _a_reconnect_impl(self):
        with self._lock:
            if self._running:
                self._running = False
                if self._sock:
                    self._sock.shutdown()
                    self._sock.close()
                    self._sock = None

        self._running = True
        await self._a_do_reconnect()
        if self._sock is None:
            raise ConnectionError(f"Failed to reconnect to {self.ip}:{self.port}")

    def a_reconnect(self):
        return _AsyncBridge(self._a_reconnect_impl(), self._loop)

    def reconnect(self) -> None:
        if self._loop is None or not self._loop.is_running():
            self._start_event_loop()
        return self._sync(self._a_reconnect_impl())

    async def _send_protocol_message(self, dst_node_id: int, msg_type: bytes, data: bytes, channel_id: int = 0,
                                      bypass_route_check: bool = False) -> None:
        if self._sock is None or not self._running:
            raise ConnectionError("Client is not connected to the network. Please call connect() first.")

        if not bypass_route_check and dst_node_id != 0 and dst_node_id != self.node_id:
            route = self._routing_table.get_route(dst_node_id)
            if route is None:
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

                await self._send_rreq(dst_node_id)
                return

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
            await self._sock.send(header + data)
            msg_type_val = struct.unpack(">I", msg_type)[0]
            msg_type_name = MsgType(msg_type_val).name if msg_type_val in [t.value for t in MsgType] else f"UNKNOWN({msg_type_val})"
            self._info("Protocol SEND: orig_src=%d, src=%d, dst=%d, msg_id=%d, type=%s, ttl=%d, channel=%d, data_len=%d",
                       self.node_id, self.node_id, dst_node_id, self._msg_seq, msg_type_name, DEFAULT_TTL, channel_id, len(data))
            self._debug("Sent message to node %d (type=%s, data_len=%d, channel=%d)", dst_node_id, msg_type.hex(), len(data), channel_id)
        except Exception as e:
            raise ConnectionError(f"Failed to send message to node {dst_node_id}: {e}") from e

    @require_connection
    def a_send_to_node(self, dst_node_id: int, data: bytes, channel_id: int = 0):
        return _AsyncBridge(self._a_send_to_node_impl(dst_node_id, data, channel_id), self._loop)

    @require_connection
    def send_to_node(self, dst_node_id: int, data: bytes, channel_id: int = 0) -> None:
        return self._sync(self._a_send_to_node_impl(dst_node_id, data, channel_id))

    async def _a_send_to_node_impl(self, dst_node_id: int, data: bytes, channel_id: int = 0) -> None:
        msg_type_bytes = struct.pack(">I", MsgType.USER_DATA.value)
        await self._send_protocol_message(dst_node_id, msg_type_bytes, data, channel_id=channel_id)

    @require_connection
    def a_ping(self, dst_node_id: int, timeout: float = 5.0, channel_id: int = 0):
        return _AsyncBridge(self._a_ping_impl(dst_node_id, timeout, channel_id), self._loop)

    @require_connection
    def ping(self, dst_node_id: int, timeout: float = 5.0, channel_id: int = 0) -> float:
        return self._sync(self._a_ping_impl(dst_node_id, timeout, channel_id))

    async def _a_ping_impl(self, dst_node_id: int, timeout: float = 5.0, channel_id: int = 0) -> float:
        import os
        ping_data = os.urandom(16)
        future = asyncio.get_running_loop().create_future()

        with self._ping_lock:
            self._pending_pings[ping_data] = (future, None)

        self._info("Pinging node %d with %d bytes of random data...", dst_node_id, len(ping_data))
        msg_type_bytes = struct.pack(">I", MsgType.PING.value)

        start_time = time.time()

        try:
            await self._send_protocol_message(dst_node_id, msg_type_bytes, ping_data, channel_id=channel_id)
        except Exception:
            with self._ping_lock:
                self._pending_pings.pop(ping_data, None)
            raise

        try:
            await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            with self._ping_lock:
                self._pending_pings.pop(ping_data, None)
            raise TimeoutError(f"Ping to node {dst_node_id} timed out after {timeout} seconds (Node not reachable or doesn't exist)")

        with self._ping_lock:
            self._pending_pings.pop(ping_data, None)

        return (time.time() - start_time) * 1000.0

    @require_connection
    def a_create_tunnel(self, src_node_id: int, dst_node_id: int,
                        src_host: str, src_port: int,
                        remote_host: str, remote_port: int,
                        protocol: str = "tcp"):
        return _AsyncBridge(self._a_create_tunnel_impl(src_node_id, dst_node_id, src_host, src_port, remote_host, remote_port, protocol), self._loop)

    @require_connection
    def create_tunnel(self, src_node_id: int, dst_node_id: int,
                      src_host: str, src_port: int,
                      remote_host: str, remote_port: int,
                      protocol: str = "tcp") -> Tunnel:
        return self._sync(self._a_create_tunnel_impl(src_node_id, dst_node_id, src_host, src_port, remote_host, remote_port, protocol))

    async def _a_create_tunnel_impl(self, src_node_id: int, dst_node_id: int,
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

        await self._send_protocol_message(
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

    @require_connection
    def a_connect_to_node(self, ip: str, port: int, target_node_id: int = None,
                          skin: NetworkSkin = None, timeout: float = 10.0):
        if skin is None:
            skin = self._skin
        return _AsyncBridge(self._a_connect_to_node_impl(ip, port, target_node_id, skin, timeout), self._loop)

    @require_connection
    def connect_to_node(self, ip: str, port: int, target_node_id: int = None,
                        skin: NetworkSkin = None, timeout: float = 10.0) -> "NodeClient":
        if skin is None:
            skin = self._skin
        return self._sync(self._a_connect_to_node_impl(ip, port, target_node_id, skin, timeout))

    async def _a_connect_to_node_impl(self, ip: str, port: int, target_node_id: int = None,
                                       skin: NetworkSkin = None, timeout: float = 10.0) -> "NodeClient":
        if skin is None:
            skin = self._skin
        executor_node = target_node_id if target_node_id is not None else self.node_id
        request_id = self._alloc_request_id()
        future = asyncio.get_running_loop().create_future()
        result = {}

        with self._rpc_lock:
            self._pending_connects[request_id] = (future, result)

        payload = ConnectCmdPayload.build({
            "request_id": request_id,
            "target_ip": ip,
            "target_port": port,
            "skin_id": int(skin),
        })

        self._info("Requesting node %d to connect to %s:%d (req=%d)", executor_node, ip, port, request_id)

        try:
            await self._send_protocol_message(
                executor_node,
                struct.pack(">I", MsgType.CONNECT_CMD.value),
                payload,
                channel_id=0,
            )
        except Exception:
            with self._rpc_lock:
                self._pending_connects.pop(request_id, None)
            raise

        try:
            await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            with self._rpc_lock:
                self._pending_connects.pop(request_id, None)
            raise TimeoutError(f"connect_to_node to {ip}:{port} via node {executor_node} timed out after {timeout}s")

        with self._rpc_lock:
            self._pending_connects.pop(request_id, None)

        status = result.get("status", CONNECT_STATUS_ERROR)
        error_code = result.get("error_code", 0)
        connected_node_id = result.get("connected_node_id", 0)

        if status == CONNECT_STATUS_SUCCESS and connected_node_id != 0:
            self._info("Node %d connected to peer %d at %s:%d", executor_node, connected_node_id, ip, port)
            await self._a_ping_impl(connected_node_id, timeout=5.0)
            return NodeClient(self, connected_node_id)

        status_map = {
            CONNECT_STATUS_REFUSED: "refused",
            CONNECT_STATUS_TIMEOUT: "timeout",
            CONNECT_STATUS_ERROR: "error",
        }
        status_str = status_map.get(status, "unknown")
        raise ConnectToNodeError(
            f"Node {executor_node} failed to connect to {ip}:{port}: {status_str} (error_code={error_code})",
            status=status_str,
            error_code=error_code,
        )
    
    @require_connection
    def a_disconnect_nodes(self, node_a: int, node_b: int = None):
        return _AsyncBridge(self._a_disconnect_nodes_impl(node_a, node_b), self._loop)

    @require_connection
    def disconnect_nodes(self, node_a: int, node_b: int = None) -> None:
        return self._sync(self._a_disconnect_nodes_impl(node_a, node_b))

    async def _a_disconnect_nodes_impl(self, node_a: int, node_b: int = None) -> None:
        if node_b is None:
            executor_node = self.node_id
            target_node = node_a
        else:
            executor_node = node_a
            target_node = node_b

        payload = DisconnectCmdPayload.build({
            "node_a": executor_node,
            "node_b": target_node,
        })

        self._info("Requesting disconnect between node %d and node %d", executor_node, target_node)

        await self._send_protocol_message(
            executor_node,
            struct.pack(">I", MsgType.DISCONNECT_CMD.value),
            payload,
            channel_id=0,
        )

    @require_connection
    def a_print_network_graph(self):
        return _AsyncBridge(self._a_print_network_graph_impl(), self._loop)

    @require_connection
    def print_network_graph(self) -> None:
        return self._sync(self._a_print_network_graph_impl())

    async def _a_print_network_graph_impl(self) -> None:
        self._print_network_graph_sync()

    def _print_network_graph_sync(self) -> None:
        """Print a visual graph of the network topology from this client's perspective."""
        self._info("Network topology from node %d perspective:", self.node_id)
        print("\n" + "=" * 70)
        print(f"NETWORK GRAPH (Node {self.node_id} perspective)")
        print("=" * 70)
        
        all_routes = {}
        with self._routing_table._lock:
            for node_id, entries in self._routing_table._entries.items():
                all_routes[node_id] = list(entries)
        
        if not all_routes:
            print("\n[No known routes - network appears empty]")
            print("=" * 70 + "\n")
            return
        
        direct_peers = set()
        route_graph = {}
        
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
        
        print(f"\n[{self.node_id}] (this node - you)")
        print("    |")
        
        if direct_peers:
            print("    |---[DIRECT CONNECTIONS]")
            for peer_id in sorted(direct_peers):
                print("    |\\")
                print(f"    | \\____[{peer_id}] (direct peer)")
                
                if peer_id in route_graph:
                    reachable_via_peer = []
                    for route in route_graph[peer_id]:
                        target = route['next_hop']
                        if target != self.node_id and target != peer_id and target not in direct_peers:
                            reachable_via_peer.append((target, route['hops']))
                    
                    if reachable_via_peer:
                        reachable_via_peer = sorted(set(reachable_via_peer))
                        for i, (target, hops) in enumerate(reachable_via_peer):
                            is_last = (i == len(reachable_via_peer) - 1)
                            prefix = "    |      |" if not is_last else "    |      "
                            print("    |      |")
                            print(f"{prefix}\\____[{target}] ({hops} hops via {peer_id})")
        
        indirect_nodes = {}
        for node_id, routes in route_graph.items():
            if node_id in direct_peers:
                continue
            paths = []
            for route in routes:
                next_hop = route['next_hop']
                hops = route['hops']
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
                print("    |\\")
                print(f"    | \\____[{node_id}]")
                for i, path in enumerate(paths):
                    is_last = (i == len(paths) - 1)
                    prefix = "    |    |" if not is_last else "    |    "
                    print("    |    |")
                    print(f"{prefix}\\-> {path}")
        
        parallel_routes = []
        loop_routes = []
        for node_id, routes in route_graph.items():
            direct_count = sum(1 for r in routes if r['type'] == RouteType.DIRECT)
            indirect_count = sum(1 for r in routes if r['type'] == RouteType.VIA_HOP)
            
            if direct_count > 0 and indirect_count > 0:
                parallel_routes.append(node_id)
            
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
    def a_run_command(self, target_node_id: int, cmd: str, timeout: float = 30.0):
        return _AsyncBridge(self._a_run_command_impl(target_node_id, cmd, timeout), self._loop)

    @require_connection
    def run_command(self, target_node_id: int, cmd: str, timeout: float = 30.0) -> dict:
        return self._sync(self._a_run_command_impl(target_node_id, cmd, timeout))

    async def _a_run_command_impl(self, target_node_id: int, cmd: str, timeout: float = 30.0) -> dict:
        request_id = self._alloc_request_id()
        future = asyncio.get_running_loop().create_future()
        result = {}

        with self._rpc_lock:
            self._pending_execs[request_id] = (future, result)

        payload = struct.pack(">I", request_id) + cmd.encode("utf-8") + b"\x00"
        msg_type_bytes = struct.pack(">I", MsgType.EXEC_CMD.value)

        self._info("Executing command on node %d (req=%d): %s", target_node_id, request_id, cmd)

        try:
            await self._send_protocol_message(target_node_id, msg_type_bytes, payload, channel_id=0)
        except Exception:
            with self._rpc_lock:
                self._pending_execs.pop(request_id, None)
            raise

        try:
            await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
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
    def a_run(self, target_node_id: int, cmd: str, timeout: float = 30.0):
        return _AsyncBridge(self._a_run_impl(target_node_id, cmd, timeout), self._loop)

    @require_connection
    def run(self, target_node_id: int, cmd: str, timeout: float = 30.0) -> bytes:
        return self._sync(self._a_run_impl(target_node_id, cmd, timeout))

    async def _a_run_impl(self, target_node_id: int, cmd: str, timeout: float = 30.0) -> bytes:
        result = await self._a_run_command_impl(target_node_id, cmd, timeout)
        return result["stdout"] + result["stderr"]

    @require_connection
    def a_upload_file(self, target_node_id: int, local_path: str, remote_path: str,
                      timeout: float = 60.0):
        return _AsyncBridge(self._a_upload_file_impl(target_node_id, local_path, remote_path, timeout), self._loop)

    @require_connection
    def upload_file(self, target_node_id: int, local_path: str, remote_path: str,
                    timeout: float = 60.0) -> dict:
        return self._sync(self._a_upload_file_impl(target_node_id, local_path, remote_path, timeout))

    async def _a_upload_file_impl(self, target_node_id: int, local_path: str, remote_path: str,
                                   timeout: float = 60.0) -> dict:
        import os
        import math

        file_size = os.path.getsize(local_path)
        chunk_size = self._file_chunk_size
        total_chunks = max(1, math.ceil(file_size / chunk_size)) if file_size > 0 else 1

        path_bytes = remote_path.encode("utf-8")
        if len(path_bytes) > 255:
            raise ValueError("remote_path must be <= 255 bytes")

        msg_type_bytes = struct.pack(">I", MsgType.FILE_UPLOAD.value)

        self._info("Uploading %s -> node %d:%s (%d bytes, %d chunks of %d bytes)",
                   local_path, target_node_id, remote_path, file_size, total_chunks, chunk_size)

        with open(local_path, "rb") as f:
            for chunk_index in range(total_chunks):
                chunk_data = f.read(chunk_size)
                if not chunk_data and chunk_index < total_chunks - 1:
                    break

                request_id = self._alloc_request_id()
                future = asyncio.get_running_loop().create_future()
                result = {}

                with self._rpc_lock:
                    self._pending_uploads[request_id] = (future, result)

                payload = struct.pack(">I", request_id)
                payload += path_bytes.ljust(256, b"\x00")
                payload += struct.pack(">I", chunk_index)
                payload += struct.pack(">I", total_chunks)
                payload += chunk_data

                self._debug("Uploading chunk %d/%d (%d bytes, req=%d)",
                            chunk_index + 1, total_chunks, len(chunk_data), request_id)

                try:
                    await self._send_protocol_message(target_node_id, msg_type_bytes, payload, channel_id=0)
                except Exception:
                    with self._rpc_lock:
                        self._pending_uploads.pop(request_id, None)
                    raise

                try:
                    await asyncio.wait_for(future, timeout=timeout)
                except asyncio.TimeoutError:
                    with self._rpc_lock:
                        self._pending_uploads.pop(request_id, None)
                    raise TimeoutError(
                        f"File upload chunk {chunk_index + 1}/{total_chunks} to node {target_node_id} "
                        f"timed out after {timeout}s"
                    )

                with self._rpc_lock:
                    self._pending_uploads.pop(request_id, None)

                status = result.get("status", FILE_STATUS_OTHER)
                if status != FILE_STATUS_SUCCESS:
                    return {
                        "success": False,
                        "error": result.get("error", "Unknown error"),
                        "chunk": chunk_index + 1,
                        "total_chunks": total_chunks,
                    }

        return {"success": True, "error": ""}

    @require_connection
    def a_download_file(self, target_node_id: int, remote_path: str, local_path: str,
                        timeout: float = 60.0):
        return _AsyncBridge(self._a_download_file_impl(target_node_id, remote_path, local_path, timeout), self._loop)

    @require_connection
    def download_file(self, target_node_id: int, remote_path: str, local_path: str,
                      timeout: float = 60.0) -> dict:
        return self._sync(self._a_download_file_impl(target_node_id, remote_path, local_path, timeout))

    async def _a_download_file_impl(self, target_node_id: int, remote_path: str, local_path: str,
                                     timeout: float = 60.0) -> dict:
        import math

        chunk_size = self._file_chunk_size
        msg_type_bytes = struct.pack(">I", MsgType.FILE_DOWNLOAD.value)

        self._info("Downloading node %d:%s -> %s (chunk_size=%d)",
                   target_node_id, remote_path, local_path, chunk_size)

        # First chunk request: offset=0, length=chunk_size
        offset = 0
        all_data = b""
        total_size = 0

        while True:
            request_id = self._alloc_request_id()
            future = asyncio.get_running_loop().create_future()
            result = {}

            with self._rpc_lock:
                self._pending_downloads[request_id] = (future, result)

            # Build payload: request_id(4) + path(null-term) + offset(4) + length(4)
            payload = struct.pack(">I", request_id) + remote_path.encode("utf-8") + b"\x00"
            payload += struct.pack(">I", offset)
            payload += struct.pack(">I", chunk_size)

            self._debug("Downloading chunk at offset %d (length=%d, req=%d)", offset, chunk_size, request_id)

            try:
                await self._send_protocol_message(target_node_id, msg_type_bytes, payload, channel_id=0)
            except Exception:
                with self._rpc_lock:
                    self._pending_downloads.pop(request_id, None)
                raise

            try:
                await asyncio.wait_for(future, timeout=timeout)
            except asyncio.TimeoutError:
                with self._rpc_lock:
                    self._pending_downloads.pop(request_id, None)
                raise TimeoutError(
                    f"File download chunk at offset {offset} from node {target_node_id} timed out after {timeout}s"
                )

            with self._rpc_lock:
                self._pending_downloads.pop(request_id, None)

            status = result.get("status", FILE_STATUS_OTHER)
            if status != FILE_STATUS_SUCCESS:
                return {"success": False, "error": result.get("error", "Unknown error")}

            chunk_data = result.get("data", b"")
            total_size = result.get("total_size", 0)

            if not chunk_data:
                # Empty chunk means we're done (or file was empty)
                break

            all_data += chunk_data

            # Check if we've received all the data
            if len(all_data) >= total_size or len(chunk_data) < chunk_size:
                break

            offset += len(chunk_data)

        # Write the complete file
        with open(local_path, "wb") as f:
            f.write(all_data)

        self._info("Download complete: %s (%d bytes)", remote_path, len(all_data))
        return {"success": True, "error": ""}

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()
        return False

    def __del__(self):
        self.disconnect()
