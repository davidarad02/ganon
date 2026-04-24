if(NOT TARGET_NAME)
    set(TARGET_NAME "ganon")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LOGGING_SOURCES main.c logging.c args.c network.c network_caps.c network_epoll.c session.c transport.c routing.c protocol.c loadbalancer.c tunnel.c monocypher.c)
else()
    set(LOGGING_SOURCES main.c args.c network.c network_caps.c network_epoll.c session.c transport.c routing.c protocol.c loadbalancer.c tunnel.c monocypher.c)
endif()

find_package(Threads REQUIRED)
if(THREADS_HAVE_PTHREAD_ARG)
    set(CMAKE_THREAD_FLAGS -pthread)
endif()

add_executable(${TARGET_NAME} ${LOGGING_SOURCES})
target_link_libraries(${TARGET_NAME} Threads::Threads)
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)

include(CheckSymbolExists)
check_symbol_exists(epoll_create "sys/epoll.h" HAVE_EPOLL)
if(HAVE_EPOLL)
    target_compile_definitions(${TARGET_NAME} PRIVATE USE_EPOLL)
endif()

# Optional libsodium for high-performance XChaCha20-Poly1305 on x64
set(LIBSODIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/libsodium-install")
if(EXISTS "${LIBSODIUM_ROOT}/lib/libsodium.a")
    target_compile_definitions(${TARGET_NAME} PRIVATE USE_LIBSODIUM)
    target_include_directories(${TARGET_NAME} PRIVATE ${LIBSODIUM_ROOT}/include)
    target_link_libraries(${TARGET_NAME} "${LIBSODIUM_ROOT}/lib/libsodium.a")
endif()

if(DEFINED OUTPUT_DIR)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
endif()