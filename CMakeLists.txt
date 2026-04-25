cmake_minimum_required(VERSION 3.16)
project(ganon C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXE_LINKER_FLAGS "-static")

if(NOT OUTPUT_DIR)
    set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin)
endif()

file(MAKE_DIRECTORY ${OUTPUT_DIR})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0 -D__DEBUG__ -Wall -Wextra -Werror")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -s -Wall -Wextra -Werror")
    if(CMAKE_CROSSCOMPILING)
        # Dead-strip unused code pulled in from static libs (libssh, mbedTLS).
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
    endif()
endif()

add_subdirectory(src)