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
- `src/transport.c` - Buffer layer, recv/send protocol_msg_t, connection abstraction
- `src/protocol.c` - Protocol parsing, serialization, byte order conversion
- `src/routing.c` - Routing table, ROUTING__on_message for broadcast/forward logic, global singleton `g_node_id`
- `src/loadbalancer.c` - Multi-route load balancing strategies (round-robin) and reordering buffer logic

### C Header Files

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, FAIL, BREAK_IF, CONTINUE_IF, FREE, VALIDATE_ARGS, IN, OUT, INOUT)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE)
- `include/args.h` - Argument parsing, addr_t struct, args_t config
- `include/network.h` - Network initialization, `g_network` global singleton, callbacks
- `include/protocol.h` - Protocol message structs, macros, parsing/serialization functions
- `include/session.h` - Session protocol handling (SESSION__on_message, SESSION__on_connected, etc.)
- `include/transport.h` - Transport layer (transport_t, TRANSPORT__recv_msg, TRANSPORT__send_msg, peer metadata)
- `include/routing.h` - Routing table (routing_table_t, route_entry_t, route_type_t), `g_node_id` global singleton
- `include/loadbalancer.h` - Load balancer interface and strategy enums

## Architecture

### Layer Separation

The architecture is separated into four distinct layers:

```
+-------------------+     +-------------------+     +-------------------+     +-------------------+
|      Network      | --> |     Transport     | --> |     Protocol       | --> |     Session       |
| (socket mgmt,    |     |  (buffer layer,   |     | (byte order,      |     | (protocol logic,  |
|  accept/reconnect)|     |   recv/send msg) |     |  validation)       |     |  handle messages) |
+-------------------+     +-------------------+     +-------------------+     +-------------------+

                        +-------------------+
                        |     Routing       |
                        | (broadcast/forward|
                        |  routing table)   |
                        +-------------------+
```

1. **Network Layer** (`network.c/h`): Socket management only
   - Creates listening socket, accepts connections
   - Manages client threads and reconnection logic
   - Owns global `g_network` singleton
   - Does NOT know about node IDs or protocol logic

2. **Transport Layer** (`transport.c/h`): Buffer between logic and network
   - `TRANSPORT__recv_msg()` - receives a complete protocol_msg_t, calls `PROTOCOL__unserialize()`
   - `TRANSPORT__send_msg()` - sends a complete protocol_msg_t, calls `PROTOCOL__serialize()`
   - Plugs into send()/recv() syscalls
   - Owns connection abstraction (fd, node_id, client_ip, port, etc.)

3. **Protocol Layer** (`protocol.c/h`): Byte order and validation
   - `PROTOCOL__unserialize()` - validates magic, converts network byte order to host
   - `PROTOCOL__serialize()` - converts host byte order to network, adds magic
   - Session works with host byte order protocol_msg_t

4. **Session Layer** (`session.c/h`): Protocol-specific logic only
   - Handles all message types (NODE_INIT, MSG__RREQ, MSG__RREP, RERR, CONNECTION_REJECTED)
   - Does NOT handle broadcast/forward logic - this is in routing layer
   - Sends via `TRANSPORT__send_msg()` - never raw sockets

5. **Routing Layer** (`routing.c/h`): Message routing
   - Implements reactive AODV (Ad-hoc On-Demand Distance Vector) algorithm
   - `ROUTING__on_message()` - handles all routing decisions, dynamically updating reverse paths via received message trace data.
   - Transparently broadcast `RREQ` (Route Requests) for unmapped destinations
   - Owns global `g_node_id` and `g_rt` (routing table pointer) singletons
   - `ROUTING__broadcast()` - iterates direct routes, sends to each via transport

### Key Design Principles

- **Network knows nothing about node IDs or protocol** - it just manages sockets
- **Session knows nothing about sockets** - it handles protocol logic only
- **Transport is the ONLY place that calls send()/recv()** - all network I/O goes through transport
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
  -p, --port N    Listen port number (1-65535, default: 5555)
  -c, --connect   Comma-separated list of IP:port to connect (default port: 5555)
  -i, --node-id N Node ID (0 or greater, mandatory)
  -w, --connect-timeout N  Connect timeout in seconds (default: 5)
  --reconnect-retries N    Reconnect retries on disconnect (default: 5, 0 to disable, max/always for unlimited)
  --reconnect-delay N       Delay between reconnect attempts (default: 5 seconds)
  --lb-strategy STR         Load balancing strategy: 'round-robin' (default) or 'all-routes'
  --tcp-rcvbuf N            TCP receive buffer size in bytes for tunnel connections (0 = system default)
  --reorder                Enable packet reordering/buffering (default: disabled, packets processed immediately)
  -h, --help                Show this help message

Commands (via Python client):
  c = GanonClient(ip, port, node_id, ..., reorder=False)  # reorder=False (default) = process packets immediately
  c.connect(ip, port)               Connect local node to peer at ip:port
  c.connect(ip, port, target_node) Instruct target_node to connect to peer at ip:port
  c.disconnect(node_id)             Disconnect local node from node_id
  c.disconnect(node_a, node_b)      Disconnect node_a from node_b
  c.print_network_graph()            Print visual graph of network topology from this node's perspective
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
    set(LOGGING_SOURCES main.c logging.c args.c network.c session.c transport.c routing.c protocol.c loadbalancer.c)
else()
    set(LOGGING_SOURCES main.c args.c network.c session.c transport.c routing.c protocol.c loadbalancer.c)
endif()
```

**IMPORTANT**: When adding new source files:
- If the file uses logging macros (LOG_ERROR, LOG_DEBUG, etc.), add it ONLY to the Debug sources list
- If the file does NOT use logging, add it to BOTH lists
- Never add `logging.c` to Release sources - it will cause link errors since logging.h will be missing

### Makefile Targets

- `make` - Builds all targets (x64r, x64d, armr, armd, mips32ber, mips32bed)
- `make x64` - Builds x64 release and debug
- `make x64r` - Builds x64 release only
- `make x64d` - Builds x64 debug only
- `make arm` - Builds arm release and debug
- `make mips32be` - Builds mips32be release and debug
- `make clean` - Removes all build artifacts

### Binary Output

After build, binaries are in `bin/`:
```
bin/
├── ganon_<ver>_x64_release
├── ganon_<ver>_x64_debug
├── ganon_<ver>_arm_release
├── ganon_<ver>_arm_debug
├── ganon_<ver>_mips32be_release
└── ganon_<ver>_mips32be_debug
```

A convenience symlink `ganon` in project root points to `./bin/ganon_<ver>_x64_debug` for local development.

## Cross-Compilation

All targets use static linking (`-static` flag):
- x64: Native GCC
- armv5: arm-linux-gnueabihf-gcc (ARMv7 hard-float)
- mips32be: mips-linux-gnu-gcc (MIPS big-endian, o32 ABI)

Toolchain files are in `cmake/`:
- `cmake/armv5-toolchain.cmake`
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

#### protocol_msg_t (32 bytes header + data)

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
    MSG__DISCONNECT_RESPONSE = 17
} msg_type_t;
```

#### Field Semantics

- **orig_src_node_id**: Original sender of the message
- **src_node_id**: Previous hop (the node that forwarded this message)
- **dst_node_id**: Destination node (0 for broadcast)
- **ttl**: Time-to-live for broadcast messages (decremented each hop)

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
    char target_ip[64];     /* IP address to connect to */
    uint32_t target_port;   /* Port to connect to */
} connect_cmd_payload_t;
```
Used to dynamically connect a node to a new peer. Sent to the node that should initiate the connection.

**CONNECT_RESPONSE**: data is a `connect_response_payload_t` structure:
```c
typedef struct __attribute__((packed)) {
    uint32_t status;      /* 0=success, 1=refused, 2=timeout, 3=error */
    uint32_t error_code;  /* Implementation-specific error code */
} connect_response_payload_t;
```
Response to CONNECT_CMD indicating success or failure with specific error codes.

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
    int listen_fd;
    pthread_t accept_thread;
    socket_entry_t *clients;
    pthread_mutex_t clients_mutex;
    int running;
    addr_t listen_addr;
    addr_t *connect_addrs;
    int connect_count;
    pthread_t *connect_threads;
    int connect_thread_count;
    int connect_timeout;
    int reconnect_retries;
    int reconnect_delay;
    int node_id;
    network_message_cb_t message_cb;
    network_disconnected_cb_t disconnected_cb;
    network_connected_cb_t connected_cb;
};
```

### transport_t

```c
struct transport {
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    uint32_t node_id;
    void *ctx;
    ssize_t (*recv)(int fd, uint8_t *buf, size_t len);
    ssize_t (*send)(int fd, const uint8_t *buf, size_t len);
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
- [x] Implement multi-path Load Balancing (Round-Robin, All-Routes)
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

## Known Bugs - Multi-Path Tunnel Race Conditions

### Bug 1: Dedup logic collision between RREQ and TUNNEL_CONN_OPEN (PARTIALLY FIXED)

**Symptom**: In multi-path environments (`--rr-count 2`), TUNNEL_CONN_OPEN messages are dropped as duplicates when they're actually new messages.

**Root cause**: The dedup key in `ROUTING__is_msg_seen()` uses only `orig_src + msg_id`, not the message type. When node 30 sends both RREQ (type=2) and CONN_OPEN (type=9) with the same `orig_src=30, msg_id=4`, they collide.

**Why it happens**: 
1. Node 30 sends RREQ for route discovery (msg_id=4, type=2)
2. Node 30 sends CONN_OPEN for tunnel (msg_id=4, type=9)
3. Both have same orig_src and msg_id but different types
4. Node 11 sees RREQ first, marks (30, 4) as seen
5. Node 11 receives CONN_OPEN, incorrectly drops it as duplicate

**Log evidence**:
```
Node 11: RECV msg: orig_src=30, msg_id=4, type=2 (RREQ) - marked as seen
Node 11: RECV msg: orig_src=30, msg_id=4, type=9 (CONN_OPEN) 
Node 11: Dropping unicast duplicate for us from 30 with ID 4 (type 9)
```

**Fix applied**: Added `type` field to `seen_msg_t` structure in `routing.c`. Updated `ROUTING__check_msg_seen_readonly()`, `ROUTING__mark_msg_seen()`, and `ROUTING__is_msg_seen()` to include type in comparison. Also updated dedup logic to only mark messages as seen when processing (broadcast or for us), not when forwarding.

**Files modified**: `src/routing.c`

**Status**: Code modified but NOT fully tested. Needs rebuild and testing without `-vv` flag.

### Bug 2: Tunnel sends data before CONN_ACK is received (FIXED)

**Symptom**: In multi-path environments, tunnel data is lost because TUNNEL_DATA arrives at destination before the connection is established.

**Root cause**: The `fwd_thread_func` in `tunnel.c` starts reading from client socket and sending TUNNEL_DATA immediately after CONN_OPEN is sent, without waiting for CONN_ACK.

**Why it happens**:
1. Client connects to tunnel source (node 30)
2. `handle_client_connection()` sends CONN_OPEN to destination (node 11)
3. `fwd_thread_func()` starts immediately
4. Thread reads data from client and sends TUNNEL_DATA
5. CONN_OPEN and TUNNEL_DATA arrive at node 11 almost simultaneously
6. Node 11 hasn't finished processing CONN_OPEN yet
7. TUNNEL_DATA arrives, connection doesn't exist yet
8. Node 11 drops data: "TUNNEL X conn Y: data for unknown connection, dropping"
9. Node 11 finishes CONN_OPEN processing, connects to destination
10. But data was already lost

**Log evidence**:
```
Node 30: SEND CONN_OPEN (msg_id=4, type=9)
Node 30: SEND TUNNEL_DATA (msg_id=5, type=11)  <- 25 microseconds later!
Node 11: RECV TUNNEL_DATA, "data for unknown connection, dropping"
Node 11: connected to 127.0.0.1:9000  <- too late
```

**Why `-vv` masks it**: Extra logging I/O adds delays, so CONN_ACK/data timing changes enough that it works by luck.

**Fix implemented** (`src/tunnel.c`):
- Added `volatile int ack_received` to `src_conn_t`; zeroed when slot is allocated.
- Added `volatile int *p_ack_received` to `fwd_ctx_t`; set to `&conn->ack_received` for src-side fwd threads, `NULL` for dst-side threads.
- Added module-level `pthread_cond_t g_ack_cond` (shared with `g_tunnel_mutex`).
- `fwd_thread_func`: if `p_ack_received != NULL`, waits up to 10 s on `g_ack_cond` for the flag to be set before entering the read/forward loop. On timeout or early shutdown, tears down the connection cleanly (with `p_slot_conn_id` re-check).
- `handle_tunnel_conn_ack`: now locks `g_tunnel_mutex`, finds the matching `src_conn_t`, sets `ack_received = 1`, and broadcasts on `g_ack_cond`.
- `handle_tunnel_conn_close`, `TUNNEL__handle_disconnect`, `TUNNEL__destroy`: each broadcasts on `g_ack_cond` so a waiting fwd thread wakes immediately instead of timing out.

### Bug 4: UDP tunnel return-path broken (FIXED)

**Symptom**: UDP tunnels forward data src→dst correctly, but replies from the remote UDP server never make it back to the client. TCP tunnels work end-to-end.

**Root cause**: The destination-side UDP socket was created but never bound to a local port before the return-path reader (`fwd_thread_func`) started calling `recvfrom()`. The socket only got an ephemeral port later, when `handle_tunnel_data` called the first `sendto()`. Until that first outbound datagram, the kernel had no port to deliver inbound replies to, so return-path datagrams were dropped.

**Why it happens**:
1. `dst_connect_thread` creates a UDP socket (`socket(AF_INET, SOCK_DGRAM, 0)`)
2. For UDP, the `connect()` call was skipped entirely (it was guarded by `if (TUNNEL_PROTO_UDP != protocol)`)
3. `fwd_thread_func` is spawned and immediately enters `recvfrom()` on a socket with no local port
4. The first `sendto()` from `handle_tunnel_data` auto-binds an ephemeral port, but replies that arrived before this are already lost
5. Even after auto-bind, replies are only delivered if the source address exactly matches; without `connect()`, there is no peer filter

**Fix implemented** (`src/tunnel.c`):
- Removed the `TUNNEL_PROTO_UDP != protocol` guard around `connect()` in `dst_connect_thread` (line ~832). For UDP, `connect()` sends no packets — it only installs the remote peer address in the kernel and binds a local ephemeral port up-front.
- Changed the UDP branch in `fwd_thread_func` to use plain `recv()` instead of `recvfrom()`, since the socket is now connected and the peer is fixed.
- This ensures replies from the remote server are matched to this fd from the moment the thread starts, and ICMP errors are surfaced for debugging.

**Files modified**: `src/tunnel.c`

### Bug 3: Multi-path duplicate CONN_OPEN handling (ALREADY FIXED)

**Symptom**: With `--rr-count 2`, CONN_OPEN arrives via both paths, creating duplicate connections.

**Root cause**: `handle_tunnel_conn_open()` didn't check if connection already exists before creating new one.

**Fix**: Already fixed in previous session - added `tunnel_find_conn_by_id()` check before creating connection.

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

## Transport-Layer Encryption

All TCP connections between ganon nodes are encrypted using **X25519 + XChaCha20-Poly1305** (via vendored Monocypher). Encryption is mandatory and transparent to all layers above transport.

### Cryptography
- **Key exchange:** Ephemeral X25519 per TCP connection
- **Cipher:** XChaCha20-Poly1305 (AEAD, 24-byte nonce, single primitive for encryption + authentication)
- **Key derivation:** BLAKE2b with single-byte context strings (`S`, `R`, `I`)
- **Randomness:** `getrandom()` when available, fallback to `/dev/urandom`
- **No authentication / no PSK:** Protects against passive eavesdropping and active tampering, but not MITM impersonation

### Handshake
After TCP connect, before any ganon protocol traffic:
1. Both sides generate an ephemeral X25519 keypair
2. Initiator sends 32-byte ephemeral public key
3. Responder sends 32-byte ephemeral public key
4. Both compute shared secret and derive directional `send_key` / `recv_key`
5. Session ID derived from shared secret for internal tracking only (never on wire)

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
- `TRANSPORT__do_handshake()` called in `socket_thread_func()` before message loop
- `TRANSPORT__send_msg()` encrypts via `crypto_aead_lock()` into length-prefixed frame
- `TRANSPORT__recv_msg()` reads length-prefixed frame, verifies nonce, decrypts via `crypto_aead_unlock()`, unserializes
- Session, routing, and tunnel layers are completely unaware of encryption

### Integration (Python Client)
- `ganon_client/transport.py` wraps the socket with `monocypher-py` bindings
- `Transport.do_handshake()` performs X25519 exchange and key derivation
- `Transport.send_encrypted()` calls `monocypher.bindings.crypto_lock()`
- `Transport.recv_decrypted()` calls `monocypher.bindings.crypto_unlock()`
- Both C and Python use the same Monocypher primitives, ensuring full compatibility

**Important API difference:** `monocypher-py`'s `crypto_blake2b(msg, key=...)` takes the *message* as the first argument and the *key* as the keyword argument. The C `crypto_blake2b_keyed(hash, hash_size, key, key_size, message, message_size)` takes the *key* as the 3rd argument and the *message* as the 5th. When deriving directional keys in Python, the shared secret must be passed as `key=shared` and the single-byte context (`b"S"`/`b"R"`) as the message.

### Files
- `src/monocypher.c` + `include/monocypher.h` — vendored crypto library (C server)
- `src/transport.c` — handshake, encrypt, decrypt (C server)
- `src/network.c` — handshake trigger in socket thread (C server)
- `include/transport.h` — encryption state in `struct transport`
- `include/err.h` — crypto error codes (`E__CRYPTO__*`)
- `ganon_client/transport.py` — Python transport encryption layer
- `ganon_client/pyproject.toml` — `monocypher-py` dependency

### Future: End-to-End Encryption
The current design is hop-to-hop (protects the link). End-to-end encryption can be added later without changing the frame format: add a second encryption layer inside `session.c` or `routing.c` that encrypts the `data` payload before `TRANSPORT__send_msg` sees it. The hop-to-hop layer will then encrypt the already-encrypted payload, providing double encryption.
