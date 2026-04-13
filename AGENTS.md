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
- `VERSION` - Version file (e.g., "1.0.0")
- `CMakeLists.txt` - Build configuration
- `Makefile` - Build orchestration
- `cmake/` - Toolchain files for cross-compilation
- `ganon_client/` - Python client library

### C Source Files

- `src/main.c` - Main entry point, signal handling
- `src/args.c` - Argument parsing implementation
- `src/logging.c` - Logging implementation
- `src/network.c` - Network socket management, accept loop, client threads
- `src/session.c` - Protocol message handling (NODE_INIT, etc.)
- `src/transport.c` - Socket I/O abstraction layer
- `src/routing.c` - Routing table implementation

### C Header Files

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, FAIL, BREAK_IF, CONTINUE_IF, FREE, VALIDATE_ARGS)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE)
- `include/args.h` - Argument parsing, addr_t struct, args_t config
- `include/network.h` - Network types, socket_entry_t, g_node_id global
- `include/protocol.h` - Protocol message structs (protocol_msg_t, msg_type_t, GANON_PROTOCOL_MAGIC)
- `include/session.h` - Session protocol handling (SESSION__process, SESSION__send_packet)
- `include/transport.h` - Transport layer (transport_t, TRANSPORT__recv_all, TRANSPORT__send_all, TRANSPORT__send_one)
- `include/routing.h` - Routing table (routing_table_t, route_entry_t, route_type_t)

### Python Client Library

- `ganon_client/` - Python package (flat structure, no subdirectories)
- `ganon_client/__init__.py` - Package init, exports GanonClient
- `ganon_client/client.py` - GanonClient class
- `ganon_client/protocol.py` - Protocol structs using construct (ProtocolHeader, ProtocolMessage, MsgType)
- `ganon_client/transport.py` - Transport class wrapping socket recv/send
- `ganon_client/routing.py` - RoutingTable class
- `ganon_client/pyproject.toml` - Build configuration
- `venv/` - Python virtual environment

## Commands

- Build all: `make`
- Build x64 (both): `make x64`
- Build x64 release: `make x64r`
- Build x64 debug: `make x64d`
- Build armv5: `make armv5`
- Build mips32be: `make mips32be`
- Run: `./bin/ganon_1.0.0_x64`
- Clean: `make clean`

## Usage

```
./bin/ganon_1.0.0_x64_debug <LISTEN IP> [OPTIONS]

Arguments:
  LISTEN IP       Listen IP address (0.0.0.0 for any interface)

Options:
  -p, --port N    Listen port number (1-65535, default: 5555)
  -c, --connect   Comma-separated list of IP:port to connect (default port: 5555)
  -i, --node-id N Node ID (0 or greater, mandatory)
  -w, --connect-timeout N  Connect timeout in seconds (default: 5)
  --reconnect-retries N    Reconnect retries on disconnect (default: 5, 0 to disable, max/always for unlimited)
  --reconnect-delay N       Delay between reconnect attempts (default: 5 seconds)
  -h, --help      Show this help message

Environment variables:
  LISTEN_IP       Listen IP address (alternative to positional argument)
  LISTEN_PORT     Listen port number (alternative to -p/--port)
  CONNECT         Comma-separated list of IP:port (alternative to -c/--connect)
  NODE_ID         Node ID (alternative to -i/--node-id)
  CONNECT_TIMEOUT  Connect timeout in seconds (alternative to -w/--connect-timeout)
  RECONNECT_RETRIES   Reconnect retries: 0 to disable, max/always for unlimited (alternative to --reconnect-retries)
  RECONNECT_DELAY     Delay between reconnect attempts (alternative to --reconnect-delay)
  LOG_LEVEL       Log level: info, debug, or trace (alternative to -v flags)
```

### Examples

```bash
# Listen on port 5555 with node ID 1
./bin/ganon 0.0.0.0 -i 1

# Listen on custom port with multiple connection targets
./bin/ganon 0.0.0.0 -p 8080 -i 1 -c "192.168.1.10:5555,10.0.0.5:5555"

# Using environment variables
NODE_ID=1 CONNECT="192.168.1.10:5555,10.0.0.5" ./bin/ganon 0.0.0.0
```

## Build Types

- **Release**: `-O3 -s` (stripped), outputs: `ganon_<ver>_<arch>`. No logging code compiled in.
- **Debug**: `-g -O0 -D__DEBUG__` (with symbols), outputs: `ganon_<ver>_<arch>_debug`. All logging enabled.

## Compiler Flags

All builds use `-Wall -Wextra -Werror` to catch warnings as errors.

## Cross-Compilation

All targets use static linking (`-static` flag):
- x64: Native GCC
- armv5: arm-linux-gnueabihf-gcc (ARMv7 hard-float)
- mips32be: mips-linux-gnu-gcc (MIPS big-endian, o32 ABI)

## Code Style

### C Code Style

- Use C11 standard
- Use meaningful function and variable names
- Maximum line length: 100 characters
- Global variables always start with `g_` (e.g., `g_log_level`, `g_node_id`)

### Python Code Style

- All imports within the ganon_client package must use full package path:
  ```python
  from ganon_client.protocol import GANON_PROTOCOL_MAGIC, MsgType, ProtocolHeader
  from ganon_client.transport import Transport
  ```
  Never use bare imports like `from protocol import ...`.

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
// Check condition and goto cleanup on failure
#define FAIL_IF(condition, error) \
    if (condition) { rc = error; goto l_cleanup; }

// Unconditional failure
#define FAIL(error) \
    { rc = error; goto l_cleanup; }

// Safe free - checks NULL, frees, and sets to NULL
#define FREE(ptr) \
    do { \
        if (NULL != (ptr)) { \
            free((ptr)); \
            (ptr) = NULL; \
        } \
    } while (0)

// Validate multiple pointer arguments
#define VALIDATE_ARGS(...) \
    do { \
        void *args[] = { __VA_ARGS__ }; \
        for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); i++) { \
            if (NULL == args[i]) { \
                rc = E__INVALID_ARG_NULL_POINTER; \
                goto l_cleanup; \
            } \
        } \
    } while (0)
```

Comparison convention: static values first (e.g., `NULL != ptr`, `E__SUCCESS != rc`, `0 > value`)

Preprocessor convention: `#endif` should have a comment indicating which `#ifdef` it closes (e.g., `#endif /* #ifdef __DEBUG__ */`)

## Logging Conventions

Log levels (in order of severity):
- **ERROR** - Catastrophic errors that prevent the program from continuing
- **WARN** - Issues that can be dealt with but indicate potential problems (e.g., disconnections, timeouts)
- **INFO** - Major events in the program (e.g., application startup, shutdown, connections established)
- **DEBUG** - Smaller events or more detail about other events (packet routing, routing table changes)
- **TRACE** - Detailed tracing data (argument parsing details, etc.)

Use appropriate log levels for each message.

## Protocol

### Wire Format

All multi-byte integers use network byte order (big-endian) for cross-architecture compatibility.

#### protocol_msg_t (32 bytes total)

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
|        Message Type (4 bytes)    |  enum msg_type_t
+--------+--------+--------+--------+
|       Data Length (4 bytes)      |
+--------+--------+--------+--------+
|       TTL (4 bytes)              |
+--------+--------+--------+--------+
|       Data (variable)            |
+---------------------------------+
```

#### msg_type_t

```c
typedef enum {
    MSG__NODE_INIT = 0,
    MSG__PEER_INFO = 1,
    MSG__NODE_DISCONNECT = 2,
    MSG__CONNECTION_REJECTED = 3,
} msg_type_t;
```

#### Field Semantics

- **orig_src_node_id**: Original sender of the message (for tracking path)
- **src_node_id**: Previous hop (the node that forwarded this message)
- **dst_node_id**: Destination node (0 for broadcast)
- **ttl**: Time-to-live for broadcast messages (decremented each hop)

#### Protocol Header Size

```c
#define PROTOCOL_HEADER_SIZE 32
#define DEFAULT_TTL 16
```

#### Message Flow

**When node A connects to server S:**
1. A sends NODE_INIT to S (I am node A)
2. S adds direct route to A
3. S broadcasts NODE_INIT to all OTHER connected peers (excluding A) with TTL-1
4. S sends PEER_INFO to A listing all peers reachable through S

**When node B connects to server S:**
1. B sends NODE_INIT to S (I am node B)
2. S adds direct route to B
3. S broadcasts NODE_INIT to all OTHER connected peers (A) with TTL-1
4. A receives broadcast, adds route to B via S
5. S sends PEER_INFO to B listing all peers (including A)
6. B adds route to A via S

**PEER_INFO Propagation (route learning):**
When a node receives PEER_INFO, it extracts the peer list and broadcasts a NEW PEER_INFO to all its OTHER direct peers (excluding the sender). This propagates learned routes through the network.

Example: Node 2 receives PEER_INFO from Node 3 listing Node 4
- Node 2 adds VIA_HOP route to Node 4 via Node 3
- Node 2 broadcasts PEER_INFO (listing Node 4) to all its other peers (Node 1, etc.)
- Node 1 then learns it can reach Node 4 via Node 2

## Routing Table

### Overview

Each node maintains a routing table (`routing_table_t`) that maps node IDs to route entries. Routes can be:
- **DIRECT**: The node is directly connected (we have its socket fd)
- **VIA_HOP**: The node is reachable through another node (we forward to next_hop_node_id)

### Data Structures

```c
typedef enum {
    ROUTE__DIRECT = 0,
    ROUTE__VIA_HOP,
} route_type_t;

typedef struct route_entry {
    uint32_t node_id;
    uint32_t next_hop_node_id;
    route_type_t route_type;
    int fd;  // valid only for DIRECT routes
} route_entry_t;

typedef struct {
    route_entry_t entries[ROUTING_TABLE_MAX_ENTRIES];
    size_t entry_count;
    pthread_mutex_t mutex;
} routing_table_t;
```

### Routing Functions

```c
err_t ROUTING__init(routing_table_t *rt);
void ROUTING__destroy(routing_table_t *rt);
err_t ROUTING__add_direct(routing_table_t *rt, uint32_t node_id, int fd);
err_t ROUTING__add_via_hop(routing_table_t *rt, uint32_t node_id, uint32_t next_hop_node_id);
err_t ROUTING__remove(routing_table_t *rt, uint32_t node_id);
err_t ROUTING__remove_via_node(routing_table_t *rt, uint32_t via_node_id);
err_t ROUTING__get_route(routing_table_t *rt, uint32_t node_id, route_entry_t *entry);
err_t ROUTING__get_next_hop(routing_table_t *rt, uint32_t node_id, uint32_t *next_hop);
int ROUTING__is_direct(routing_table_t *rt, uint32_t node_id);
err_t ROUTING__send_to_node(routing_table_t *rt, uint32_t node_id, const uint8_t *buf, size_t len,
                            ssize_t (*send_fn)(int, const uint8_t *, size_t));
```

### Routing Error Codes

Errors for routing are in range 0x501-0x5FF:
- `E__ROUTING__NODE_NOT_FOUND = 0x501`
- `E__ROUTING__TABLE_FULL`
- `E__ROUTING__INVALID_ARG`

## Architecture

### Layer Separation

The architecture is separated into three layers for future extensibility:

```
+-------------------+     +-------------------+     +-------------------+
|    Application    |     |     Session       |     |    Transport      |
|   (message       | --> |   (protocol       | --> |    (socket        |
|    handlers)      |     |    parsing)       |     |     I/O)          |
+-------------------+     +-------------------+     +-------------------+
```

1. **Transport Layer** (`transport.c/h`): Low-level socket I/O
   - `transport_t` struct with function pointers for `recv` and `send`
   - `TRANSPORT__recv_all()` / `TRANSPORT__send_all()` for guaranteed complete I/O
   - `TRANSPORT__send_one()` for sending a complete message in one go
   - **This is the ONLY layer that calls `send()` and `recv()` syscalls**
   - Can be extended for encrypted/compressed channels

2. **Session Layer** (`session.c/h`): Protocol message handling
   - `SESSION__process()` reads and validates protocol messages
   - `SESSION__send_packet()` - **the ONE function to send a protocol message to a peer**
   - Validates magic, parses header, reads data
   - Dispatches to message handlers based on `msg_type_t`
   - Handles routing table updates on NODE_INIT
   - All sent packets are logged at TRACE level via `log_sent_packet()`
   - Can be extended for additional message types

3. **Application Layer**: Message handlers
   - `SESSION__handle_node_init()` - handles NODE_INIT messages, adds to routing table
   - Future: encryption key exchange, compression, etc.

**Key Design Principle**: The `send()` syscall is ONLY called from within `transport.c`. Any code that needs to send protocol messages must use `SESSION__send_packet()` (for messages originated by this node) or go through transport via `TRANSPORT__send_one()` (for forwarded messages).

### Routing Integration

- `network_t` contains `routing_table_t` member
- `SESSION__process()` takes `routing_table_t*`, `fd`, `peer_node_id*`, `out_header*`, `header_len`, `out_peer_list*`, `out_peer_count*`, `out_data*`, `out_data_len*` parameters
- On NODE_INIT (direct connection, src == orig_src): `ROUTING__add_direct()` is called to add the peer to routing table
- On NODE_INIT (relayed broadcast, src != orig_src): `ROUTING__add_via_hop()` is called with next_hop=src_node_id
- When broadcasting NODE_INIT via `broadcast_to_others()`, src_node_id is set to g_node_id so recipients know the message is relayed
- Server sends NODE_INIT back to newly connected client so it can add the server as a direct route
- Duplicate connection check in `SESSION__handle_node_init`: only applies to direct connections (src == orig_src), not broadcasts
- `prev_peer_node_id` initialized to 0 in `socket_thread_func` (not `entry->peer_node_id`) to correctly detect node reconnections
- On PEER_INFO: learned peers are returned via `out_peer_list*` and `out_peer_count*`
- On MSG__NODE_DISCONNECT: uses orig_src_node_id to identify disconnected node and removes it from routing tables
- On MSG__CONNECTION_REJECTED: connection is abandoned (no reconnect)
- `broadcast_to_others()` broadcasts NODE_INIT to all connected clients except the sender
- `forward_message()` forwards non-protocol messages to the next hop when `dst_node_id` is not local
- `broadcast_peer_info_to_others()` propagates PEER_INFO to all peers except sender
- `broadcast_node_disconnect()` notifies all peers when a node disconnects (sets orig_src=disconnected_node, src=g_node_id)

## Data Structures

### addr_t
IP address and port pair:
```c
typedef struct {
    char *ip;
    int port;
} addr_t;
```

### socket_entry_t
Connected socket tracking:
```c
typedef struct socket_entry {
    int fd;                           // Socket file descriptor
    pthread_t thread;                  // Thread handling this socket
    struct socket_entry *next;         // Next entry in linked list
    network_t *net;                    // Parent network
    char client_ip[INET_ADDRSTRLEN]; // Remote IP
    int client_port;                   // Remote port
    int is_incoming;                  // 1 for incoming, 0 for outgoing
    uint32_t peer_node_id;            // Node ID of the peer (when known)
} socket_entry_t;
```

### args_t
Command line configuration:
```c
typedef struct {
    addr_t listen_addr;               // Listen IP and port
    log_level_t log_level;            // Log level (debug builds only)
    addr_t connect_addrs[64];         // Connection targets
    int connect_count;                 // Number of connection targets
    int node_id;                      // This node's ID (mandatory)
    int connect_timeout;               // Connect timeout in seconds
    int reconnect_retries;             // Reconnect retries (-1 for unlimited)
    int reconnect_delay;              // Delay between reconnect attempts
} args_t;
```

### network_t
Network state:
```c
struct network_t {
    int listen_fd;                    // Listening socket
    pthread_t accept_thread;           // Accept loop thread
    socket_entry_t *clients;          // Connected clients list
    pthread_mutex_t clients_mutex;     // Mutex for clients list
    routing_table_t routing_table;    // Routing table for mesh
    int running;                      // Shutdown flag
    addr_t listen_addr;               // Listen address
    addr_t *connect_addrs;            // Outgoing connection targets
    int connect_count;                 // Number of targets
    pthread_t *connect_threads;        // Outgoing connection threads
    int connect_thread_count;          // Count of connect threads
    int connect_timeout;               // Connect timeout in seconds
    int reconnect_retries;             // Reconnect retries (-1 for unlimited)
    int reconnect_delay;              // Delay between reconnect attempts
};
```

### transport_t
Transport abstraction:
```c
struct transport {
    int fd;
    ssize_t (*recv)(int fd, uint8_t *buf, size_t len);
    ssize_t (*send)(int fd, const uint8_t *buf, size_t len);
};
```

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

### Session Error Codes

- `E__SESSION__HANDLE_NODE_INIT_FAILED = 0x401`
- `E__SESSION__HANDLE_MESSAGE_FAILED`
- `E__SESSION__CONNECTION_REJECTED`

## Network Architecture

### Accept Loop
- Runs in `accept_thread_func`
- Uses `select()` with 1-second timeout for responsive shutdown
- Accepts incoming client connections
- Creates `socket_entry_t` for each client with `is_incoming=1`
- Spawns `socket_thread_func` thread per client

### Client Threads (Socket Handler)
- Each connected socket runs in its own thread (`socket_thread_func`)
- Unified handling for both incoming clients and outgoing connections
- Uses `SESSION__process()` for protocol handling
- Logs connection as "from" for incoming, "to" for outgoing
- Logs disconnection at WARN level
- Removes itself from clients list on disconnect
- On disconnect: calls `broadcast_node_disconnect()`, then `ROUTING__remove()` and `ROUTING__remove_via_node()` with the peer's node_id
- On NODE_INIT: calls `broadcast_to_others()` to announce new node to network

### Duplicate Node ID Detection
- When a node connects and sends NODE_INIT, server checks if that node_id is already connected via `ROUTING__is_direct()`
- If duplicate: server sends `MSG__CONNECTION_REJECTED` and closes socket
- Client receives `MSG__CONNECTION_REJECTED` and abandons connection (no reconnect)
- The rejected socket's `peer_node_id` stays 0, so no disconnect broadcast or routing cleanup is triggered

### Outgoing Connections
- `connect_thread_func` handles each connection target
- Socket set to non-blocking before connect for timeout control
- 5-second timeout for connection attempts
- On success: restores blocking mode, creates socket_entry_t with `is_incoming=0`, spawns socket_thread_func
- On failure: logs warning and continues without it

### Auto-Reconnect
- Only applies to outgoing connections
- On disconnect, retries up to `reconnect_retries` times
- Waits `reconnect_delay` seconds between retries
- Logs each attempt and success/failure
- If all retries fail, gives up and cleans up

### Shutdown Behavior
- SIGINT/SIGTERM signals set shutdown flag
- Closing listen socket stops accept loop
- `shutdown(SHUT_RDWR)` + `close()` on all client sockets wakes blocking threads
- `pthread_detach` for connect threads (self-cleanup)
- `pthread_join` for client threads

## Python Client Development

### Package Structure

The package uses a **flat structure** - all modules are directly under `ganon_client/`:
```
ganon_client/
├── __init__.py      # Exports GanonClient
├── client.py        # GanonClient class
├── protocol.py      # Protocol structs (construct)
├── transport.py     # Transport class
└── routing.py       # RoutingTable class
```

### Import Convention

**All imports must use full package path:**
```python
from ganon_client.protocol import GANON_PROTOCOL_MAGIC, MsgType, ProtocolHeader
from ganon_client.transport import Transport
```

**Never use bare imports:**
```python
# WRONG
from protocol import ...
from transport import ...
```

### Dependencies

Dependencies are specified in `ganon_client/pyproject.toml`:
- `construct>=2.10` - Struct parsing for protocol messages

After any change to `ganon_client/`, reinstall it in the venv:
```bash
/home/arad/projects/ganon/venv/bin/pip install -e /home/arad/projects/ganon/ganon_client
```

### GanonClient Class

```python
from ganon_client import GanonClient

client = GanonClient(
    ip="127.0.0.1",
    port=5555,
    node_id=1,              # This node's ID
    connect_timeout=5,       # Connection attempt timeout (seconds)
    reconnect_retries=5,    # Retries on disconnect (-1 for unlimited)
    reconnect_delay=5,       # Delay between reconnect attempts (seconds)
    log_level=LOG_LEVEL_INFO,
)
```

#### Constructor Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ip` | str | (required) | Server IP address |
| `port` | int | (required) | Server port |
| `node_id` | int | (required) | This node's ID |
| `connect_timeout` | int | 5 | Socket connection timeout (seconds) |
| `reconnect_retries` | int | 5 | Reconnect attempts (-1 = unlimited, 0 = disabled) |
| `reconnect_delay` | int | 5 | Seconds between reconnect attempts |
| `log_level` | int | LOG_LEVEL_DEBUG | Minimum log level to output |

#### Log Levels

```python
LOG_LEVEL_TRACE = 0  # Detailed tracing
LOG_LEVEL_DEBUG = 1  # Debug messages
LOG_LEVEL_INFO = 2   # Informational messages
```

#### Methods

| Method | Description |
|--------|-------------|
| `connect() -> bool` | Connect to server. Returns True on success. |
| `disconnect()` | Disconnect from server gracefully. |
| `reconnect() -> bool` | Force reconnect. Returns True if reconnected. |
| `is_connected() -> bool` | Check if connected. |
| `send(data: bytes) -> int` | Send raw bytes to server. |
| `recv(bufsize: int = 4096) -> bytes` | Receive raw bytes (blocking). |
| `send_to_node(dst_node_id, data) -> bool` | Send protocol message to another node via connected peer. |

#### Context Manager

```python
with GanonClient("127.0.0.1", 5555, node_id=1) as client:
    client.send(b"hello")
    data = client.recv()
```

#### Callbacks

Set callbacks to handle events asynchronously:

```python
client.set_on_data_received(lambda data: print("Received:", data))
client.set_on_disconnected(lambda: print("Disconnected!"))
client.set_on_reconnected(lambda: print("Reconnected!"))
```

| Callback | Signature | Triggered When |
|----------|-----------|---------------|
| `on_data_received` | `(data: bytes) -> None` | Data received from server |
| `on_disconnected` | `() -> None` | Connection lost |
| `on_reconnected` | `() -> None` | Successfully reconnected |

#### Python Architecture

- **Transport**: `Transport` class wraps socket recv/send with `recv_all`/`send_all` helpers
- **Protocol**: `ProtocolHeader` and `ProtocolMessage` construct structs parse the wire format (32 bytes)
- **RoutingTable**: Maps node IDs to route entries (DIRECT or VIA_HOP)
- **Protocol Loop**: `_protocol_loop()` reads and parses protocol messages
- **Process**: `_process()` dispatches to `_handle_node_init()` based on message type
- **Thread Safety**: All public methods use locks for thread-safe access
- **Logging**: Follows ganon C logging conventions (ERROR/WARN always logged, INFO/DEBUG/TRACE conditional)

#### Python Client Internal Flow

**Connection and Data Flow:**
1. `connect()` creates socket with connect_timeout, calls `_connect()`, then removes timeout (`sock.settimeout(None)`)
2. `_start_recv_thread()` spawns `_protocol_loop()` in a daemon thread
3. `_protocol_loop()` uses `Transport.recv_all()` to block until full message is received
4. On disconnect detection, calls `_handle_disconnect()` which triggers `_do_reconnect()`

**Reconnection Flow:**
1. `_handle_disconnect()` is called when `_protocol_loop` detects socket closed/disconnected
2. Sets `_reconnecting = True` to prevent concurrent reconnection attempts
3. `_do_reconnect()` loops with retry logic:
   - On success: assigns new socket, starts new `_protocol_loop` thread
   - On failure: if retries exhausted, sets `_running = False` and calls `_on_disconnected()`

**Socket Timeout Behavior:**
- Timeout is set ONLY during connect phase (`sock.settimeout(connect_timeout)`)
- After successful connect, timeout is removed (`sock.settimeout(None)`)
- This ensures recv operations block indefinitely waiting for data

**Thread Safety:**
- `_running` flag controls if client should stay connected
- `_reconnecting` flag prevents cascade reconnection loops
- All socket operations protected by `_lock` mutex
- `_sock` is set to None after socket is fully closed/shutdown

## TODO

- [x] Implement forwarding of non-direct messages via `ROUTING__send_to_node()`
- [ ] Test multi-node mesh topology with various connection patterns
- [ ] Update Python client with disconnect routing cleanup
- [ ] Build and test with multiple nodes

## Message Forwarding

When a node receives a message with `dst_node_id != 0` and `dst_node_id != g_node_id`:
1. The message is NOT a broadcast or local delivery
2. `forward_message()` is called to route the message to its destination
3. `src_node_id` is updated to the current node (since we're forwarding)
4. `ttl` is decremented
5. `ROUTING__send_to_node()` looks up the route and sends to the next hop

**Implementation:**
- `forward_message()` modifies the header to update `src_node_id` and `ttl`
- `ROUTING__send_to_node()` handles multi-hop routing recursively
- `send_wrapper()` provides the correct function signature for `send()`

**Forwarding Flow:**
1. Receive message with dst_node_id = D, src_node_id = S, orig_src = O
2. Look up route to D - if DIRECT, send directly; if VIA_HOP, forward to next_hop
3. When forwarding: set src_node_id = our_node_id, ttl--
4. The next hop repeats until message reaches D

## PEER_INFO Propagation

When a node receives PEER_INFO, it extracts the peer list and broadcasts a NEW PEER_INFO to all its OTHER direct peers (excluding the sender). This propagates learned routes through the network.

**Implementation:**
- `SESSION__process()` now returns `out_peer_list` and `out_peer_count` for learned peers from PEER_INFO
- `broadcast_peer_info_to_others()` sends PEER_INFO to all connected peers except the sender
- `socket_thread_func` calls `broadcast_peer_info_to_others()` after learning new routes

**Example: Node 1 ↔ Node 2, Node 3 ↔ Node 4, Node 2 connects to Node 3**
1. Node 2→Node 3: NODE_INIT
2. Node 3 broadcasts to Node 4: "Node 2 available via Node 3"
3. Node 3→Node 2: PEER_INFO (listing Node 4)
4. Node 2 adds route to Node 4 via Node 3
5. **Node 2 broadcasts PEER_INFO (listing Node 4) to Node 1**
6. **Node 1 adds route to Node 4 via Node 2**
7. **Node 1 adds route to Node 3 via Node 2**
