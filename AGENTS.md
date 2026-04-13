# Ganon Project

This is the Ganon project - a mesh-style network tunneler in C built with CMake.

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
- `src/session.c` - Protocol message handling (PING, etc.)
- `src/transport.c` - Socket I/O abstraction layer

### C Header Files

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, FAIL, BREAK_IF, CONTINUE_IF, FREE, VALIDATE_ARGS)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE)
- `include/args.h` - Argument parsing, addr_t struct, args_t config
- `include/network.h` - Network types, socket_entry_t, g_node_id global
- `include/protocol.h` - Protocol message structs (protocol_msg_t, msg_type_t, GANON_PROTOCOL_MAGIC)
- `include/session.h` - Session protocol handling (SESSION__process)
- `include/transport.h` - Transport layer (transport_t, TRANSPORT__recv_all, TRANSPORT__send_all)

### Python Client Library

- `ganon_client/` - Python package (flat structure, no subdirectories)
- `ganon_client/__init__.py` - Package init, exports GanonClient
- `ganon_client/client.py` - GanonClient class
- `ganon_client/protocol.py` - Protocol structs using construct (ProtocolHeader, ProtocolMessage, MsgType)
- `ganon_client/transport.py` - Transport class wrapping socket recv/send
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
- **DEBUG** - Smaller events or more detail about other events
- **TRACE** - Detailed tracing data (argument parsing details, etc.)

Use appropriate log levels for each message.

## Protocol

### Wire Format

All multi-byte integers use network byte order (big-endian) for cross-architecture compatibility.

#### protocol_msg_t

```
+--------+--------+--------+--------+
|  Magic (4 bytes)           |  "GNN\0"
+--------+--------+--------+--------+
|        Node ID (4 bytes)         |
+--------+--------+--------+--------+
|       Message ID (4 bytes)       |
+--------+--------+--------+--------+
|        Message Type (4 bytes)    |  enum msg_type_t
+--------+--------+--------+--------+
|       Data Length (4 bytes)      |
+--------+--------+--------+--------+
|       Data (variable)            |
+---------------------------------+
```

#### msg_type_t

```c
typedef enum {
    MSG__PING = 0,
} msg_type_t;
```

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
   - Can be extended for encrypted/compressed channels

2. **Session Layer** (`session.c/h`): Protocol message handling
   - `SESSION__process()` reads and validates protocol messages
   - Validates magic, parses header, reads data
   - Dispatches to message handlers based on `msg_type_t`
   - Can be extended for additional message types

3. **Application Layer**: Message handlers
   - `SESSION__handle_ping()` - handles PING messages
   - Future: encryption key exchange, compression, etc.

### Future Extensions

When adding encryption/compression:
- C: Create new transport variant that wraps the existing transport, injects between session and socket
- Python: Create new `Channel` class wrapping `Transport`

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

### Outgoing Connections
- `connect_thread_func` handles each connection target
- Socket set to non-blocking before connect for timeout control
- 5-second timeout for connection attempts
- On success: creates socket_entry_t with `is_incoming=0`, spawns socket_thread_func
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
├── client.py         # GanonClient class
├── protocol.py       # Protocol structs (construct)
└── transport.py      # Transport class
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
    connect_timeout=5,      # Connection attempt timeout (seconds)
    reconnect_retries=5,     # Retries on disconnect (-1 for unlimited)
    reconnect_delay=5,       # Delay between reconnect attempts (seconds)
    log_level=LOG_LEVEL_INFO,
)
```

#### Constructor Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ip` | str | (required) | Server IP address |
| `port` | int | (required) | Server port |
| `connect_timeout` | int | 5 | Socket connection timeout (seconds) |
| `reconnect_retries` | int | 5 | Reconnect attempts (-1 = unlimited, 0 = disabled) |
| `reconnect_delay` | int | 5 | Seconds between reconnect attempts |
| `log_level` | int | LOG_LEVEL_INFO | Minimum log level to output |

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

#### Context Manager

```python
with GanonClient("127.0.0.1", 5555) as client:
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
|----------|-----------|----------------|
| `on_data_received` | `(data: bytes) -> None` | Data received from server |
| `on_disconnected` | `() -> None` | Connection lost |
| `on_reconnected` | `() -> None` | Successfully reconnected |

#### Python Architecture

- **Transport**: `Transport` class wraps socket recv/send with `recv_all`/`send_all` helpers
- **Protocol**: `ProtocolHeader` and `ProtocolMessage` construct structs parse the wire format
- **Protocol Loop**: `_protocol_loop()` reads and parses protocol messages
- **Process**: `_process()` dispatches to `_handle_ping()` based on message type
- **Thread Safety**: All public methods use locks for thread-safe access
- **Logging**: Follows ganon C logging conventions (ERROR/WARN always logged, INFO/DEBUG/TRACE conditional)
