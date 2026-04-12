#ifndef GANON_ARGS_H
#define GANON_ARGS_H

#include <stddef.h>

#include "err.h"

#define ARGS_ENV_LISTEN_IP "LISTEN_IP"
#define ARGS_ENV_LISTEN_PORT "LISTEN_PORT"
#define ARGS_FLAG_PORT_SHORT "-p"
#define ARGS_FLAG_PORT_LONG "--port"

#define ARGS_PORT_DEFAULT 5555

typedef struct {
    char *listen_ip;
    int listen_port;
} args_t;

err_t args_parse(args_t *args_out, int argc, char *argv[]);

#endif