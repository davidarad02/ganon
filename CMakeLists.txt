cmake_minimum_required(VERSION 3.16)
project(ganon C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXE_LINKER_FLAGS "-static")

if(NOT OUTPUT_DIR)
    set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin)
endif()

file(MAKE_DIRECTORY ${OUTPUT_DIR})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -D__DEBUG__")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -s")
endif()

add_subdirectory(src)