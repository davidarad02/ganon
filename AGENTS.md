# Ganon Project

This is the Ganon project - a C application built with CMake.

## Project Structure

- `main.c` - Entry point
- `CMakeLists.txt` - Build configuration

## Commands

- Build x64: `cmake -B build-x64 && cmake --build build-x64 && cp build-x64/ganon bin/ganon_x64`
- Build armv5: `cmake -B build-armv5 -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-toolchain.cmake && cmake --build build-armv5 && cp build-armv5/ganon bin/ganon_armv5`
- Build mips32be: `cmake -B build-mips32be -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-toolchain.cmake && cmake --build build-mips32be && cp build-mips32be/ganon bin/ganon_mips32be`
- Run: `./bin/ganon_x64`
- Clean: `rm -rf build-x64 build-armv5 build-mips32be bin/ganon_*`

## Cross-Compilation

- All targets use static linking (`-static` flag)
- x64: Native GCC
- armv5: arm-linux-gnueabihf-gcc (ARMv7 hard-float)
- mips32be: mips-linux-gnu-gcc (MIPS big-endian, o32 ABI)

## Code Style

- Use C11 standard (already set in CMakeLists.txt)
- Use meaningful function and variable names
- Maximum line length: 100 characters

## Testing

- Run the built executable to verify basic functionality