#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_LIBSODIUM
#include <sodium.h>
#endif

#include "common.h"
#include "logging.h"
#include "args.h"
#include "network.h"
#include "routing.h"
#include "session.h"
#include "transport.h"
#include "loadbalancer.h"
#include "tunnel.h"
#include "skins_config.h"
#if SKIN_ENABLE_MONOCYPHER
#include "skins/skin_tcp_monocypher.h"
#endif
#if SKIN_ENABLE_PLAIN
#include "skins/skin_tcp_plain.h"
#endif
#if SKIN_ENABLE_XOR
#include "skins/skin_tcp_xor.h"
#endif
#if SKIN_ENABLE_CHACHA20
#include "skins/skin_tcp_chacha20.h"
#endif
#if SKIN_ENABLE_SSH
#include "skins/skin_tcp_ssh.h"
#endif
#if SKIN_ENABLE_QUIC
    #include "skins/skin_udp_quic.h"
#endif
#if SKIN_ENABLE_QUIC2
    #include "skins/skin_udp_quic2.h"
#endif

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

int main(int argc, char *argv[]) {
    err_t rc = E__SUCCESS;
    args_t args;

#if SKIN_ENABLE_MONOCYPHER
    SKIN_TCPM__register();
#endif
#if SKIN_ENABLE_PLAIN
    SKIN_TCP_PLAIN__register();
#endif
#if SKIN_ENABLE_XOR
    SKIN_TCP_XOR__register();
#endif
#if SKIN_ENABLE_CHACHA20
    SKIN_TCP_CHACHA20__register();
#endif
#if SKIN_ENABLE_SSH
    SKIN_TCP_SSH__register();
#endif
#if SKIN_ENABLE_QUIC
    SKIN_UDP_QUIC__register();
#endif
#if SKIN_ENABLE_QUIC2
    SKIN_UDP_QUIC2__register();
#endif

    rc = ARGS__parse(&args, argc, argv);
    FAIL_IF(E__SUCCESS != rc, rc);

#ifdef USE_LIBSODIUM
    if (sodium_init() < 0) {
        LOG_ERROR("libsodium initialization failed");
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }
#endif

    g_node_id = args.node_id;

    LOG_INFO("Starting ganon...");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE so that writes to closed sockets return EPIPE instead of killing the process */
    signal(SIGPIPE, SIG_IGN);

    rc = SESSION__init(SESSION__get_session(), g_node_id);
    FAIL_IF(E__SUCCESS != rc, rc);

    SESSION__set_network(SESSION__get_session(), &g_network);

    TUNNEL__init(args.tcp_rcvbuf);
    SESSION__set_file_chunk_size(args.file_chunk_size);
    LB__init(args.lb_strategy, args.reorder_timeout, args.rr_count, args.reorder);
    ROUTING__init_globals(SESSION__get_routing_table(SESSION__get_session()), SESSION__on_message);

    rc = NETWORK__init(&g_network, &args, g_node_id, ROUTING__on_message, SESSION__on_disconnected, SESSION__on_connected);
    FAIL_IF(E__SUCCESS != rc, rc);

    LOG_INFO("Network initialized");
    LOG_INFO("Node ID: %d", g_node_id);
    for (int i = 0; i < args.listener_count; i++) {
        LOG_INFO("Listen[%d]: %s:%d (skin_id=%u)", i,
                 args.listeners[i].addr.ip, args.listeners[i].addr.port,
                 args.listeners[i].skin_id);
    }
    if (args.tcp_rcvbuf > 0) {
        LOG_INFO("TCP receive buffer: %d bytes", args.tcp_rcvbuf);
    }
    if (args.file_chunk_size > 0) {
        LOG_INFO("File chunk size: %d bytes", args.file_chunk_size);
    }
    for (int i = 0; i < args.connect_count; i++) {
        LOG_INFO("Connect[%d]: %s:%d (skin_id=%u)", i,
                 args.connect_addrs[i].addr.ip, args.connect_addrs[i].addr.port,
                 args.connect_addrs[i].skin_id);
    }

    while (!g_shutdown_requested) {
        sleep(1);
    }

    LOG_INFO("Shutdown requested, stopping network...");
    rc = NETWORK__shutdown(&g_network);
    FAIL_IF(E__SUCCESS != rc, rc);

    SESSION__destroy(SESSION__get_session());
    LB__destroy();
    TUNNEL__destroy();

    LOG_INFO("ganon stopped");

 l_cleanup:
    return (int)rc;
}
