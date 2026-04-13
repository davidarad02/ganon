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

### Python Client Library

- `ganon_client/` - Python package
- `ganon_client/__init__.py` - Package init, exports GanonClient
- `ganon_client/client.py` - GanonClient class
- `ganon_client/pyproject.toml` - Build configuration
- `venv/` - Python virtual environment

### Headers

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, BREAK_IF, CONTINUE_IF)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE)
- `include/args.h` - Argument parsing, addr_t struct, args_t config
- `include/network.h` - Network types, socket_entry_t, g_node_id global

### Source Files

- `src/main.c` - Main entry point, signal handling
- `src/args.c` - Argument parsing implementation
- `src/logging.c` - Logging implementation
- `src/network.c` - Network socket management, accept loop, client threads

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

## Cross-Compilation

All targets use static linking (`-static` flag):
- x64: Native GCC
- armv5: arm-linux-gnueabihf-gcc (ARMv7 hard-float)
- mips32be: mips-linux-gnu-gcc (MIPS big-endian, o32 ABI)

## Code Style

- Use C11 standard
- Use meaningful function and variable names
- Maximum line length: 100 characters
- Global variables always start with `g_` (e.g., `g_log_level`, `g_node_id`)

## Function Conventions

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
    int connect_count;                // Number of connection targets
    int node_id;                     // This node's ID (mandatory)
    int connect_timeout;              // Connect timeout in seconds
    int reconnect_retries;            // Reconnect retries (-1 for unlimited)
    int reconnect_delay;             // Delay between reconnect attempts
} args_t;
```

### network_t
Network state:
```c
struct network_t {
    int listen_fd;                    // Listening socket
    pthread_t accept_thread;           // Accept loop thread
    socket_entry_t *clients;          // Connected clients list
    pthread_mutex_t clients_mutex;    // Mutex for clients list
    int running;                      // Shutdown flag
    addr_t listen_addr;              // Listen address
    addr_t *connect_addrs;           // Outgoing connection targets
    int connect_count;                // Number of targets
    pthread_t *connect_threads;       // Outgoing connection threads
    int connect_thread_count;         // Count of connect threads
    int connect_timeout;              // Connect timeout in seconds
    int reconnect_retries;            // Reconnect retries (-1 for unlimited)
    int reconnect_delay;             // Delay between reconnect attempts
};
```

## Error Codes

Errors are defined in `include/err.h` as enum `err_t`:
- First error must be `E__SUCCESS = 0`
- Naming convention: `E__<MODULE>_<FUNCTION>_<ERROR>`
- Hex ranges: Generic=0x001-0x0FF, args=0x200-0x2FF, network=0x300-0x3FF

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
- Echo functionality: received data is sent back on the same socket
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
- On disconnect, retries up to `NETWORK_RETRY_COUNT` (5) times
- Waits `NETWORK_RETRY_DELAY_SEC` (5) seconds between retries
- Logs each attempt and success/failure
- If all retries fail, gives up and cleans up

### Shutdown Behavior
- SIGINT/SIGTERM signals set shutdown flag
- Closing listen socket stops accept loop
- `shutdown(SHUT_RDWR)` + `close()` on all client sockets wakes blocking threads
- `pthread_detach` for connect threads (self-cleanup)
- `pthread_join` for client threads

## Python Client Development

Dependencies are specified in `ganon_client/pyproject.toml`:
- `construct>=2.10` - Struct parsing for protocol messages

After any change to `ganon_client/`, reinstall it in the venv:
```bash
/home/arad/projects/ganon/venv/bin/pip install -e /home/arad/projects/ganon/ganon_client
```

Test the client:
```bash
/home/arad/projects/ganon/venv/bin/ipython
from ganon_client import GanonClient
```

### GanonClient Class

```python
from ganon_client import GanonClient

client = GanonClient(
    ip="127.0.0.1",
    port=5555,
    connect_timeout=5,      # Connection attempt timeout (seconds)
    reconnect_retries=5,    # Retries on disconnect (-1 for unlimited)
    reconnect_delay=5,      # Delay between reconnect attempts (seconds)
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

#### Architecture

- **Receive Thread**: Background thread monitors socket for incoming data and disconnection
- **Auto-Reconnect**: On unexpected disconnect, automatically attempts reconnection
- **Thread Safety**: All public methods use locks for thread-safe access
- **Logging**: Follows ganon C logging conventions (ERROR/WARN always logged, INFO/DEBUG/TRACE conditional)
