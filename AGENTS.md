# Ganon Project

This is the Ganon project - a C application built with CMake.

## Project Structure

- `src/` - Source files
- `include/` - Header files
- `VERSION` - Version file (e.g., "1.0.0")
- `CMakeLists.txt` - Build configuration
- `Makefile` - Build orchestration
- `cmake/` - Toolchain files for cross-compilation

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

## Testing

- Run the built executable to verify basic functionality

## Error Handling

- Errors defined in `include/err.h` as enum `err_t`
- Naming convention: `E__<MODULE>_<FUNCTION>_<ERROR>`
- Add new errors as needed throughout development