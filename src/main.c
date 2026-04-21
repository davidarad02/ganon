#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "args.h"
#include "network.h"
#include "routing.h"
#include "session.h"
#include "transport.h"
#include "loadbalancer.h"
#include "tunnel.h"

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

int main(int argc, char *argv[]) {
    err_t rc = E__SUCCESS;
    args_t args;

    rc = ARGS__parse(&args, argc, argv);
    FAIL_IF(E__SUCCESS != rc, rc);

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
    LB__init(args.lb_strategy, args.reorder_timeout, args.rr_count, args.reorder);
    ROUTING__init_globals(SESSION__get_routing_table(SESSION__get_session()), SESSION__on_message);

    rc = NETWORK__init(&g_network, &args, g_node_id, ROUTING__on_message, SESSION__on_disconnected, SESSION__on_connected);
    FAIL_IF(E__SUCCESS != rc, rc);

    LOG_INFO("Network initialized");
    LOG_INFO("Node ID: %d", g_node_id);
    LOG_INFO("Listen: %s:%d", args.listen_addr.ip, args.listen_addr.port);
    if (args.tcp_rcvbuf > 0) {
        LOG_INFO("TCP receive buffer: %d bytes", args.tcp_rcvbuf);
    }
    for (int i = 0; i < args.connect_count; i++) {
        LOG_INFO("Connect[%d]: %s:%d", i, args.connect_addrs[i].ip, args.connect_addrs[i].port);
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
