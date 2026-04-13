#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "args.h"
#include "network.h"
#include "session.h"
#include "transport.h"

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

static network_t g_network;
static session_t g_session;

static void on_connected(void *ctx, transport_t *t) {
    (void)ctx;
    SESSION__on_connected(&g_session, t);
}

static void on_message(void *ctx, transport_t *t, const uint8_t *buf, size_t len) {
    (void)ctx;
    SESSION__on_message(&g_session, t, buf, len);
}

static void on_disconnected(void *ctx, transport_t *t) {
    (void)ctx;
    SESSION__on_disconnected(&g_session, TRANSPORT__get_node_id(t));
}

static void session_send_wrapper(uint32_t node_id, const uint8_t *buf, size_t len, void *ctx) {
    (void)ctx;
    NETWORK__send_to(&g_network, node_id, buf, len);
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

    rc = SESSION__init(&g_session, g_node_id);
    FAIL_IF(E__SUCCESS != rc, rc);

    SESSION__set_network(&g_session, &g_network);
    NETWORK__set_send_fn(&g_network, session_send_wrapper, &g_session);

    rc = NETWORK__init(&g_network, &args, g_node_id, on_message, on_disconnected, on_connected, &g_session);
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
    rc = NETWORK__shutdown(&g_network);
    FAIL_IF(E__SUCCESS != rc, rc);

    SESSION__destroy(&g_session);

    LOG_INFO("ganon stopped");

l_cleanup:
    return (int)rc;
}
