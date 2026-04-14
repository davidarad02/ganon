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
   - Handles all message types (NODE_INIT, PEER_INFO, NODE_DISCONNECT, CONNECTION_REJECTED)
   - Does NOT handle broadcast/forward logic - this is in routing layer
   - Sends via `TRANSPORT__send_msg()` - never raw sockets

5. **Routing Layer** (`routing.c/h`): Message routing
   - `ROUTING__on_message()` - handles all routing decisions
   - If dst == this node: calls session callback
   - If dst == 0 (broadcast): calls session callback AND broadcasts to all direct peers except src
   - If dst != this node and != 0: forwards to next hop
   - Drops messages with TTL == 0
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
  -h, --help      Show this help message
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
    set(LOGGING_SOURCES main.c logging.c args.c network.c session.c transport.c routing.c protocol.c)
else()
    set(LOGGING_SOURCES main.c args.c network.c session.c transport.c routing.c protocol.c)
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
    MSG__PEER_INFO = 1,
    MSG__NODE_DISCONNECT = 2,
    MSG__CONNECTION_REJECTED = 3,
} msg_type_t;
```

#### Field Semantics

- **orig_src_node_id**: Original sender of the message
- **src_node_id**: Previous hop (the node that forwarded this message)
- **dst_node_id**: Destination node (0 for broadcast)
- **ttl**: Time-to-live for broadcast messages (decremented each hop)

#### Message Data

**NODE_DISCONNECT**: data contains list of nodes that were reachable via the disconnected node (unreachable nodes due to this disconnect). Each node ID is a 4-byte network-order integer.

## Data Structures

### route_entry_t

```c
typedef struct route_entry {
    uint32_t node_id;
    uint32_t next_hop_node_id;
    route_type_t route_type;
    int fd;
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
- [ ] Update Python client to match new architecture
- [ ] Test multi-node mesh topology