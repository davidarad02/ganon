# Ganon Project

This is the Ganon project - a C application built with CMake.

## Project Structure

- `main.c` - Entry point
- `CMakeLists.txt` - Build configuration

## Commands

- Build: `cmake -B build && cmake --build build`
- Run: `./build/ganon`
- Clean: `rm -rf build`

## Code Style

- Use C11 standard (already set in CMakeLists.txt)
- Use meaningful function and variable names
- Maximum line length: 100 characters

## Testing

- Run the built executable to verify basic functionality