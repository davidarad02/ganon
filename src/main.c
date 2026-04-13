#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "args.h"
#include "network.h"

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

int main(int argc, char *argv[]) {
    err_t rc = E__SUCCESS;
    args_t args;
    network_t network;

    rc = args_parse(&args, argc, argv);
    FAIL_IF(E__SUCCESS != rc, rc);

    g_node_id = args.node_id;

    LOG_INFO("Starting ganon...");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rc = network_init(&network, &args);
    FAIL_IF(E__SUCCESS != rc, rc);

    LOG_INFO("Network initialized");
    LOG_INFO("Node ID: %d", g_node_id);
    LOG_INFO("Listen: %s:%d", args.listen_addr.ip, args.listen_addr.port);
    for (int i = 0; i < args.connect_count; i++) {
        LOG_INFO("Connect[%d]: %s:%d", i, args.connect_addrs[i].ip, args.connect_addrs[i].port);
    }

    while (!g_shutdown_requested) {
        sleep(1);
    }

    LOG_INFO("Shutdown requested, stopping network...");
    rc = network_shutdown(&network);
    FAIL_IF(E__SUCCESS != rc, rc);

    LOG_INFO("ganon stopped");

l_cleanup:
    return (int)rc;
}
