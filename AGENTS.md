# Ganon Project

This is the Ganon project - a mesh-style network tunneler in C built with CMake.

## Agent Rules

**Any change to the project must update this AGENTS.md file.**

When making significant changes (new features, bug fixes, architectural changes, etc.), you must:
1. Update the relevant sections of this document to reflect the new state
2. Add or update the TODO list at the bottom
3. Commit the AGENTS.md changes along with the code changes

This ensures AGENTS.md stays in sync with the codebase.

## Project Structure

- `src/` - Source files
- `include/` - Header files
- `VERSION` - Version file (e.g., "0.2.0")
- `CMakeLists.txt` - Build configuration
- `Makefile` - Build orchestration
- `cmake/` - Toolchain files for cross-compilation
- `ganon_client/` - Python client library

### C Source Files

- `src/main.c` - Main entry point, signal handling, wires network and session
- `src/args.c` - Argument parsing implementation
- `src/logging.c` - Logging implementation
- `src/network.c` - Socket management, accept loop, reconnection logic, global singleton `g_network`
- `src/session.c` - Protocol logic (NODE_INIT, PEER_INFO, NODE_DISCONNECT, CONNECTION_REJECTED handlers)
- `src/transport.c` - Buffer layer, recv/send protocol_msg_t, connection abstraction, outbound queue
- `src/protocol.c` - Protocol parsing, serialization, byte order conversion
- `src/routing.c` - Routing table, ROUTING__on_message for broadcast/forward logic, global singleton `g_node_id`
- `src/loadbalancer.c` - Multi-route load balancing strategies (round-robin, all-routes, sticky) and reordering buffer logic
- `src/tunnel.c` - TCP/UDP tunnel implementation (TUNNEL_OPEN, CONN_OPEN, CONN_ACK, DATA, CONN_CLOSE, CLOSE)
- `src/skin.c` - Skin registry (SKIN__register, SKIN__by_id, SKIN__by_name)
- `src/skins/skin_tcp_monocypher.c` - Default skin: TCP + X25519 handshake + XChaCha20-Poly1305 encryption
- `src/skins/skin_tcp_plain.c` - Plain TCP skin: no encryption, length-prefixed raw protocol frames
- `src/skins/skin_tcp_xor.c` - XOR-obfuscated TCP skin: X25519 handshake + repeating-key XOR (obfuscation only, not secure)
- `src/skins/skin_tcp_chacha20.c` - ChaCha20 stream-cipher TCP skin: X25519 handshake + ChaCha20 (no MAC, ~1.5x throughput of monocypher)
- `src/monocypher.c` - Vendored Monocypher library (X25519, BLAKE2b, AEAD fallback)

### C Header Files

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, FAIL, BREAK_IF, CONTINUE_IF, FREE, VALIDATE_ARGS, IN, OUT, INOUT)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE)
- `include/args.h` - Argument parsing, addr_t struct, args_t config, listener_entry_t, connect_entry_t
- `include/network.h` - Network initialization, `g_network` global singleton, callbacks, multi-listener support
- `include/protocol.h` - Protocol message structs, macros, parsing/serialization functions, connect/disconnect/exec/file payloads
- `include/session.h` - Session protocol handling (SESSION__on_message, SESSION__on_connected, etc.), file chunk size
- `include/transport.h` - Transport layer (transport_t, TRANSPORT__recv_msg, TRANSPORT__send_msg, peer metadata)
- `include/routing.h` - Routing table (routing_table_t, route_entry_t, route_type_t), `g_node_id` global singleton
- `include/loadbalancer.h` - Load balancer interface and strategy enums
- `include/tunnel.h` - Tunnel API (TUNNEL__init, TUNNEL__on_message, TUNNEL__handle_disconnect)
- `include/skin.h` - Skin abstraction (skin_ops_t vtable, skin_id_t, registry)

## Architecture

### Layer Separation

The architecture is separated into six distinct layers:

```
+-------------------+     +-------------------+     +-------------------+     +-------------------+     +-------------------+
|      Network      | --> |       Skin        | --> |     Transport     | --> |     Protocol       | --> |     Session       |
| (socket mgmt,    |     | (connect/accept,  |     |  (buffer layer,   |     | (byte order,      |     | (protocol logic,  |
|  accept/reconnect)|     |  handshake,       |     |   recv/send msg) |     |  validation)       |     |  handle messages) |
|                  |     |  encrypt/decrypt) |     |                   |     |                    |     |                   |
+-------------------+     +-------------------+     +-------------------+     +-------------------+     +-------------------+
                                                                                ^
                                                                                |
                                                                         +-------------------+
                                                                         |     Routing       |
                                                                         | (broadcast/forward|
                                                                         |  routing table)   |
                                                                         +-------------------+
```

1. **Network Layer** (`network.c/h`): Socket management and multi-listener support
   - Creates listening endpoints (one per `listener_t`), accepts connections
   - Manages client threads and reconnection logic per peer
   - Owns global `g_network` singleton
   - Does NOT know about node IDs or protocol logic
   - Supports multiple simultaneous listeners bound to different skins

2. **Skin Layer** (`skin.c/h`, `skins/skin_tcp_monocypher.c`): Pluggable transport vertical slice
   - `skin_ops_t` vtable: `connect`, `listener_create`, `listener_accept`, `send_msg`, `recv_msg`, `on_readable`, `on_writable`, `enqueue_outbuf`, `transport_destroy`
   - Each skin owns the full vertical slice: TCP connect/accept, encryption handshake, per-message framing, encrypt/decrypt, teardown
   - Default skin: `tcp-monocypher` (SKIN_ID__TCP_MONOCYPHER = 1) - TCP + X25519 + XChaCha20-Poly1305
   - Skin registry by name and wire-stable ID
   - Network layer initializes listeners with their bound skin and dispatches all I/O through the vtable

3. **Transport Layer** (`transport.c/h`): Thin dispatch layer between Network/Skin and Protocol
   - `TRANSPORT__recv_msg()` - dispatches to `t->skin->recv_msg()`, then calls `PROTOCOL__unserialize()`
   - `TRANSPORT__send_msg()` - calls `PROTOCOL__serialize()`, then dispatches to `t->skin->send_msg()`
   - `TRANSPORT__alloc_base()` / `TRANSPORT__free_base()` / `TRANSPORT__destroy()` - lifecycle management
   - `TRANSPORT__send_to_node_id()` - convenience lookup + send
   - Does NOT directly call send()/recv() anymore - all I/O goes through the skin vtable

4. **Protocol Layer** (`protocol.c/h`): Byte order and validation
   - `PROTOCOL__unserialize()` - validates magic, converts network byte order to host
   - `PROTOCOL__serialize()` - converts host byte order to network, adds magic
   - Session works with host byte order protocol_msg_t

5. **Session Layer** (`session.c/h`): Protocol-specific logic only
   - Handles all message types (NODE_INIT, MSG__RREQ, MSG__RREP, RERR, CONNECTION_REJECTED, EXEC_*, FILE_*, TUNNEL_*)
   - Does NOT handle broadcast/forward logic - this is in routing layer
   - Sends via `TRANSPORT__send_msg()` - never raw sockets

6. **Routing Layer** (`routing.c/h`): Message routing
   - Implements reactive AODV (Ad-hoc On-Demand Distance Vector) algorithm
   - `ROUTING__on_message()` - handles all routing decisions, dynamically updating reverse paths via received message trace data.
   - Transparently broadcast `RREQ` (Route Requests) for unmapped destinations
   - Owns global `g_node_id` and `g_rt` (routing table pointer) singletons
   - `ROUTING__broadcast()` - iterates direct routes, sends to each via transport

### Network Backend

The network layer uses a **thread-per-connection** model. Each TCP connection gets a dedicated thread that performs blocking `recv()` / `send()` via the skin vtable. This is simple, portable, and sufficient for the current mesh tunneler workload. All I/O goes through the skin's `send_msg`/`recv_msg` functions; the network layer itself never calls `send()`/`recv()` directly.

An experimental **epoll event loop** was previously developed on the `feature/epoll` branch but has been removed from master to simplify the codebase and focus on stability.

### Key Design Principles

- **Network knows nothing about node IDs or protocol** - it just manages sockets and listeners
- **Skin owns the full vertical slice** - connect/accept, handshake, framing, encrypt/decrypt, teardown
- **Transport is a thin dispatch layer** - it calls skin vtable and protocol serialize/unserialize
- **All byte order conversion happens in protocol.c** - serialize/unserialize
- **Routing handles broadcast/forward logic** - session only handles protocol-specific logic
- **Global singletons**: `g_network` (network module), `g_node_id` (routing module), `g_rt` (routing module)

### Global Singletons

```c
// network.h - the one network instance
extern network_t g_network;

// routing.h - this node's ID and routing table pointer
extern int g_node_id;
```

### Interface

**main.c wires everything together:**
```c
g_node_id = args.node_id;
SESSION__init(SESSION__get_session(), g_node_id);
SESSION__set_network(SESSION__get_session(), &g_network);
ROUTING__init_globals(SESSION__get_routing_table(SESSION__get_session()), SESSION__on_message);
NETWORK__init(&g_network, &args, g_node_id, ROUTING__on_message, SESSION__on_disconnected, SESSION__on_connected);
```

## Usage

```
./bin/ganon [OPTIONS]

Arguments:
  LISTEN IP       Listen IP address (IPv4 format, default: 0.0.0.0)

Options:
  -p, --port N    Listen port number (1-65535, default: 5555). Shorthand for --listen tcp-monocypher:<ip>:<port>
  -c, --connect   Comma-separated list of IP:port[:skin] to connect (default port: 5555)
  -i, --node-id N Node ID (0 or greater, mandatory)
  -w, --connect-timeout N  Connect timeout in seconds (default: 5)
  --reconnect-retries N    Reconnect retries on disconnect (default: 5, 0 to disable, max/always for unlimited)
  --reconnect-delay N       Delay between reconnect attempts (default: 5 seconds)
  --lb-strategy STR         Load balancing strategy: 'round-robin' (default), 'all-routes', 'sticky'
  --rr-count N              Routes per round-robin step (default: 1)
  --reorder-timeout N       Out-of-order buffering timeout in ms (default: 100)
  --tcp-rcvbuf N            TCP receive buffer size in bytes for tunnel connections (0 = system default)
  --file-chunk-size N       File upload/download chunk size in bytes (default: 262144 = 256KB)
  --reorder                Enable packet reordering/buffering (default: disabled, packets processed immediately)
  --skin NAME              Skin for -p/--port listener (default: tcp-monocypher)
  --default-skin NAME      Skin for outbound --connect entries (default: tcp-monocypher)
  --listen SKIN:IP:PORT    Add a listener (repeatable; if used, -p/--port is ignored)
  -h, --help                Show this help message

Environment variables:
  LISTEN_IP, LISTEN_PORT, CONNECT, NODE_ID, CONNECT_TIMEOUT,
  RECONNECT_RETRIES, RECONNECT_DELAY, LB_STRATEGY, RR_COUNT,
  REORDER_TIMEOUT, TCP_RCVBUF, REORDER, SKIN, DEFAULT_SKIN, LISTEN,
  FILE_CHUNK_SIZE
```

Commands (via Python client):
  c = GanonClient(ip, port, node_id, ..., reorder=False, skin=NetworkSkin.TCP_MONOCYPHER)
  c.connect()                       Connect to local ganon node (sync wrapper)
  await c.a_connect()               Async connect variant
  c.disconnect()                    Disconnect from the network
  c.reconnect()                     Force reconnection
  c.node(node_id, verify=True, timeout=5.0)  Return NodeClient bound to node_id (verifies reachability by default)
  c.connect_to_node(ip, port)       Instruct local node to connect to peer at ip:port, returns NodeClient
  c.disconnect_nodes(node_a, node_b) Disconnect node_a from node_b
  c.print_network_graph()            Print visual graph of network topology from this node's perspective

  All public methods have both sync and `a_` prefixed async variants (e.g., `c.a_ping()`, `c.a_run_command()`).
  The client manages its own asyncio event loop in a background thread; sync wrappers submit coroutines via
  `asyncio.run_coroutine_threadsafe()`.
```

## Build Types

### Release
- Compiler flags: `-O3 -s -Wall -Wextra -Werror`
- Output: `ganon_<ver>_<arch>_release`
- **CRITICAL**: `logging.c` is NOT compiled - no logging code included
- Uses `CMAKE_BUILD_TYPE=Release`

### Debug
- Compiler flags: `-g -O0 -D__DEBUG__ -Wall -Wextra -Werror`
- Output: `ganon_<ver>_<arch>_debug`
- `logging.c` IS compiled with `__DEBUG__` defined
- Uses `CMAKE_BUILD_TYPE=Debug`

## Build Configuration

### CMakeLists.txt Structure

**src/CMakeLists.txt** controls which source files are compiled based on build type:

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LOGGING_SOURCES main.c logging.c args.c network.c session.c transport.c routing.c protocol.c loadbalancer.c tunnel.c monocypher.c skin.c skins/skin_tcp_monocypher.c)
else()
    set(LOGGING_SOURCES main.c args.c network.c session.c transport.c routing.c protocol.c loadbalancer.c tunnel.c monocypher.c skin.c skins/skin_tcp_monocypher.c)
endif()
```

**IMPORTANT**: When adding new source files:
- If the file uses logging macros (LOG_ERROR, LOG_DEBUG, etc.), add it ONLY to the Debug sources list
- If the file does NOT use logging, add it to BOTH lists
- Never add `logging.c` to Release sources - it will cause link errors since logging.h will be missing

### Makefile Targets

- `make` - Builds all targets (x64r, x64d, armr, armd, armv7r, armv7d, mips32ber, mips32bed)
- `make x64` - Builds x64 release and debug
- `make x64r` - Builds x64 release only
- `make x64d` - Builds x64 debug only
- `make arm` - Builds arm release and debug
- `make armv7` - Builds armv7 release and debug
- `make mips32be` - Builds mips32be release and debug
- `make libsodium` - Builds libsodium from `third_party/libsodium` into `third_party/libsodium-install`
- `make clean-libsodium` - Removes libsodium build artifacts
- `make clean` - Removes all build artifacts

### Binary Output

After build, binaries are in `bin/`:
```
bin/
├── ganon_<ver>_x64_release
├── ganon_<ver>_x64_debug
├── ganon_<ver>_arm_release
├── ganon_<ver>_arm_debug
├── ganon_<ver>_armv7_release
├── ganon_<ver>_armv7_debug
├── ganon_<ver>_mips32be_release
└── ganon_<ver>_mips32be_debug
```

A convenience symlink `ganon` in project root points to `./bin/ganon_<ver>_x64_debug` for local development.

## Cross-Compilation

All targets use static linking (`-static` flag):
- x64: Native GCC
- armv5: arm-linux-gnueabihf-gcc (ARMv7 hard-float, no NEON)
- armv7: arm-linux-gnueabihf-gcc (ARMv7-A with NEON-VFPv4 hard-float)
- mips32be: mips-linux-gnu-gcc (MIPS big-endian, o32 ABI)

Toolchain files are in `cmake/`:
- `cmake/armv5-toolchain.cmake`
- `cmake/armv7-toolchain.cmake`
- `cmake/mips32be-toolchain.cmake`

## Code Style

### C Code Style

- Use C11 standard
- Use meaningful function and variable names
- Maximum line length: 100 characters
- Global variables always start with `g_` (e.g., `g_log_level`, `g_node_id`, `g_network`)

## Function Conventions (C)

Every function (except `main`) must:
- Have the module name in all caps with double underscore as prefix (e.g., `ARGS__parse`, `NETWORK__init`)
- Return `err_t` (not `int`)
- Start with `err_t rc = E__SUCCESS;`
- Have an empty line after `err_t rc = E__SUCCESS;`
- Have an `l_cleanup:` label before return
- Return `rc` at the end
- Use output parameters via pointers for returning data
- Use `FAIL_IF(condition, error_code)` to check and fail
- Use `FAIL(error_code)` when failure is unconditional
- Use `BREAK_IF(condition)` and `CONTINUE_IF(condition)` in loops

### Common Macros

```c
#define FAIL_IF(condition, error) \
    if (condition) { rc = error; goto l_cleanup; }

#define FAIL(error) \
    { rc = error; goto l_cleanup; }

#define FREE(ptr) \
    do { \
        if (NULL != (ptr)) { \
            free((ptr)); \
            (ptr) = NULL; \
        } \
    } while (0)

#define VALIDATE_ARGS(...) \
    do { \
        const void *args[] = { __VA_ARGS__ }; \
        for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++) { \
            if (NULL == args[i]) { \
                rc = E__INVALID_ARG_NULL_POINTER; \
                goto l_cleanup; \
            } \
        } \
    } while (0)
```

Comparison convention: static values first (e.g., `NULL != ptr`, `E__SUCCESS != rc`, `0 > value`)

### Parameter Direction Macros

Use `IN`, `OUT`, and `INOUT` macros to document parameter intent in function signatures:

```c
#define IN
#define OUT
#define INOUT
```

- `IN` - Input parameter (passed to the function, not modified)
- `OUT` - Output parameter (returned via pointer)
- `INOUT` - Parameter that is both input and output

Example:
```c
err_t ROUTING__get_route(IN routing_table_t *rt, IN uint32_t node_id, OUT route_entry_t *entry);
```

Preprocessor convention: `#endif` should have a comment indicating which `#ifdef` it closes

## Logging Conventions

Log levels (in order of severity):
- **ERROR** - Catastrophic errors that prevent the program from continuing
- **WARN** - Issues that can be dealt with but indicate potential problems
- **INFO** - Major events (startup, shutdown, connections established)
- **DEBUG** - Smaller events or more detail
- **TRACE** - Detailed tracing data

## Protocol

### Wire Format

All multi-byte integers use network byte order (big-endian).

#### protocol_msg_t (36 bytes header + data)

```
+--------+--------+--------+--------+
|  Magic (4 bytes)           |  "GNN\0"
+--------+--------+--------+--------+
|  Original Source Node ID (4 bytes) |
+--------+--------+--------+--------+
|     Source Node ID (4 bytes)       |
+--------+--------+--------+--------+
|     Destination Node ID (4 bytes) |
+--------+--------+--------+--------+
|       Message ID (4 bytes)       |
+--------+--------+--------+--------+
|        Message Type (4 bytes)    |
+--------+--------+--------+--------+
|       Data Length (4 bytes)      |
+--------+--------+--------+--------+
|       TTL (4 bytes)              |
+--------+--------+--------+--------+
|       Channel ID (4 bytes)       |
+--------+--------+--------+--------+
|       Data (variable)            |
+---------------------------------+
```

#### msg_type_t

```c
typedef enum {
    MSG__NODE_INIT = 0,
    MSG__CONNECTION_REJECTED = 1,
    MSG__RREQ = 2,
    MSG__RREP = 3,
    MSG__RERR = 4,
    MSG__USER_DATA = 5,
    MSG__PING = 6,
    MSG__PONG = 7,
    MSG__TUNNEL_OPEN = 8,
    MSG__TUNNEL_CONN_OPEN = 9,
    MSG__TUNNEL_CONN_ACK = 10,
    MSG__TUNNEL_DATA = 11,
    MSG__TUNNEL_CONN_CLOSE = 12,
    MSG__TUNNEL_CLOSE = 13,
    MSG__CONNECT_CMD = 14,
    MSG__CONNECT_RESPONSE = 15,
    MSG__DISCONNECT_CMD = 16,
    MSG__DISCONNECT_RESPONSE = 17,
    MSG__EXEC_CMD = 18,
    MSG__EXEC_RESPONSE = 19,
    MSG__FILE_UPLOAD = 20,
    MSG__FILE_UPLOAD_RESPONSE = 21,
    MSG__FILE_DOWNLOAD = 22,
    MSG__FILE_DOWNLOAD_RESPONSE = 23,
} msg_type_t;
```

#### Field Semantics

- **orig_src_node_id**: Original sender of the message
- **src_node_id**: Previous hop (the node that forwarded this message)
- **dst_node_id**: Destination node (0 for broadcast)
- **ttl**: Time-to-live for broadcast messages (decremented each hop)
- **channel_id**: Used by load balancer for sticky routing (0 = global/default)

#### Message Data

**RERR**: data contains list of disconnected node IDs that were reachable via the disconnected node (unreachable nodes due to this disconnect). Each node ID is a 4-byte network-order integer.

**TUNNEL_CLOSE**: data is a `tunnel_id_payload_t` structure:
```c
typedef struct __attribute__((packed)) {
    uint32_t tunnel_id;  /* Tunnel to close */
    uint32_t flags;      /* 0 = soft close (default), 1 = force close */
} tunnel_id_payload_t;
```
- **Soft close** (flags=0): Closes the listening port to stop accepting new connections, but keeps existing connections alive until they close naturally.
- **Force close** (flags=1): Immediately closes the listening port and terminates all existing connections.

**CONNECT_CMD**: data is a `connect_cmd_payload_t` structure:
```c
typedef struct __attribute__((packed)) {
    uint32_t request_id;    /* correlation id echoed back in response */
    char target_ip[64];     /* IP address to connect to */
    uint32_t target_port;   /* Port to connect to */
    uint32_t skin_id;       /* 0 = use default skin on the receiving node */
} connect_cmd_payload_t;
```
Used to dynamically connect a node to a new peer. Sent to the node that should initiate the connection.

**CONNECT_RESPONSE**: data is a `connect_response_payload_t` structure:
```c
typedef struct __attribute__((packed)) {
    uint32_t request_id;        /* matches the request */
    uint32_t status;            /* 0=success, 1=refused, 2=timeout, 3=error */
    uint32_t error_code;        /* Implementation-specific error code */
    uint32_t connected_node_id; /* node id of the peer we connected to (0 if unknown) */
} connect_response_payload_t;
```
Response to CONNECT_CMD indicating success or failure with specific error codes. The `connected_node_id` is only known after the peer sends `NODE_INIT`; the C server defers the response until then.

**DISCONNECT_CMD**: data is a `disconnect_cmd_payload_t` structure:
```c
typedef struct __attribute__((packed)) {
    uint32_t node_a;  /* First node (initiator) */
    uint32_t node_b;  /* Second node (target to disconnect from) */
} disconnect_cmd_payload_t;
```
Used to dynamically disconnect two nodes. Sent to node_a which will terminate its connection to node_b.

**DISCONNECT_RESPONSE**: data is a `disconnect_response_payload_t` structure:
```c
typedef struct __attribute__((packed)) {
    uint32_t status;      /* 0=success, 1=not_connected, 2=error */
    uint32_t error_code;
} disconnect_response_payload_t;
```
Response to DISCONNECT_CMD indicating success or failure.

**EXEC_CMD**: data layout:
```
[4 bytes]  request_id (correlation ID, network byte order)
[N bytes]  command string (null-terminated)
```
Sent to a node to execute a shell command. The target node forks `/bin/sh -c <cmd>` and captures stdout/stderr separately.

**EXEC_RESPONSE**: data layout:
```
[4 bytes]  request_id (matches the request)
[4 bytes]  exit_code
[4 bytes]  stdout_len
[4 bytes]  stderr_len
[N bytes]  stdout data
[M bytes]  stderr data
```

**FILE_UPLOAD**: data layout:
```
[4 bytes]  request_id
[256 bytes] remote path (null-padded string)
[4 bytes]  chunk_index (0-based, 0 for single upload)
[4 bytes]  total_chunks (1 for single upload, >1 for chunked)
[N bytes]  file data (chunk data for chunked uploads)
```

**FILE_UPLOAD_RESPONSE**: data layout:
```
[4 bytes]  request_id
[4 bytes]  status (0=success, 2=no space, 3=read-only, 4=permission denied, 5=other)
[256 bytes] error message (null-padded, empty on success)
```

**FILE_DOWNLOAD**: data layout:
```
[4 bytes]  request_id
[N bytes]  path string (null-terminated)
[4 bytes]  offset (byte offset to start reading, 0 = start of file)
[4 bytes]  length (bytes to read, 0 = entire file for backwards compat)
```

**FILE_DOWNLOAD_RESPONSE**: data layout:
```
[4 bytes]  request_id
[4 bytes]  status (0=success, 1=not found, 4=permission denied, 5=other)
[4 bytes]  total_size (total file size on disk, 0 if error)
[N bytes]  file data (if success) or error message (if failure)
```

## Data Structures

### route_entry_t

```c
typedef struct route_entry {
    uint32_t node_id;
    uint32_t next_hop_node_id;
    route_type_t route_type;
    int fd;
    time_t last_updated;
    uint8_t hop_count;
} route_entry_t;
```

### routing_table_t

```c
typedef struct {
    route_entry_t entries[ROUTING_TABLE_MAX_ENTRIES];
    size_t entry_count;
    pthread_mutex_t mutex;
} routing_table_t;
```

### network_t

```c
struct network_t {
    listener_t     *listeners;
    int             listener_count;

    socket_entry_t *clients;
    pthread_mutex_t clients_mutex;
    int             running;

    addr_t *connect_addrs;   /* legacy pointer into args; skin_id in connect_entries */
    connect_entry_t *connect_entries;
    int              connect_count;
    pthread_t       *connect_threads;
    int              connect_thread_count;
    int              connect_timeout;
    int              reconnect_retries;
    int              reconnect_delay;
    int              node_id;
    uint32_t         default_skin_id;

    network_message_cb_t     message_cb;
    network_disconnected_cb_t disconnected_cb;
    network_connected_cb_t    connected_cb;
};
```

### listener_t

```c
typedef struct {
    const skin_ops_t *skin;
    skin_listener_t  *skin_listener;
    int               listen_fd;   /* -1 if skin manages its own I/O */
    addr_t            addr;
    pthread_t         accept_thread;
    network_t        *net;
    int               running;
} listener_t;
```

### transport_t

```c
struct transport {
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    uint32_t node_id;
    void *ctx;   /* generic back-pointer (e.g. network_t*); skin state is in skin_ctx */

    /* Pluggable skin vtable + per-connection opaque state. */
    const skin_ops_t *skin;
    void *skin_ctx;
};
```

### skin_ops_t

```c
struct skin_ops {
    uint32_t    skin_id;   /* wire-stable, matches skin_id_t */
    const char *name;      /* "tcp-monocypher", "tls13", etc. */

    err_t (*listener_create)(IN const addr_t *addr,
                              OUT skin_listener_t **out_listener,
                              OUT int *out_listen_fd);
    err_t (*listener_accept)(IN skin_listener_t *l,
                              OUT transport_t **out_transport);
    void  (*listener_destroy)(IN skin_listener_t *l);

    err_t (*connect)(IN const char *ip, IN int port,
                      IN int connect_timeout_sec,
                      OUT transport_t **out_transport);

    err_t (*send_msg)(IN transport_t *t, IN const protocol_msg_t *msg,
                       IN const uint8_t *data);
    err_t (*recv_msg)(IN transport_t *t, OUT protocol_msg_t *msg,
                       OUT uint8_t **data);

    void  (*transport_destroy)(IN transport_t *t);
};
```

## Routing Functions

### ROUTING__on_message

Main routing handler - called for every received message:
- If dst == this node: calls session callback (message for us)
- If dst == 0 (broadcast): calls session callback AND broadcasts to all direct peers except src
- If dst != this node and != 0: forwards to next hop (ttl decremented, src updated to this node)
- Drops messages with TTL == 0

### ROUTING__broadcast

Sends message to all direct peers except one:
- Iterates routing table for DIRECT routes
- Excludes specified node_id from broadcast
- Decrements TTL and updates src_node_id before sending
- Uses NETWORK__get_transport + TRANSPORT__send_msg internally

## Load Balancing

When multiple routes exist to the same destination, the load balancer (`loadbalancer.c`) selects which route(s) to use:

- **round-robin** (default): Cycles through available routes. The `--rr-count N` flag (default 1) controls how many consecutive routes are used per message — e.g. with 3 routes and `--rr-count 2`, packet 1 -> routes [1,2], packet 2 -> routes [2,3], packet 3 -> routes [3,1].
- **all-routes**: Sends to all available routes simultaneously.
- **sticky**: Non-zero `channel_id` -> always uses route `(channel_id-1) % route_count` (sticky per channel). `channel_id=0` -> falls back to round-robin with rr_count. The `round-robin` and `all-routes` strategies always ignore `channel_id`.

Set strategy via `--lb-strategy` CLI flag or `LB_STRATEGY` env; set rr-count via `--rr-count` or `RR_COUNT` env.

**Channel ID** (`channel_id` in the message header):
- Meaningful only in `sticky` strategy. Channel 0 = global (round-robin). Channel N -> route `(N-1) % route_count`.
- PONG responses automatically mirror the channel_id of the incoming PING (C session and Python client both).
- Python client: pass `channel_id=N` to `send_to_node()` or `ping()`.

**Wire format note**: `channel_id` is the last field in `protocol_msg_t` (added after `ttl`). Header is 36 bytes. Old nodes (32-byte header) are incompatible.

## Error Codes

Errors are defined in `include/err.h` as enum `err_t`:
- First error must be `E__SUCCESS = 0`
- Naming convention: `E__<MODULE>_<FUNCTION>_<ERROR>`
- Hex ranges:
  - Generic: 0x001-0x0FF
  - args: 0x200-0x2FF
  - network: 0x300-0x3FF
  - session: 0x401-0x4FF
  - routing: 0x501-0x5FF
  - crypto: 0x601-0x6FF
  - udp-quic2 skin: 0x700-0x7FF

## Skin Abstraction

Ganon uses a **pluggable skin system** to decouple the network layer from transport security and framing.

A **skin** is a vtable (`skin_ops_t`) that owns the full vertical slice for a given wire format:
- **Listener lifecycle**: `listener_create`, `listener_accept`, `listener_destroy`
- **Client dial**: `connect`
- **Per-message I/O**: `send_msg`, `recv_msg`
- **Teardown**: `transport_destroy`

The default skin is `tcp-monocypher` (ID 1): plain TCP sockets with an X25519 handshake followed by XChaCha20-Poly1305 encryption. All encryption state lives in the skin's opaque `skin_ctx` (not in `transport_t`).

The `tcp-plain` skin (ID 2) provides unencrypted TCP transport with length-prefixed raw protocol frames. It is useful for debugging, testing, or environments where encryption overhead is undesirable:
```
[4 bytes]  Big-endian payload length
[N bytes]  Plaintext serialized protocol_msg_t + data
```

The `tcp-xor` skin (ID 3) uses the same X25519 handshake and BLAKE2b key derivation as `tcp-monocypher`, but applies a repeating-key XOR with the derived 32-byte directional key instead of XChaCha20-Poly1305. This provides per-connection obfuscation (traffic looks different from the source plaintext and different on every link) but **not** cryptographic security. It is useful when you only need lightweight obfuscation:
```
[4 bytes]  Big-endian payload length
[N bytes]  XOR-obfuscated serialized protocol_msg_t + data
```

The `tcp-chacha20` skin (ID 4) uses the same X25519 handshake and BLAKE2b key derivation, but applies ChaCha20 as a raw stream cipher (no Poly1305 MAC). This eliminates the authentication overhead while still providing strong per-connection obfuscation. On x64 with libsodium it achieves ~1.5x the tunnel throughput of `tcp-monocypher`:
```
[4 bytes]  Big-endian payload length
[8 bytes]  Nonce (little-endian message counter)
[N bytes]  ChaCha20-obfuscated serialized protocol_msg_t + data
```

Network layer multi-listener support:
- Each `listener_t` is bound to exactly one skin
- `--listen SKIN:IP:PORT` adds a listener (repeatable, up to `ARGS_MAX_LISTENERS`)
- `-p/--port PORT` is shorthand for `--listen tcp-monocypher:0.0.0.0:PORT`
- `--skin NAME` sets the skin for the `-p/--port` shorthand
- `--default-skin NAME` sets the skin for outbound `--connect` entries
- `connect_cmd_payload_t.skin_id` allows a Python client to request a specific skin for dynamic connects

## Transport-Layer Encryption

All TCP connections between ganon nodes are encrypted using **X25519 + XChaCha20-Poly1305**. Encryption lives entirely inside the active skin (default: `skin_tcp_monocypher.c`) and is transparent to all layers above transport.

### Cryptography
- **Key exchange:** Ephemeral X25519 per TCP connection (Monocypher)
- **Cipher:** XChaCha20-Poly1305 (AEAD, 24-byte nonce)
- **Key derivation:** BLAKE2b with single-byte context strings (`S`, `R`, `I`) (Monocypher)
- **AEAD implementation:** Monocypher (reference C implementation) for cross-compilation targets; **libsodium** (AVX2/SSE4.1-optimized assembly) for x64 when available
- **Randomness:** `getrandom()` when available, fallback to `/dev/urandom`
- **No authentication / no PSK:** Protects against passive eavesdropping and active tampering, but not MITM impersonation

### Why libsodium for x64

Monocypher is a compact, auditable, single-file reference implementation written in portable C. libsodium is a production library that includes hand-optimized assembly for common platforms. For XChaCha20-Poly1305 on x64:

**Technical differences:**
- **ChaCha20:** libsodium uses 4-way or 8-way parallel block processing via AVX2/SSE4.1 SIMD instructions. It encrypts multiple 64-byte blocks simultaneously in a single round, whereas Monocypher processes one block at a time in portable C.
- **Poly1305:** libsodium uses SIMD (SSE2/AVX2) to process multiple message blocks in parallel for the universal hash computation. Monocypher uses a portable 32-bit limb implementation.
- **Result:** libsodium achieves ~3.6x higher throughput in isolated crypto benchmarks.

**Isolated benchmark** (65 KB frames, tight loop, `-O3`, same x64 machine):

| Implementation | Throughput |
|----------------|------------|
| Monocypher (reference C) | **2.49 Gbps** |
| libsodium (AVX2/SSE4.1 assembly) | **8.97 Gbps** |

**Tunnel benchmark** (1 GB over local TCP tunnel) shows high variance (~2.5-4.5 Gbps for both builds) because the end-to-end path is bottlenecked by syscall overhead, thread context switches, and Python client/protocol framing -- not by raw crypto speed. libsodium removes crypto from the critical path; further tunnel throughput gains require optimizing the non-crypto pipeline.

### Self-Contained Build

libsodium is vendored in `third_party/`:
- `third_party/libsodium/` -- libsodium 1.0.20 source tree (rebuildable)
- `third_party/libsodium-install/` -- prebuilt static library and headers for x64

The ganon `Makefile` has a `libsodium` target that builds from source:
```
make libsodium    # Build third_party/libsodium -> third_party/libsodium-install
make x64r         # Builds ganon linked against local libsodium
make clean-libsodium  # Remove build artifacts
```

`src/CMakeLists.txt` auto-detects `${CMAKE_SOURCE_DIR}/third_party/libsodium-install/lib/libsodium.a` and defines `USE_LIBSODIUM` when present. Cross-compilation targets (ARM, MIPS) skip libsodium detection and fall back to Monocypher.

### Handshake
After TCP connect, before any ganon protocol traffic:
1. Both sides generate an ephemeral X25519 keypair
2. Initiator sends 32-byte ephemeral public key
3. Responder sends 32-byte ephemeral public key
4. Both compute shared secret and derive directional `send_key` / `recv_key`
5. Session ID derived from shared secret for internal tracking only (never on wire)

**Note:** libsodium's `crypto_scalarmult_curve25519()` does not clamp the scalar internally, whereas Monocypher's `crypto_x25519()` does. To maintain wire compatibility with the Python client (which uses Monocypher), the C server keeps Monocypher for X25519 key exchange and BLAKE2b key derivation, using libsodium **only** for the XChaCha20-Poly1305 AEAD encrypt/decrypt.

### Wire Format (post-handshake)
```
[4 bytes]  Big-endian frame length
[24 bytes] Nonce (16 zero bytes + 8-byte LE counter)
[16 bytes] Poly1305 MAC
[N bytes]  Ciphertext of serialized (protocol_msg_t || data)
```
- **AAD:** 0 bytes (no visible metadata to minimize traffic fingerprinting)
- **Replay protection:** Strict 64-bit nonce equality check per session
- **Overhead:** 44 bytes per frame

### Integration (C Server)
- `skin_tcp_monocypher.c` performs handshake in `connect()` and `listener_accept()` before returning a ready `transport_t`
- `skin->send_msg()` encrypts via `crypto_aead_xchacha20poly1305_ietf_encrypt_detached()` (libsodium) or `crypto_aead_lock()` (Monocypher) into length-prefixed frame
- `skin->recv_msg()` reads length-prefixed frame, verifies nonce, decrypts via `crypto_aead_xchacha20poly1305_ietf_decrypt_detached()` (libsodium) or `crypto_aead_unlock()` (Monocypher), then unserializes
- Session, routing, and tunnel layers are completely unaware of encryption

### Integration (Python Client)
- `ganon_client/skins/tcp_monocypher.py` implements `NetworkSkinImpl` ABC
- `TcpMonocypherSkin.open()` performs X25519 exchange and key derivation
- `TcpMonocypherSkin.send()` calls `monocypher.bindings.crypto_lock()`
- `TcpMonocypherSkin.recv()` calls `monocypher.bindings.crypto_unlock()`
- Both C and Python use the same Monocypher primitives for handshake/key derivation, ensuring full compatibility

**Important API difference:** `monocypher-py`'s `crypto_blake2b(msg, key=...)` takes the *message* as the first argument and the *key* as the keyword argument. The C `crypto_blake2b_keyed(hash, hash_size, key, key_size, message, message_size)` takes the *key* as the 3rd argument and the *message* as the 5th. When deriving directional keys in Python, the shared secret must be passed as `key=shared` and the single-byte context (`b"S"`/`b"R"`) as the message.

### Files
- `src/monocypher.c` + `include/monocypher.h` -- vendored Monocypher library (X25519, BLAKE2b, AEAD fallback)
- `src/skins/skin_tcp_monocypher.c` -- default skin: handshake, encrypt, decrypt (conditionally uses libsodium AEAD)
- `src/network.c` -- multi-listener accept loop, dynamic connect/disconnect
- `src/CMakeLists.txt` -- auto-detects and links local libsodium (`USE_LIBSODIUM`)
- `src/main.c` -- calls `sodium_init()` when `USE_LIBSODIUM` is defined
- `include/skin.h` -- skin vtable and registry
- `include/transport.h` -- transport_t with skin pointer
- `include/err.h` -- crypto error codes (`E__CRYPTO__*`)
- `third_party/libsodium/` -- libsodium 1.0.20 source
- `third_party/libsodium-install/` -- prebuilt x64 static library and headers
- `ganon_client/skins/tcp_monocypher.py` -- Python skin implementation
- `ganon_client/skin.py` -- Python skin ABC and registry
- `ganon_client/pyproject.toml` -- `monocypher-py` dependency

### Performance on ARM and MIPS

**ARM:**
- libsodium includes NEON-optimized assembly for ARMv7+ and AArch64. On those platforms, expect a **2-4x** isolated-crypto improvement over Monocypher.
- Our ARMv5 target (`arm-linux-gnueabihf`) does **not** have NEON. On ARMv5, libsodium falls back to its reference C implementation, which is only marginally faster than Monocypher (maybe **1.2-1.5x**). For ARMv5, Monocypher remains a perfectly viable choice.
- **Recommendation:** If you ever move to ARMv7+ or AArch64, cross-compile libsodium with `--host=arm-linux-gnueabihf` (or `aarch64-linux-gnu`) and link it; the NEON paths will unlock significant gains.

**MIPS (big-endian, o32 ABI):**
- libsodium has limited MIPS-specific assembly. Most primitives fall back to reference C.
- Expected improvement over Monocypher on MIPS: **1.0-1.3x** (often negligible).
- Monocypher is competitive on MIPS because it is already well-optimized portable C.

### How to Improve Performance on ARM/MIPS

Since raw crypto speed is not the main bottleneck on these platforms (and libsodium offers limited gains), focus on **non-crypto optimizations**:

1. **Increase tunnel buffer size** (`TUNNEL_BUF_SIZE` in `include/tunnel.h`). Larger buffers mean fewer encrypt/decrypt calls per gigabyte transferred, amortizing function-call and nonce-construction overhead.
2. **Batch small reads** in `fwd_thread_func()` (`src/tunnel.c`). Instead of `recv()` -> encrypt -> send for every small chunk, read as much as possible before encrypting. This reduces the number of `TRANSPORT__send_msg()` calls.
3. **Zero-copy optimizations:**
   - Use `sendfile()` or `splice()` for tunnel forwarding when both source and destination are local sockets, bypassing userspace entirely.
   - Avoid the `malloc()`/`free()` in `TRANSPORT__recv_msg()` by using a stack-allocated frame buffer up to a reasonable size (e.g., 68 KB).
4. **Reduce wire overhead:** The nonce prefix is always 16 zero bytes. Consider sending only the 8-byte counter on the wire and reconstructing the full 24-byte nonce on receipt. This saves 16 bytes per frame (~0.024% overhead reduction for 65 KB frames, but more meaningful for small control messages).
5. **kernel TLS (kTLS):** On Linux 4.17+ with OpenSSL or wolfSSL, kTLS can offload ChaCha20-Poly1305 to the kernel, eliminating userspace crypto and syscall overhead entirely. This is a larger architectural change but can double tunnel throughput on any architecture.
6. **Cross-compile libsodium properly for ARM:** If you upgrade to ARMv7+ or AArch64, build libsodium with the cross-compiler:
   ```
   cd third_party/libsodium
   ./configure --host=arm-linux-gnueabihf --prefix=... \
       CC=arm-linux-gnueabihf-gcc CFLAGS="-O3" \
       --enable-static --disable-shared --disable-tests
   ```
   Then update `src/CMakeLists.txt` to point at the ARM install prefix for ARM builds.

### Future: End-to-End Encryption
The current design is hop-to-hop (protects the link). End-to-end encryption can be added later without changing the frame format: add a second encryption layer inside `session.c` or `routing.c` that encrypts the `data` payload before `TRANSPORT__send_msg` sees it. The hop-to-hop layer will then encrypt the already-encrypted payload, providing double encryption.

## Python Client API

### Setup

Use the project-local virtual environment:
```bash
source venv/bin/activate
```

Dependencies (e.g., `monocypher-py`) are installed into this venv.

### Async Architecture

The Python client (`ganon_client/client.py`) is built on an **async-native core** with **sync wrappers**. All network I/O runs on an internal `asyncio` event loop in a background daemon thread. Response correlation uses `asyncio.Future` instead of `threading.Event`.

**Internal async methods** (no `a_` prefix because they're internal):
- `_protocol_loop()` -- reads encrypted frames from the socket
- `_send_protocol_message()` -- sends encrypted frames
- `_send_rreq()`, `_flush_pending_messages()`, `_send_node_init()`

**Public async methods** (`a_` prefix) return `_AsyncBridge` objects that can be `await`-ed from any event loop:
```python
# From your own async code
result = await c.a_run_command(10, "uptime")
```

**Public sync methods** block the caller thread and internally delegate to the async implementation via `asyncio.run_coroutine_threadsafe()`:
```python
# From sync code
result = c.run_command(10, "uptime")
```

### `_AsyncBridge`

`_AsyncBridge` is a small awaitable that bridges a coroutine running on the client's internal loop to the caller's async context:

```python
class _AsyncBridge:
    def __init__(self, coro, loop):
        self._coro = coro
        self._loop = loop

    def __await__(self):
        future = asyncio.run_coroutine_threadsafe(self._coro, self._loop)
        wrapped = asyncio.wrap_future(future)
        return wrapped.__await__()
```

This means `await c.a_ping(10)` works from any async context (your own event loop, Jupyter, etc.) because the actual coroutine executes on the client's internal loop and the result is bridged back.

### Dual API Reference

Every external-facing command has both a sync and an async version:

| Sync Method | Async Method | Description |
|---|---|---|
| `connect()` | `a_connect()` | Connect to local ganon node |
| `disconnect()` | `a_disconnect()` | Disconnect and cleanup |
| `reconnect()` | `a_reconnect()` | Force reconnection |
| `ping(node)` | `a_ping(node)` | Ping a remote node |
| `send_to_node(node, data)` | `a_send_to_node(node, data)` | Send raw data to a node |
| `create_tunnel(...)` | `a_create_tunnel(...)` | Create a tunnel |
| `connect_to_node(ip, port)` | `a_connect_to_node(ip, port)` | Instruct a node to connect to a peer |
| `disconnect_nodes(a, b)` | `a_disconnect_nodes(a, b)` | Disconnect two nodes |
| `run_command(node, cmd)` | `a_run_command(node, cmd)` | Execute command on remote node |
| `run(node, cmd)` | `a_run(node, cmd)` | Execute command, return merged output |
| `upload_file(node, local, remote)` | `a_upload_file(node, local, remote)` | Upload a file |
| `download_file(node, remote, local)` | `a_download_file(node, remote, local)` | Download a file |
| `print_network_graph()` | `a_print_network_graph()` | Print topology graph |

`NodeClient` exposes the same dual API for all node-bound operations:
```python
nc = c.node(10)
nc.ping()           # sync
await nc.a_ping()   # async
nc.run_command("uptime")           # sync
await nc.a_run_command("uptime")   # async
```

### `node()` -- Reachability Verification

`GanonClient.node(node_id, verify=True, timeout=5.0)` returns a `NodeClient` bound to the specified node id. By default it sends a ping to verify the node is reachable before returning.

```python
nc = c.node(10)                    # pings node 10 before returning (default)
nc = c.node(10, verify=False)      # skips ping check
nc = c.node(10, timeout=2.0)       # custom ping timeout
```

Raises `TimeoutError` if `verify=True` and the node does not respond within `timeout` seconds.

### Skin Selection

The Python client supports pluggable skins via the `skin` parameter:
```python
from ganon_client.skin import NetworkSkin

c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.TCP_MONOCYPHER)
```

The default is `NetworkSkin.TCP_MONOCYPHER`. Skin implementations live in `ganon_client/skins/` and register themselves via `register_skin()`.

Available skins:
- `NetworkSkin.TCP_MONOCYPHER` (id=1) — default, TCP + X25519 + XChaCha20-Poly1305
- `NetworkSkin.TCP_PLAIN` (id=2) — unencrypted TCP (debugging)
- `NetworkSkin.TCP_XOR` (id=3) — TCP + X25519 + repeating-key XOR
- `NetworkSkin.TCP_CHACHA20` (id=4) — TCP + X25519 + ChaCha20 stream
- `NetworkSkin.TCP_SSH` (id=5) — TCP + libssh
- `NetworkSkin.UDP_QUIC` (id=6) — broken, kept for reference
- `NetworkSkin.UDP_QUIC2` (id=7) — UDP + QUIC (ngtcp2 + picotls/mbedTLS), ALPN "ganon-q2", working

```python
# Use the unencrypted plain TCP skin (useful for debugging)
c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.TCP_PLAIN)

# Use the working QUIC skin (requires aioquic + cryptography packages)
c = GanonClient("127.0.0.1", 5555, 99, skin=NetworkSkin.UDP_QUIC2)
```

### Exception Policy

All methods raise exceptions on failure instead of returning `False`/`None`/dict:
- `ConnectionError` -- not connected, send failure, TCP error, handshake failure, reconnect failure
- `TimeoutError` -- no response within timeout (ping, command execution, file transfer, connect_to_node)
- `ConnectToNodeError` -- remote node reports connection failure (has `.status` and `.error_code` attributes)

### Usage Examples

**Sync usage:**
```python
from ganon_client.client import GanonClient

c = GanonClient("127.0.0.1", 5555, 99)
c.connect()
print(c.ping(10))
result = c.run_command(10, "uptime")
print(result["exit_code"], result["stdout"])
c.disconnect()
```

**Async usage:**
```python
import asyncio
from ganon_client.client import GanonClient

c = GanonClient("127.0.0.1", 5555, 99)
await c.a_connect()
lat = await c.a_ping(10)
result = await c.a_run_command(10, "uptime")
await c.a_disconnect()
```

**NodeClient:**
```python
nc = c.node(10)
await nc.a_ping()
await nc.a_run_command("cat /etc/os-release")
```

## TODO

- [x] Major refactor: separate network/session/transport layers
- [x] Fix byte order conversion - session works in host order, transport serializes
- [x] Move broadcast/forward logic to routing layer (ROUTING__on_message)
- [x] Add IN/OUT/INOUT parameter direction macros
- [x] NODE_DISCONNECT carries list of unreachable nodes
- [x] Add ROUTING__get_via_nodes function
- [x] Reconnection logic: connect_and_run_thread handles reconnect loop per peer
- [x] Update Python client to match new architecture (encryption, protocol, routing)
- [x] Test multi-node mesh topology
- [x] Migrated routing to reactive AODV mesh logic
- [ ] Implement AODV message structs into Python Client
- [x] Implement multi-path Load Balancing (Round-Robin, All-Routes, Sticky)
- [x] Implement sequential msg_id tracking, deduplication, and reordering buffer (C & Python)
- [x] Fix protocol mismatch between C and Python client
- [x] Improve RERR logic for multi-path mesh reachability
- [x] Created Docker test environment for multi-node testing
- [x] Fix tunnel close race condition - set listen socket to non-blocking so accept() can be interrupted by close()
- [x] Fix tunnel creation without existing AODV route - buffer messages and trigger RREQ in ROUTING__route_message when no route exists
- [x] Implement tunnel soft close - `t.close()` stops accepting new connections but keeps existing ones alive; `t.close(force=True)` for immediate termination
- [x] Fix tunnel multi-connection channel isolation - use unique channel_id per connection (conn_id) for proper load balancer sequencing, master channel (tunnel_id) for control messages
- [x] Fix UDP tunnel support - create SOCK_DGRAM sockets for UDP, use recvfrom/sendto instead of accept/send/recv, track UDP clients by address
- [x] Fix UDP tunnel destination-side forwarding - properly track UDP connections in dst_conn_t and use sendto() with correct remote address
- [x] Implement tunnel connection persistence during route outages - TCP connections enter "stalled" state with backpressure when no route exists, resume when route is restored (UDP packets are lost as expected)
- [x] Add --tcp-rcvbuf CLI option and TCP_RCVBUF env variable to configure TCP receive buffer size for tunnel connections
- [x] Add dynamic connect/disconnect feature - `c.connect(ip, port, target_node)` to connect nodes, `c.disconnect(node_a, node_b)` to disconnect nodes, with proper error handling
- [x] Add network graph visualization - `c.print_network_graph()` to display network topology from the client's perspective, properly handling loops and parallel routes
- [x] Fix message ID generation - was using Unix timestamp which made message IDs look like node IDs (e.g., 1776634649), now uses sequential counter starting from 1
- [x] Fix ping timeout when no route exists - C server: flush buffered messages when RREP establishes route; Python client: buffer messages and trigger RREQ when no route, flush when RREP arrives
- [x] Add optional packet reordering/buffering - CLI flag `--reorder` and Python param `reorder=False` (default), packets processed immediately when disabled
- [x] Fix multi-path routing deduplication - don't drop "duplicate" unicast messages at intermediate nodes, only drop broadcast duplicates
- [x] Fix tunnel duplicate connection creation - check if connection already exists before creating new one (prevents duplicate connections with multi-path routing)
- [x] Fix tunnel soft-close reuse - allow soft-closed tunnels to be recreated (check `is_soft_closed` flag in addition to `is_active`)
- [x] Add encryption at the transport layer
- [x] Switch Python client from `cryptography` to `monocypher-py` for Monocypher compatibility
- [x] Revert C server from IETF ChaCha20 (12-byte nonce) to XChaCha20 (24-byte nonce) for Monocypher API compatibility
- [x] Fix BLAKE2b key derivation in Python client - `monocypher-py` takes `crypto_blake2b(msg, key=...)` where C takes `crypto_blake2b_keyed(hash, hash_size, key, key_size, msg, msg_size)`; shared secret must be passed as `key=shared` and context byte as `msg`
- [x] Integrate libsodium for high-performance XChaCha20-Poly1305 AEAD on x64 (~3.6x isolated crypto improvement: 8.97 Gbps vs 2.49 Gbps)
- [x] Maintain Monocypher for X25519/BLAKE2b to preserve Python client wire compatibility
- [x] Move libsodium source and build artifacts into `third_party/` for self-contained builds
- [x] Add Makefile `libsodium` and `clean-libsodium` targets to rebuild from vendored source
- [x] Update CMake auto-detection to use project-local `third_party/libsodium-install/`
- [x] Add ARMv7 toolchain (`cmake/armv7-toolchain.cmake`) with NEON flags and Makefile targets `armv7r`/`armv7d`
- [x] Increase TUNNEL_BUF_SIZE from 64K to 128K (+8% tunnel throughput, 128K chosen for embedded RAM constraints)
- [x] Fix stack-allocated `buf[TUNNEL_BUF_SIZE + 8]` in `handle_tunnel_conn_ack` to use `MAX_PRE_ACK_DATA + 8`
- [x] Increase encrypted frame size limit from 200KB to 300KB in both C server and Python client
- [x] Implement stack-allocated frame buffer in `TRANSPORT__recv_msg` (avoids malloc on hot path for typical frames)
- [x] Create `PERFORMANCE_PLAN.md` with ranked list of future optimizations (io_uring, kTLS, splice, batching, etc.)
- [x] Add detailed Appendix A to `PERFORMANCE_PLAN.md` covering io_uring on legacy kernels (2.6.x, 3.3.x) and embedded architectures (ARM/MIPS)
- [x] Add kernel-generation-specific optimization availability table (2.6.x vs 3.3.x vs 5.6+) with estimated impact per generation
- [x] Implement runtime kernel capability probing (`network_caps.c/h`) — **removed from master, preserved on `feature/epoll` branch**
- [x] Implement epoll event loop foundation (`network_epoll.c/h`) with per-transport recv buffers and outbound queues — **removed from master, preserved on `feature/epoll` branch**
- [x] Refactor `TRANSPORT__recv_msg()` to extract `TRANSPORT__decrypt_frame()` for reuse by epoll loop — **reverted on master**
- [x] Build and test epoll mode on localhost (two-node mesh) — **reverted on master**
- [x] Build and test thread-per-connection fallback compilation
- [x] Fix SIGINT shutdown hang — `NETWORK__shutdown` now closes client fds before joining threads; `connect_and_run_thread` and `socket_thread_func` have clean ownership boundaries; fixed use-after-free in `socket_thread_func`
- [x] Add remote command execution - `c.run_command(node, "cmd")` returns dict with exit_code/stdout/stderr; `c.run(node, "cmd")` returns merged output
- [x] Add remote file upload/download - `c.upload_file(node, local, remote)` and `c.download_file(node, remote, local)` with helpful error messages (no space, read-only fs, permission denied, not found)
- [x] Add EXEC_CMD/EXEC_RESPONSE, FILE_UPLOAD/FILE_UPLOAD_RESPONSE, FILE_DOWNLOAD/FILE_DOWNLOAD_RESPONSE protocol messages
- [x] Add NodeClient - `nc = c.node(30)` binds all commands to a specific node id so you can call `nc.run_command("cmd")`, `nc.upload_file(local, remote)`, `nc.ping()`, etc. without repeating the node id
- [x] Make `connect_to_node()` synchronous - add `request_id` correlation to `CONNECT_CMD/RESPONSE`, defer `CONNECT_RESPONSE` in C server until peer `NODE_INIT` arrives so the peer node id is known, handle pending-connect cleanup on disconnect
- [x] Make `connect_to_node()` return `NodeClient` (via `self.node()`) and raise `ConnectToNodeError` on remote failure
- [x] Make `node()` verify reachability with a ping before returning (configurable via `verify=` and `timeout=`)
- [x] Python client: convert silent failures to explicit exceptions - `connect()`, `reconnect()`, `_connect()`, `_send_protocol_message()`, `send_to_node()`, `disconnect_nodes()` all raise `ConnectionError` instead of returning `False`/`None`/dict; `run_command()`, `upload_file()`, `download_file()` no longer manually check send return values
- [x] Refactor Python client to async-capable architecture - manages internal asyncio event loop in background thread, all I/O methods are async with `a_` prefixes, sync wrappers use `asyncio.run_coroutine_threadsafe()`, response correlation uses `asyncio.Future` instead of `threading.Event`
- [x] Add pluggable network skins abstraction - `skin_ops_t` vtable for connect/accept/listen/send/recv/teardown, registry by name and ID, multi-listener support, `--skin`/`--default-skin`/`--listen` CLI flags, `ConnectCmdPayload` gains `skin_id` field
- [x] Implement Python skin abstraction - `NetworkSkin` enum, `NetworkSkinImpl` ABC, `TcpMonocypherSkin` class, skin registry, `GanonClient(skin=NetworkSkin.TCP_MONOCYPHER)` parameter
- [x] Add file chunking for uploads/downloads - `--file-chunk-size` CLI flag (default 256KB), `FILE_CHUNK_SIZE` env, `GanonClient(file_chunk_size=...)` parameter, chunked FILE_UPLOAD with chunk_index/total_chunks, chunked FILE_DOWNLOAD with offset/length, FILE_DOWNLOAD_RESPONSE includes total_size
- [x] Replace `run_in_executor` socket connect with native asyncio `loop.sock_connect` in TcpMonocypherSkin
- [x] Add plain TCP skin (`tcp-plain`) for debugging/testing without encryption (C + Python)
- [x] Add XOR-obfuscated TCP skin (`tcp-xor`): X25519 handshake + repeating-key XOR for lightweight per-connection obfuscation (C + Python)
- [x] Add ChaCha20 stream-cipher TCP skin (`tcp-chacha20`): X25519 handshake + ChaCha20 (no MAC, ~1.5x throughput of monocypher) (C + Python)
- [ ] Add TLS 1.3 skin (`tls13`) as alternative to `tcp-monocypher`
- [ ] Add QUIC skin (`quic`) for UDP-based transport
- [ ] Test multi-path tunnel race conditions without `-vv` flag (Bug 1 fix verification)

## Known Bugs - Multi-Path Tunnel Race Conditions

### Bug 1: Dedup logic collision between RREQ and TUNNEL_CONN_OPEN (FIXED)

**Symptom**: In multi-path environments (`--rr-count 2`), TUNNEL_CONN_OPEN messages are dropped as duplicates when they're actually new messages.

**Root cause**: The dedup key in `ROUTING__is_msg_seen()` used only `orig_src + msg_id`, not the message type. When node 30 sent both RREQ (type=2) and CONN_OPEN (type=9) with the same `orig_src=30, msg_id=4`, they collided.

**Fix applied**: Added `type` field to `seen_msg_t` structure in `routing.c`. Updated `ROUTING__check_msg_seen_readonly()`, `ROUTING__mark_msg_seen()`, and `ROUTING__is_msg_seen()` to include type in comparison. Also updated dedup logic to only mark messages as seen when processing (broadcast or for us), not when forwarding.

**Files modified**: `src/routing.c`

**Status**: Code modified. Needs rebuild and testing without `-vv` flag to fully verify.

### Bug 2: Tunnel sends data before CONN_ACK is received (FIXED)

**Symptom**: In multi-path environments, tunnel data is lost because TUNNEL_DATA arrives at destination before the connection is established.

**Root cause**: The `fwd_thread_func` in `tunnel.c` started reading from client socket and sending TUNNEL_DATA immediately after CONN_OPEN was sent, without waiting for CONN_ACK.

**Fix implemented** (`src/tunnel.c`):
- Added `volatile int ack_received` to `src_conn_t`; zeroed when slot is allocated.
- Added `volatile int *p_ack_received` to `fwd_ctx_t`; set to `&conn->ack_received` for src-side fwd threads, `NULL` for dst-side threads.
- Added module-level `pthread_cond_t g_ack_cond` (shared with `g_tunnel_mutex`).
- `fwd_thread_func`: if `p_ack_received != NULL`, waits up to 10 s on `g_ack_cond` for the flag to be set before entering the read/forward loop.
- `handle_tunnel_conn_ack`: now locks `g_tunnel_mutex`, finds the matching `src_conn_t`, sets `ack_received = 1`, and broadcasts on `g_ack_cond`.
- `handle_tunnel_conn_close`, `TUNNEL__handle_disconnect`, `TUNNEL__destroy`: each broadcasts on `g_ack_cond` so a waiting fwd thread wakes immediately instead of timing out.

### Bug 3: Multi-path duplicate CONN_OPEN handling (FIXED)

**Symptom**: With `--rr-count 2`, CONN_OPEN arrived via both paths, creating duplicate connections.

**Root cause**: `handle_tunnel_conn_open()` didn't check if connection already exists before creating new one.

**Fix**: Added `tunnel_find_conn_by_id()` check before creating connection.

### Bug 4: UDP tunnel return-path broken (FIXED)

**Symptom**: UDP tunnels forwarded data src->dst correctly, but replies from the remote UDP server never made it back to the client. TCP tunnels worked end-to-end.

**Root cause**: The destination-side UDP socket was created but never bound to a local port before the return-path reader (`fwd_thread_func`) started calling `recvfrom()`. The socket only got an ephemeral port later, when `handle_tunnel_data` called the first `sendto()`.

**Fix implemented** (`src/tunnel.c`):
- Removed the `TUNNEL_PROTO_UDP != protocol` guard around `connect()` in `dst_connect_thread`. For UDP, `connect()` sends no packets -- it only installs the remote peer address in the kernel and binds a local ephemeral port up-front.
- Changed the UDP branch in `fwd_thread_func` to use plain `recv()` instead of `recvfrom()`, since the socket is now connected and the peer is fixed.

### Test Setup for Reproducing

```
Node 30: ./ganon 0.0.0.0 -p 11131 -i 30 --reconnect-retries always --rr-count 2 -vv
Node 22: ./ganon 0.0.0.0 -p 11122 -c 127.0.0.1:11131 -i 22 --reconnect-retries always -vv
Node 21: ./ganon 0.0.0.0 -p 11121 -c 127.0.0.1:11131 -i 21 --reconnect-retries always -vv
Node 11: ./ganon 0.0.0.0 -p 11111 -c 127.0.0.1:11121,127.0.0.1:11122 -i 11 --reconnect-retries always --rr-count 2 -vv

Python: c = GanonClient("127.0.0.1", 11111, 3); c.connect()
        t = c.create_tunnel(30, 11, "127.0.0.1", 8000, "127.0.0.1", 9000, "tcp")
        # Then: iperf3 -s -p 9000 & iperf3 -c 127.0.0.1 -p 8000 -t 10
```

### Bug 5: UDP-QUIC tunnel data corruption, decryption failures, and extreme slowness (FIXED)

**Symptom**: The `udp-quic` tunnel experienced frequent "Payload decryption failed" errors, "INVALID SIGNATURE" errors in the C server, and extreme slowness/memory exhaustion (crashing the computer) during high-throughput tests like `iperf3`.

**Root cause 1 (O(N^2) Memory Explosion)**: `quic_write_stream` was re-allocating a stable copy of the *entire remaining message* for every single QUIC packet sent. For large transfers, this led to massive redundant memory usage and astronomical overhead.

**Root cause 2 (Handshake Data Corruption)**: Handshake messages were passed to `ngtcp2` from temporary buffers that were destroyed immediately. If `ngtcp2` needed to retransmit handshake data, it used corrupted memory, leading to incorrect key derivation and subsequent AEAD decryption failures.

**Root cause 3 (Memory Leaks)**: Neither `pending_data` (stream buffers) nor `crypto_data` (handshake buffers) were being freed when the connection was closed.

**Fix implemented**:
- **Optimized Stream Writing**: `quic_write_stream` now allocates a stable copy of the message ONCE and uses slices for `ngtcp2` calls, fixing the O(N^2) slowness.
- **Stable Crypto Storage**: Added `quic_crypto_data_t` linked list to `skin_quic_ctx_t` to store all handshake data until connection teardown.
- **Robust Cleanup**: Updated `quic_free_ctx` to properly free all pending stream and crypto data.
- **Non-blocking I/O**: Transitioned internal pipes to non-blocking mode with an updated I/O thread poll loop to prevent stalls when the application is busy.

**Status**: Verified stable, high-performance end-to-end communication. O(N^2) allocation bug eliminated.

### Bug 6: Outbound QUIC connection segfaults on connect_to_node (FIXED)

**Symptom**: When a node is instructed to connect to another node using the `udp-quic` skin (via `CONNECT_CMD`), it immediately segmentation faults.

**Root cause**: `quic_connect` was calling `ngtcp2_crypto_picotls_configure_client_session` too early, before the `ngtcp2_conn` object was created. This function calls `ngtcp2_conn_encode_local_transport_params`, which dereferences the (null) connection pointer.

**Fix implemented** (`src/skins/skin_udp_quic.c`):
- Removed the redundant and premature call to `ngtcp2_crypto_picotls_configure_client_session` from `quic_connect`.
- The session is correctly configured later in the same function, after the connection object has been successfully initialized.

**Status**: Verified stable outbound connections with Node 11 connecting to Node 21 via QUIC.
