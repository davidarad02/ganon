#ifndef GANON_ARGS_H
#define GANON_ARGS_H

#include <stddef.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "loadbalancer.h"

#define ARGS_ENV_LISTEN_IP "LISTEN_IP"
#define ARGS_ENV_LISTEN_PORT "LISTEN_PORT"
#define ARGS_ENV_CONNECT "CONNECT"
#define ARGS_ENV_NODE_ID "NODE_ID"
#define ARGS_ENV_CONNECT_TIMEOUT "CONNECT_TIMEOUT"
#define ARGS_ENV_RECONNECT_RETRIES "RECONNECT_RETRIES"
#define ARGS_ENV_RECONNECT_DELAY "RECONNECT_DELAY"
#define ARGS_ENV_LB_STRATEGY "LB_STRATEGY"
#define ARGS_ENV_REORDER_TIMEOUT "REORDER_TIMEOUT"
#define ARGS_ENV_RR_COUNT "RR_COUNT"
#define ARGS_ENV_TCP_RCVBUF "TCP_RCVBUF"
#define ARGS_FLAG_PORT_SHORT "-p"
#define ARGS_FLAG_PORT_LONG "--port"
#define ARGS_FLAG_CONNECT_SHORT "-c"
#define ARGS_FLAG_CONNECT_LONG "--connect"
#define ARGS_FLAG_NODE_ID_SHORT "-i"
#define ARGS_FLAG_NODE_ID_LONG "--node-id"
#define ARGS_FLAG_CONNECT_TIMEOUT_SHORT "-w"
#define ARGS_FLAG_CONNECT_TIMEOUT_LONG "--connect-timeout"
#define ARGS_FLAG_RECONNECT_RETRIES_LONG "--reconnect-retries"
#define ARGS_FLAG_RECONNECT_DELAY_LONG "--reconnect-delay"
#define ARGS_FLAG_HELP_SHORT "-h"
#define ARGS_FLAG_HELP_LONG "--help"
#define ARGS_FLAG_LB_STRATEGY_LONG "--lb-strategy"
#define ARGS_FLAG_REORDER_TIMEOUT_LONG "--reorder-timeout"
#define ARGS_FLAG_RR_COUNT_LONG "--rr-count"
#define ARGS_FLAG_TCP_RCVBUF_LONG "--tcp-rcvbuf"

#define ARGS_PORT_DEFAULT 5555
#define ARGS_CONNECT_DEFAULT_PORT 5555
#define ARGS_MAX_CONNECT_ENTRIES 64
#define ARGS_CONNECT_TIMEOUT_DEFAULT 5
#define ARGS_RECONNECT_RETRIES_DEFAULT 5
#define ARGS_RECONNECT_DELAY_DEFAULT 5
#define ARGS_REORDER_TIMEOUT_DEFAULT 100
#define ARGS_RR_COUNT_DEFAULT 1
#define ARGS_TCP_RCVBUF_DEFAULT 0  /* 0 = use system default */

typedef struct {
    char *ip;
    int port;
} addr_t;

typedef struct {
    addr_t listen_addr;
    log_level_t log_level;
    addr_t connect_addrs[ARGS_MAX_CONNECT_ENTRIES];
    int connect_count;
    int node_id;
    int connect_timeout;
    int reconnect_retries;
    int reconnect_delay;
    int reorder_timeout;
    lb_strategy_t lb_strategy;
    int rr_count;
    int tcp_rcvbuf;  /* TCP receive buffer size in bytes (0 = system default) */
} args_t;

err_t ARGS__parse(OUT args_t *args_out, IN int argc, IN char *argv[]);
void args_print_usage(IN const char *prog_name);
void args_print_help(IN const char *prog_name);

#endif /* #ifndef GANON_ARGS_H */