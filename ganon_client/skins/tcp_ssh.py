import asyncio
import struct
from typing import Optional

import asyncssh

from ganon_client.skin import NetworkSkin, NetworkSkinImpl, register_skin


class SshSkin(NetworkSkinImpl):
    """SSH transport skin using asyncssh.

    Connects to a ganon node acting as SSH server, opens a "session" channel,
    and requests the "ganon" subsystem.  Authentication uses the "none" method.
    Host key verification is disabled (the server uses ephemeral keys).

    Wire frame format (identical to tcp-plain, carried inside the SSH channel):
        [4 bytes] big-endian payload length
        [N bytes] serialized protocol_msg_t + data
    """

    def __init__(
        self,
        conn: asyncssh.SSHClientConnection,
        stdin: asyncssh.SSHWriter,
        stdout: asyncssh.SSHReader,
    ):
        self._conn = conn
        self._stdin = stdin
        self._stdout = stdout

    # ------------------------------------------------------------------ #
    # Construction                                                         #
    # ------------------------------------------------------------------ #

    @classmethod
    async def open(cls, ip: str, port: int, timeout: float) -> "SshSkin":
        conn = await asyncio.wait_for(
            asyncssh.connect(
                ip,
                port,
                known_hosts=None,
                username="ganon",
                preferred_auth="none",
                # Disable all key-based and password auth; use none-auth only.
                client_keys=None,
                password=None,
            ),
            timeout=timeout,
        )
        stdin, stdout, _ = await conn.open_session(subsystem="ganon", encoding=None)
        return cls(conn, stdin, stdout)

    # ------------------------------------------------------------------ #
    # I/O                                                                  #
    # ------------------------------------------------------------------ #

    async def send(self, plaintext: bytes) -> None:
        frame = struct.pack(">I", len(plaintext)) + plaintext
        self._stdin.write(frame)
        try:
            await self._stdin.drain()
        except (asyncssh.ChannelOpenError, OSError) as e:
            raise ConnectionError(f"SSH send failed: {e}") from e

    async def recv(self) -> Optional[bytes]:
        try:
            len_bytes = await self._stdout.readexactly(4)
        except (asyncio.IncompleteReadError, asyncssh.ChannelOpenError, OSError):
            return None
        if not len_bytes:
            return None

        payload_len = struct.unpack(">I", len_bytes)[0]
        if payload_len < 36 or payload_len > 300_000:
            return None

        try:
            payload = await self._stdout.readexactly(payload_len)
        except (asyncio.IncompleteReadError, asyncssh.ChannelOpenError, OSError):
            return None
        if not payload:
            return None

        return payload

    # ------------------------------------------------------------------ #
    # Teardown                                                             #
    # ------------------------------------------------------------------ #

    def shutdown(self) -> None:
        try:
            self._stdin.channel.close()
        except Exception:
            pass

    def close(self) -> None:
        try:
            self._conn.close()
        except Exception:
            pass


register_skin(NetworkSkin.SSH, SshSkin)
