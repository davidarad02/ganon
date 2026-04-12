# Ganon Project

This is the Ganon project - a C application built with CMake.

## Project Structure

- `main.c` - Entry point
- `CMakeLists.txt` - Build configuration
- `Makefile` - Build orchestration
- `cmake/` - Toolchain files for cross-compilation

## Commands

- Build all: `make`
- Build x64: `make x64`
- Build armv5: `make armv5`
- Build mips32be: `make mips32be`
- Run: `./bin/ganon_x64`
- Clean: `make clean`

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