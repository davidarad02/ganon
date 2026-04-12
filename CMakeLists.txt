cmake_minimum_required(VERSION 3.16)
project(ganon C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static")
set(CMAKE_EXE_LINKER_FLAGS "-static")

add_executable(ganon main.c)

add_custom_target(x64 COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Release -B build-x64 -S ${CMAKE_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} --build build-x64)

add_custom_target(armv5 COMMAND ${CMAKE_COMMAND} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/armv5-toolchain.cmake
    -DCMAKE_BUILD_TYPE=Release -B build-armv5 -S ${CMAKE_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} --build build-armv5)

add_custom_target(mips32be COMMAND ${CMAKE_COMMAND} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/mips32be-toolchain.cmake
    -DCMAKE_BUILD_TYPE=Release -B build-mips32be -S ${CMAKE_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} --build build-mips32be)