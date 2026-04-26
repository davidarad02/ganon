if(NOT TARGET_NAME)
    set(TARGET_NAME "ganon")
endif()

# ── Parse skins_config.h to decide which skins to compile ─────────────────
# This file is the single source of truth — edit it to enable/disable skins.
file(STRINGS "${CMAKE_SOURCE_DIR}/include/skins_config.h" _skins_cfg)

macro(skin_enabled _macro _out_var)
    if(_skins_cfg MATCHES "#define ${_macro}[ \t]*1")
        set(${_out_var} TRUE)
    else()
        set(${_out_var} FALSE)
    endif()
endmacro()

skin_enabled(SKIN_ENABLE_MONOCYPHER SKIN_MONOCYPHER)
skin_enabled(SKIN_ENABLE_PLAIN      SKIN_PLAIN)
skin_enabled(SKIN_ENABLE_XOR        SKIN_XOR)
skin_enabled(SKIN_ENABLE_CHACHA20   SKIN_CHACHA20)
skin_enabled(SKIN_ENABLE_SSH        SKIN_SSH)
skin_enabled(SKIN_ENABLE_QUIC       SKIN_QUIC)

# ── Base sources ───────────────────────────────────────────────────────────

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SOURCES main.c logging.c args.c network.c session.c transport.c routing.c protocol.c loadbalancer.c tunnel.c monocypher.c skin.c)
else()
    set(SOURCES main.c args.c network.c session.c transport.c routing.c protocol.c loadbalancer.c tunnel.c monocypher.c skin.c)
endif()

# ── Conditionally add skin sources ─────────────────────────────────────────

if(SKIN_MONOCYPHER)
    list(APPEND SOURCES skins/skin_tcp_monocypher.c)
    message(STATUS "Skin enabled: tcp-monocypher")
else()
    message(STATUS "Skin disabled: tcp-monocypher")
endif()

if(SKIN_PLAIN)
    list(APPEND SOURCES skins/skin_tcp_plain.c)
    message(STATUS "Skin enabled: tcp-plain")
else()
    message(STATUS "Skin disabled: tcp-plain")
endif()

if(SKIN_XOR)
    list(APPEND SOURCES skins/skin_tcp_xor.c)
    message(STATUS "Skin enabled: tcp-xor")
else()
    message(STATUS "Skin disabled: tcp-xor")
endif()

if(SKIN_CHACHA20)
    list(APPEND SOURCES skins/skin_tcp_chacha20.c)
    message(STATUS "Skin enabled: tcp-chacha20")
else()
    message(STATUS "Skin disabled: tcp-chacha20")
endif()

# (QUIC skin detection moved below)

# ── Select arch-specific libssh/mbedTLS install dirs ──────────────────────

if(NOT CMAKE_CROSSCOMPILING)
    set(LIBSSH_ROOT "${CMAKE_SOURCE_DIR}/third_party/libssh-install")
    set(MBEDTLS_ROOT "${CMAKE_SOURCE_DIR}/third_party/mbedtls-install")
    set(LIBSSH_CRYPTO_BACKEND "openssl")
    set(LIB_SUFFIX "")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "mips")
    set(LIBSSH_ROOT "${CMAKE_SOURCE_DIR}/third_party/libssh-install-mips32be")
    set(MBEDTLS_ROOT "${CMAKE_SOURCE_DIR}/third_party/mbedtls-install-mips32be")
    set(LIBSSH_CRYPTO_BACKEND "mbedtls")
    set(LIB_SUFFIX "-mips32be")
else()
    set(LIBSSH_ROOT "${CMAKE_SOURCE_DIR}/third_party/libssh-install-arm")
    set(MBEDTLS_ROOT "${CMAKE_SOURCE_DIR}/third_party/mbedtls-install-arm")
    set(LIBSSH_CRYPTO_BACKEND "mbedtls")
    set(LIB_SUFFIX "-arm")
endif()

# ── QUIC skin (ngtcp2 + picotls-mbedtls backend, all architectures) ──────

set(NGTCP2_ROOT "${CMAKE_SOURCE_DIR}/third_party/ngtcp2-install")
set(PICOTLS_ROOT "${CMAKE_SOURCE_DIR}/third_party/picotls-install")
set(USE_QUIC_SKIN FALSE)

if(SKIN_QUIC)
    if(EXISTS "${NGTCP2_ROOT}/lib/libngtcp2${LIB_SUFFIX}.a" AND
       EXISTS "${PICOTLS_ROOT}/lib/libpicotls-mbedtls${LIB_SUFFIX}.a")

        list(APPEND SOURCES
            skins/skin_udp_quic.c
            skins/quic_tls.c
            skins/quic_ngtcp2_crypto.c
            "${CMAKE_SOURCE_DIR}/third_party/ngtcp2/crypto/shared.c"
        )
        set(USE_QUIC_SKIN TRUE)
        message(STATUS "Skin enabled: udp-quic (ngtcp2+picotls-mbedtls${LIB_SUFFIX})")

    else()
        message(WARNING "Skin udp-quic enabled but ngtcp2/picotls-mbedtls${LIB_SUFFIX} not built — skipped. Run 'make ngtcp2'.")
        list(APPEND SOURCES skins/skin_udp_quic_stub.c)
    endif()
else()
    message(STATUS "Skin disabled: udp-quic")
    list(APPEND SOURCES skins/skin_udp_quic_stub.c)
endif()

if(SKIN_SSH)
    if(EXISTS "${LIBSSH_ROOT}/lib/libssh.a")
        list(APPEND SOURCES skins/skin_tcp_ssh.c)
        set(USE_LIBSSH_SKIN TRUE)
        message(STATUS "Skin enabled: tcp-ssh (libssh at ${LIBSSH_ROOT}, crypto=${LIBSSH_CRYPTO_BACKEND})")
    else()
        set(USE_LIBSSH_SKIN FALSE)
        message(WARNING "Skin tcp-ssh is enabled in skins_config.h but libssh is not built at ${LIBSSH_ROOT} — skin will be skipped. Run 'make libssh' (or 'make libssh-arm' / 'make libssh-mips32be') to build it.")
    endif()
else()
    set(USE_LIBSSH_SKIN FALSE)
    message(STATUS "Skin disabled: tcp-ssh")
endif()

# ── Target ─────────────────────────────────────────────────────────────────

find_package(Threads REQUIRED)
if(THREADS_HAVE_PTHREAD_ARG)
    set(CMAKE_THREAD_FLAGS -pthread)
endif()

add_executable(${TARGET_NAME} ${SOURCES})
target_link_libraries(${TARGET_NAME} Threads::Threads)
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# ── Optional libsodium (x64 native only) ──────────────────────────────────

set(LIBSODIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/libsodium-install")
if(NOT CMAKE_CROSSCOMPILING AND EXISTS "${LIBSODIUM_ROOT}/lib/libsodium.a")
    target_compile_definitions(${TARGET_NAME} PRIVATE USE_LIBSODIUM)
    target_include_directories(${TARGET_NAME} PRIVATE ${LIBSODIUM_ROOT}/include)
    target_link_libraries(${TARGET_NAME} "${LIBSODIUM_ROOT}/lib/libsodium.a")
endif()

# ── Optional libssh ────────────────────────────────────────────────────────

if(USE_LIBSSH_SKIN)
    target_include_directories(${TARGET_NAME} PRIVATE ${LIBSSH_ROOT}/include)
    target_link_libraries(${TARGET_NAME} "${LIBSSH_ROOT}/lib/libssh.a")

    if(LIBSSH_CRYPTO_BACKEND STREQUAL "openssl")
        find_library(OPENSSL_CRYPTO_STATIC libcrypto.a)
        if(OPENSSL_CRYPTO_STATIC)
            target_link_libraries(${TARGET_NAME} "${OPENSSL_CRYPTO_STATIC}" dl)
        else()
            target_link_libraries(${TARGET_NAME} -lcrypto)
        endif()
    else()
        target_include_directories(${TARGET_NAME} PRIVATE ${MBEDTLS_ROOT}/include)
        target_link_libraries(${TARGET_NAME}
            "${MBEDTLS_ROOT}/lib/libmbedtls.a"
            "${MBEDTLS_ROOT}/lib/libmbedx509.a"
            "${MBEDTLS_ROOT}/lib/libmbedcrypto.a"
        )
    endif()
endif()

# ── Optional ngtcp2 + picotls-mbedtls (any QUIC skin) ─────────────────────

if(USE_QUIC_SKIN)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${NGTCP2_ROOT}/include
        ${PICOTLS_ROOT}/include
        ${MBEDTLS_ROOT}/include
        "${CMAKE_SOURCE_DIR}/third_party/ngtcp2/lib"
        "${CMAKE_SOURCE_DIR}/third_party/ngtcp2/crypto"
    )
    target_link_libraries(${TARGET_NAME}
        "${NGTCP2_ROOT}/lib/libngtcp2${LIB_SUFFIX}.a"
        "${PICOTLS_ROOT}/lib/libpicotls-mbedtls${LIB_SUFFIX}.a"
        "${PICOTLS_ROOT}/lib/libpicotls-core${LIB_SUFFIX}.a"
        "${MBEDTLS_ROOT}/lib/libmbedtls.a"
        "${MBEDTLS_ROOT}/lib/libmbedx509.a"
        "${MBEDTLS_ROOT}/lib/libmbedcrypto.a"
        dl
    )
endif()

if(DEFINED OUTPUT_DIR)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
endif()
