# Ganon Project

This is the Ganon project - a C application built with CMake.

## Project Structure

- `src/` - Source files
- `include/` - Header files
- `VERSION` - Version file (e.g., "1.0.0")
- `CMakeLists.txt` - Build configuration
- `Makefile` - Build orchestration
- `cmake/` - Toolchain files for cross-compilation

### Headers

- `include/err.h` - Error codes enum
- `include/common.h` - Common macros (FAIL_IF, BREAK_IF, CONTINUE_IF)
- `include/logging.h` - Logging macros (LOG_ERROR, LOG_WARNING, LOG_INFO, LOG_DEBUG, LOG_TRACE)

## Commands

- Build all: `make`
- Build x64 (both): `make x64`
- Build x64 release: `make x64r`
- Build x64 debug: `make x64d`
- Build armv5: `make armv5`
- Build mips32be: `make mips32be`
- Run: `./bin/ganon_1.0.0_x64`
- Clean: `make clean`

## Build Types

- **Release**: `-O3 -s` (stripped), outputs: `ganon_<ver>_<arch>`
- **Debug**: `-g -O0 -D__DEBUG__` (with symbols), outputs: `ganon_<ver>_<arch>_debug`

## Cross-Compilation

CMake accepts these variables:
- `TARGET_NAME` - Output binary name
- `TOOLCHAIN_FILE` - Path to toolchain cmake file (optional)

All targets use static linking (`-static` flag):
- x64: Native GCC
- armv5: arm-linux-gnueabihf-gcc (ARMv7 hard-float)
- mips32be: mips-linux-gnu-gcc (MIPS big-endian, o32 ABI)

## Code Style

- Use C11 standard (already set in CMakeLists.txt)
- Use meaningful function and variable names
- Maximum line length: 100 characters
- Global variables always start with `g_` (e.g., `g_verbose`, `g_num_connections`)

## Function Conventions

Every function (except `main`) must:
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

## Logging Conventions

Log levels (in order of severity):

- **ERROR** - Catastrophic errors that prevent the program from continuing
- **WARN** - Issues that can be dealt with but indicate potential problems
- **INFO** - Major events in the program (e.g., application startup, shutdown, significant state changes)
- **DEBUG** - Smaller events or more detail about other events
- **TRACE** - Detailed tracing data (argument parsing details, etc.)

Use appropriate log levels for each message:
- Most internal processing (like argument parsing in normal flow) should use TRACE
- User-facing messages or significant milestones should use INFO
- Recoverable issues should use WARN
- Fatal errors should use ERROR

- Errors defined in `include/err.h` as enum `err_t`
- First error must be `E__SUCCESS = 0`
- Naming convention: `E__<MODULE>_<FUNCTION>_<ERROR>`
- Add new errors as needed throughout development