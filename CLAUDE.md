# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Update Requirement

**Any change to the project must update this CLAUDE.md file.**

When making changes to the codebase:
1. Update relevant sections of this document to reflect the new state
2. If you add new build targets, commands, or conventions → update "Build Commands", "Running Ganon", or "Critical Conventions" sections
3. If you add new architectural components or change layer behavior → update "Architecture Overview" or "Key Architectural Patterns"
4. If you add Python client features → update "Python Client" section
5. Commit CLAUDE.md changes along with code changes

This keeps CLAUDE.md in sync with the actual state of the codebase so future agents have accurate guidance.

## Quick Reference

**Ganon** is a mesh-style network tunneler in C that implements reactive AODV (Ad-hoc On-Demand Distance Vector) routing for decentralized networks.

**Before making changes**: Read AGENTS.md for full architecture, protocol, and convention details. This file focuses on practical workflow and key patterns.

## Build Commands

```bash
# Build all targets (x64, ARMv5, ARMv7, MIPS32BE — release & debug)
make

# Build x64 only (both release and debug)
make x64

# Build specific target
make x64r      # x64 release
make x64d      # x64 debug
make arm       # ARMv5 (hard-float, musl)
make armv7     # ARMv7 with NEON (musl)
make mips32be  # MIPS 32-bit big-endian (musl)

# Build/rebuild third-party libraries
make libssh         # x64 libssh (OpenSSL backend)
make libssh-arm     # ARM libssh (mbedTLS backend)
make libssh-mips32be
make mbedtls-arm
make mbedtls-mips32be
make musl-arm        # musl libc for ARM
make musl-mips32be

# Clean all build artifacts (does NOT clean third-party libs)
make clean
# Clean specific third-party installs
make clean-cross-libs   # removes ARM+MIPS mbedTLS and libssh installs
make clean-musl         # removes musl installs
```

After build, binaries are in `bin/`. A convenience symlink `ganon` points to `ganon_<version>_x64_debug` for local dev.

**Binary sizes (approximate, all skins):**
- x64 release: ~5.7 MB (glibc + OpenSSL + ngtcp2 + picotls for QUIC)
- ARM release: ~630 KB (musl + mbedTLS + dead-stripping; no QUIC)
- ARMv7 release: ~635 KB
- MIPS32BE release: ~965 KB

**Third-party rebuild note**: `make clean` only removes ganon binaries. To force rebuild of libssh or mbedTLS, remove the install directory (e.g., `rm -rf third_party/libssh-install-arm`) and run `make arm`.

**Cross-compile flags**: ARM/MIPS cross-compiled libraries use `CROSS_SIZE_FLAGS = -Os -ffunction-sections -fdata-sections -D_FORTIFY_SOURCE=0`. The `-D_FORTIFY_SOURCE=0` is required because libssh compiled with glibc headers references `__snprintf_chk`/`__strncat_chk` which musl does not provide.

## Running Ganon

```bash
# Start node 1 listening on port 5555
./ganon -i 1 -p 5555

# Start node 2 and connect to node 1
./ganon -i 2 -p 5556 -c 127.0.0.1:5555

# With reconnect settings
./ganon -i 3 -p 5557 --reconnect-retries 10 --reconnect-delay 3 -c 127.0.0.1:5555
```

See binary with `./ganon -h` for all options (load balancing strategy, timeouts, etc.).

## Architecture Overview

**5-Layer Model** (see AGENTS.md for full details):

1. **Network** (`network.c/h`) - Socket management, accept loop, reconnection per peer
2. **Transport** (`transport.c/h`) - Buffer layer, recv/send protocol_msg_t, connection abstraction
3. **Protocol** (`protocol.c/h`) - Byte order conversion (network ↔ host), magic validation
4. **Session** (`session.c/h`) - Protocol-specific logic (NODE_INIT, PEER_INFO, CONNECTION_REJECTED, etc.)
5. **Routing** (`routing.c/h`) - AODV route discovery (RREQ/RREP), broadcast/forward logic, RERR handling

**Key Design Rule**: Each layer must not know about the layers above it. E.g., Network knows nothing about node IDs; Session knows nothing about sockets. Transport dispatches all I/O through the skin vtable.

**Skins** (`skin.c/h`, `skins/skin_tcp_*.c`) - Pluggable transport vertical slice:
- `tcp-monocypher` (default): TCP + X25519 + XChaCha20-Poly1305 encryption
- `tcp-plain`: Unencrypted TCP with length-prefixed raw protocol frames (useful for debugging)
- `tcp-xor`: X25519 handshake + repeating-key XOR obfuscation (not secure, but per-connection obfuscation)
- `tcp-ssh`: TCP + libssh (server-side ephemeral Ed25519 host key, "none" auth, "ganon" subsystem channel)
- `udp-quic`: UDP + QUIC (ngtcp2 v1.12.0 + picotls with OpenSSL crypto backend, TLS 1.3, self-signed ECDSA P-256 cert, "ganon" ALPN)

Skins are controlled by `include/skins_config.h` — each macro is guarded by `#ifndef` so CMake can override via `-D` flags on the command line. CMake reads these macros at configure time via `file(STRINGS ...)`.

TCP-SSH skin availability by architecture:
- x64: libssh with OpenSSL backend (from `third_party/libssh-install`)
- ARM/ARMv7: libssh with mbedTLS backend (from `third_party/libssh-install-arm`)
- MIPS32BE: libssh with mbedTLS backend (from `third_party/libssh-install-mips32be`)

UDP-QUIC skin availability:
- **x64 only** — requires ngtcp2 + picotls-openssl + system OpenSSL. Automatically disabled on cross-compile targets via `-DSKIN_ENABLE_QUIC=0` compile definition.
- Third-party libraries: `third_party/ngtcp2-install` + `third_party/picotls-install`
- Build: `make ngtcp2` (depends on `make picotls` which depends on system OpenSSL 1.1.1+)
- Clean: `make clean-quic`

## Critical Conventions

### Function Naming & Error Handling

Every function except `main` must:
- Use module prefix in ALL_CAPS with `__` (e.g., `NETWORK__init`, `ROUTING__get_route`)
- Return `err_t` (not `int`)
- Start with `err_t rc = E__SUCCESS;` followed by a blank line
- End with `l_cleanup:` label and `return rc;`
- Use output parameters via pointers for returning data

```c
err_t NETWORK__connect(IN const char *host, OUT int *fd) {
    err_t rc = E__SUCCESS;

    FAIL_IF(NULL == host || NULL == fd, E__INVALID_ARG_NULL_POINTER);
    // ... implementation using FAIL_IF, FAIL, etc.

l_cleanup:
    return rc;
}
```

### Common Macros (from include/common.h)

- `FAIL_IF(condition, error)` - Check and goto cleanup on failure
- `FAIL(error)` - Unconditional failure
- `BREAK_IF(condition)` / `CONTINUE_IF(condition)` - Loop control
- `FREE(ptr)` - Safe pointer cleanup
- `VALIDATE_ARGS(...)` - Null-pointer validation for multiple arguments
- `IN` / `OUT` / `INOUT` - Parameter direction documentation (no runtime effect)

### Comparison Convention

Static values first: `NULL != ptr`, `E__SUCCESS != rc`, `0 > value` (not `ptr != NULL`, `rc != E__SUCCESS`, `value < 0`)

### Global Singletons

Only two globals (defined in their respective modules):
- `g_network` (network.h) - The one network instance
- `g_node_id` (routing.h) - This node's ID

Both are wired in `main.c` during initialization.

## Key Architectural Patterns

### Byte Order Handling

- **Host order**: All C code logic works in host byte order (native endianness)
- **Network order**: Protocol wire format uses network byte order (big-endian)
- **Conversion**: Happens ONLY in `PROTOCOL__serialize()` and `PROTOCOL__unserialize()`
- **Session Layer**: Always works with host-order `protocol_msg_t` from Transport

### Routing & Broadcasting

- `ROUTING__on_message()` - Called for every received message; handles destination routing, broadcasts, forwards
- `ROUTING__broadcast()` - Sends to all direct DIRECT-type routes except specified node; decrements TTL, updates src
- `ROUTING_TABLE_MAX_ENTRIES` - Fixed size routing table; see include/routing.h for size

### Load Balancing & Channels

When multiple routes exist to same destination:
- `round-robin` strategy (default): Cycles through available routes. The `--rr-count N` flag (default 1) controls how many consecutive routes are used per message — e.g. with 3 routes and `--rr-count 2`, packet 1 → routes [1,2], packet 2 → routes [2,3], packet 3 → routes [3,1].
- `all-routes` strategy: Sends to all available routes simultaneously.
- `sticky` strategy: Non-zero `channel_id` → always uses route `(channel_id-1) % route_count` (sticky per channel). `channel_id=0` → falls back to round-robin with rr_count. The `round-robin` and `all-routes` strategies always ignore `channel_id`.
- Set strategy via `--lb-strategy` CLI flag or `LB_STRATEGY` env; set rr-count via `--rr-count` or `RR_COUNT` env.

**Channel ID** (`channel_id` in the message header):
- Meaningful only in `sticky` strategy. Channel 0 = global (round-robin). Channel N → route `(N-1) % route_count`.
- PONG responses automatically mirror the channel_id of the incoming PING (C session and Python client both).
- Python client: pass `channel_id=N` to `send_to_node()` or `ping()`.

**Wire format note**: `channel_id` is the last field in `protocol_msg_t` (added after `ttl`). Header is now 36 bytes. Old nodes (32-byte header) are incompatible.

### Logging (Debug Builds Only)

```c
LOG_ERROR(fmt, ...)   // Catastrophic errors
LOG_WARN(fmt, ...)    // Issues that can be handled
LOG_INFO(fmt, ...)    // Major events (startup, connections)
LOG_DEBUG(fmt, ...)   // Smaller events, more detail
LOG_TRACE(fmt, ...)   // Detailed tracing data
```

Logging is **completely compiled out in Release builds** (`CMAKE_BUILD_TYPE=Release` excludes `logging.c`). When adding new `.c` files:
- If it uses LOG_* macros → add ONLY to Debug sources in src/CMakeLists.txt
- If it doesn't use LOG_* → add to both Release and Debug sources

**Note**: `tunnel.c` uses LOG_* macros and is added to both Debug and Release sources in `src/CMakeLists.txt` (the macros are no-ops in Release).

### TCP/UDP Tunneling (`tunnel.c/h`)

Ganon supports transparent port-forwarding tunnels through the mesh. A tunnel has a **src node** (listener) and a **dst node** (connector to remote).

**Message types** (added to `msg_type_t`):
| Type | Value | Direction | Meaning |
|---|---|---|---|
| `MSG__TUNNEL_OPEN` | 8 | Python → src_node | Start listening on src_host:src_port |
| `MSG__TUNNEL_CONN_OPEN` | 9 | src → dst | New client connected, connect to remote |
| `MSG__TUNNEL_CONN_ACK` | 10 | dst → src | Connected to remote, start data flow |
| `MSG__TUNNEL_DATA` | 11 | bidirectional | Raw TCP data (prefixed with tunnel_id+conn_id) |
| `MSG__TUNNEL_CONN_CLOSE` | 12 | either side | This connection is closed |
| `MSG__TUNNEL_CLOSE` | 13 | Python → src_node | Close entire tunnel/listener |

**Python API:**
```python
# Create a tunnel: src_node listens on src_host:src_port,
# each new connection triggers dst_node to connect to remote_host:remote_port
tunnel = c.create_tunnel(src_node_id=1, dst_node_id=30,
                         src_host="0.0.0.0", src_port=8080,
                         remote_host="example.com", remote_port=80,
                         protocol="tcp")

tunnel.is_up    # True if tunnel is open and client is connected
tunnel.close()  # Send TUNNEL_CLOSE to src_node, mark tunnel down
```

**Internals:**
- `TUNNEL__init()` / `TUNNEL__destroy()` — called from `main.c`
- `TUNNEL__on_message()` — called from `session.c` for all TUNNEL_* message types
- `TUNNEL__handle_disconnect(node_id)` — called from `SESSION__on_disconnected`; closes all tunnels/connections associated with the disconnected node
- Limits: `MAX_SRC_TUNNELS=32`, `MAX_CONNS_PER_TUNNEL=64`, `MAX_DST_CONNS=512`
- `channel_id` in the ganon header is set to `tunnel_id` for all tunnel messages (enables sticky routing)
- SIGPIPE is ignored at process level (`signal(SIGPIPE, SIG_IGN)` in `main.c`)

## Python Client (ganon_client/)

Quick setup:
```bash
source venv/bin/activate
# Dependencies (e.g., monocypher-py) are already installed in the venv
```

The Python client mirrors C message types and protocol. See ganon_client/README.md for usage. **Note**: Python client may need updates when C protocol changes (e.g., new msg_type_t values).

**Skin selection:**
```python
from ganon_client.skin import NetworkSkin

# Default encrypted skin
c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.TCP_MONOCYPHER)

# Plain unencrypted skin (useful for debugging)
c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.TCP_PLAIN)

# XOR-obfuscated skin (per-connection obfuscation, not secure)
c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.TCP_XOR)

# QUIC (UDP + TLS 1.3) — requires aioquic; server must have udp-quic skin enabled
c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.UDP_QUIC)
```

Key methods: `connect()`, `disconnect()`, `send_to_node()`, `ping()`, `create_tunnel()`. The `Tunnel` object returned by `create_tunnel()` has `is_up` property and `close()` method.

## Debugging Tips

### Common Issues

1. **Byte order bugs**: Always check that conversions happen in Protocol layer only. If you see manual `htonl()` calls outside protocol.c, that's a bug.

2. **Network layer calling Session directly**: Network should only call registered callbacks (via function pointers). Never call session functions directly from network.c.

3. **Socket leaks**: Check that all fd cleanup happens in Transport or Network cleanup paths. Transport owns the fd after initialization.

4. **Routing loops**: RREQ deduplication uses `seen_msgs_cache` (LRU). If broadcasts are repeating, check that cache is being populated correctly.

5. **Round-robin only picks one path**: The destination node must respond to EVERY copy of an RREQ (one per path), not just the first. This is handled in `ROUTING__on_message` before the dedup check: RREQ messages targeting this node always trigger a direct RREP on the arrival transport before the message is marked seen. If dedup fires first, only one RREP is sent and the requester learns only one route.

6. **Cross-compilation issues**: ARM and MIPS builds use static linking. If linking fails, verify all dependencies are available for the target architecture.

### Debug Build vs Release

```bash
# Debug: Has logging, debug symbols, no optimization
make x64d
./bin/ganon_0.1.0_x64_debug -i 1

# Release: No logging, stripped binary, -O3 optimization
make x64r
./bin/ganon_0.1.0_x64_release -i 1
```

Use debug builds for development. Release builds are ~90% smaller (static link + strip + musl for cross targets).

### Logging in Debug

Set log level via code (in network initialization) or use conditionals in source:
```c
#ifdef __DEBUG__
    LOG_DEBUG("Detailed trace: %d", value);
#endif
```

## Testing & Validation

### Manual Multi-Node Testing

```bash
# Terminal 1: Node 1
./ganon -i 1 -p 5555

# Terminal 2: Node 2 (connects to Node 1)
./ganon -i 2 -p 5556 -c 127.0.0.1:5555

# Terminal 3: Node 3 (connects to Node 1)
./ganon -i 3 -p 5557 -c 127.0.0.1:5555
```

Check logs (if debug build) for routing table updates and message forwarding.

### Docker Multi-Node Testing

See docker-compose.yml for containerized multi-node setup.

## File Organization

```
.
├── src/                    # C implementation files
├── include/                # C header files
├── ganon_client/           # Python client library
├── cmake/                  # Toolchain files (cross-compilation)
├── bin/                    # Built binaries (after make)
├── Makefile                # Build orchestration
├── CMakeLists.txt          # Root CMake config
├── src/CMakeLists.txt      # Source CMake config (controls compilation)
├── VERSION                 # Version string (0.1.0)
├── AGENTS.md               # Architecture, protocol, conventions (MUST READ)
├── ROUTING_README.md       # AODV routing algorithm details
├── src/tunnel.c            # TCP/UDP tunnel implementation
└── include/tunnel.h        # Tunnel API
```

## Important Notes

1. **AGENTS.md is the source of truth** for architecture, protocol format, data structures, and error codes. This file complements it with workflow guidance.

2. **Always update AGENTS.md** when making architectural changes, adding new message types, or changing error codes (per Agent Rules in AGENTS.md).

3. **Protocol version compatibility**: The wire format magic is "GNN\0". If you change the protocol structure, consider version negotiation in NODE_INIT.

4. **Threading model**: Network layer uses thread-per-connection: one accept thread per listener, one connect/reconnect thread per outbound peer, and one socket thread per TCP connection. Session and Routing must be thread-safe. Check mutex usage in routing_table_t. Shutdown closes all client fds first, then joins connect threads (which have already joined their socket threads), then joins any remaining incoming socket threads, then frees entries.

5. **Static linking**: All binaries are statically linked (`CMAKE_EXE_LINKER_FLAGS="-static"`). This affects how dependencies are resolved during build.

6. **Error code ranges** (from include/err.h):
   - Generic: 0x001-0x0FF
   - args: 0x200-0x2FF
   - network: 0x300-0x3FF
   - session: 0x401-0x4FF
   - routing: 0x501-0x5FF

## Getting Started on a Task

1. Read the relevant section of AGENTS.md first (it's comprehensive)
2. Check the layer diagram above to understand which module(s) you'll touch
3. Look at similar functions in the module for naming/error-handling patterns
4. If modifying protocol: update AGENTS.md protocol section + Python client
5. If adding a .c file: update src/CMakeLists.txt appropriately
6. Build with `make x64d`, test manually, verify logs in debug build
