cmake_minimum_required(VERSION 3.16)
project(ganon C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static")
set(CMAKE_EXE_LINKER_FLAGS "-static")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_executable(ganon main.c)