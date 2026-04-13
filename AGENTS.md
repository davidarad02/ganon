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
- `src/network.c` - Socket management, accept loop, reconnection logic
- `src/session.c` - All protocol logic, routing table, broadcasts
- `src/transport.c` - Buffer layer, recv/send protocol_msg_t, connection abstraction
- `src/protocol.c` - Protocol parsing, serialization, byte order conversion
- `src/routing.c` - Routing table implementation

### C Header Files

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, FAIL, BREAK_IF, CONTINUE_IF, FREE, VALIDATE_ARGS)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE)
- `include/args.h` - Argument parsing, addr_t struct, args_t config
- `include/network.h` - Network initialization, callbacks, send functions
- `include/protocol.h` - Protocol message structs, macros, parsing/serialization functions
- `include/session.h` - Session protocol handling (SESSION__on_message, SESSION__on_connected, etc.)
- `include/transport.h` - Transport layer (transport_t, TRANSPORT__recv_msg, TRANSPORT__send_msg, peer metadata)
- `include/routing.h` - Routing table (routing_table_t, route_entry_t, route_type_t)

## Architecture

### Layer Separation

The architecture is separated into four distinct layers:

```
+-------------------+     +-------------------+     +-------------------+     +-------------------+
|      Network      | --> |     Transport     | --> |     Protocol       | --> |     Session       |
| (socket mgmt,    |     |  (buffer layer,   |     | (byte order,      |     | (protocol logic,  |
|  reconnection)    |     |   recv/send msg) |     |  validation)       |     |  routing table)   |
+-------------------+     +-------------------+     +-------------------+     +-------------------+
```

1. **Network Layer** (`network.c/h`): Socket management only
   - Creates listening socket, accepts connections
   - Manages client threads and reconnection logic
   - Does NOT know about node IDs or protocol logic

 2. **Transport Layer** (`transport.c/h`): Buffer between logic and network
   - `TRANSPORT__recv_msg()` - receives a complete protocol_msg_t, calls `PROTOCOL__unserialize()`
   - `TRANSPORT__send_msg()` - sends a complete protocol_msg_t, calls `PROTOCOL__serialize()`
   - `TRANSPORT__send_to_node_id()` - looks up transport by node_id and sends
   - Plugs into send()/recv() syscalls
   - Owns connection abstraction (fd, node_id, client_ip, port, etc.)

3. **Protocol Layer** (`protocol.c/h`): Byte order and validation
   - `PROTOCOL__unserialize()` - validates magic, converts network byte order to host
   - `PROTOCOL__serialize()` - converts host byte order to network, adds magic
   - Session works with host byte order protocol_msg_t

4. **Session Layer** (`session.c/h`): All protocol logic
   - Handles all message types (NODE_INIT, PEER_INFO, NODE_DISCONNECT, etc.)
   - Maintains routing table
   - Implements broadcast propagation
   - Sends ONLY via `TRANSPORT__send_to_node_id()` - never raw sockets

### Key Design Principles

- **Network knows nothing about node IDs or protocol** - it just manages sockets
- **Session knows nothing about sockets** - it handles protocol logic only
- **Transport is the ONLY place that calls send()/recv()** - all network I/O goes through transport
- **All byte order conversion happens in protocol.c** - serialize/unserialize
- **Session sends messages only via TRANSPORT__send_to_node_id()** - never raw

### Interface

**main.c wires everything together:**
```c
SESSION__init(SESSION__get_session(), node_id);
SESSION__set_network(SESSION__get_session(), &net);

NETWORK__init(&net, &args, node_id, SESSION__on_message, SESSION__on_disconnected, SESSION__on_connected);
```
```

## Usage

```
./bin/ganon <LISTEN IP> [OPTIONS]

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
- Global variables always start with `g_` (e.g., `g_log_level`, `g_node_id`)

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

## Data Structures

### connection_t

```c
struct connection {
    uint32_t node_id;
    int fd;
    int is_incoming;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    void *ctx;
};
```

### session_t

```c
struct session_t {
    int node_id;
    routing_table_t routing_table;
    network_t *net;
};
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
    void *session_ctx;
    network_message_cb_t message_cb;
    network_disconnected_cb_t disconnected_cb;
    network_connected_cb_t connected_cb;
    network_send_fn_t send_fn;
    void *send_ctx;
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

## Network Callbacks

```c
typedef void (*network_message_cb_t)(void *session_ctx, transport_t *t, const uint8_t *buf, size_t len);
typedef void (*network_disconnected_cb_t)(void *session_ctx, transport_t *t);
typedef void (*network_connected_cb_t)(void *session_ctx, transport_t *t);
typedef void (*network_send_fn_t)(uint32_t node_id, const uint8_t *buf, size_t len, void *ctx);
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

## TODO

- [x] Major refactor: separate network/session/transport layers
- [ ] Update Python client to match new architecture
- [ ] Test multi-node mesh topology
