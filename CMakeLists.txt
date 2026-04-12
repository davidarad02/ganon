cmake_minimum_required(VERSION 3.16)
project(ganon C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static -I${CMAKE_SOURCE_DIR}/include")
set(CMAKE_EXE_LINKER_FLAGS "-static")

if(NOT TARGET_NAME)
    set(TARGET_NAME "ganon")
endif()

if(NOT TOOLCHAIN_FILE)
    set(TOOLCHAIN_FILE "")
endif()

if(TOOLCHAIN_FILE)
    include(${TOOLCHAIN_FILE})
endif()

if(NOT OUTPUT_DIR)
    set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin)
endif()

file(MAKE_DIRECTORY ${OUTPUT_DIR})

add_executable(${TARGET_NAME} src/main.c)
set_target_properties(${TARGET_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}
    OUTPUT_NAME "${TARGET_NAME}")