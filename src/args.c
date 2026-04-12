#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "common.h"
#include "logging.h"

static int is_flag_short(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    return (arg[0] == '-' && arg[1] != '-' && arg[1] != '\0');
}

static int is_flag_long(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    return (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0');
}

static int is_positional(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    return (arg[0] != '-');
}

static char *get_env(const char *key) {
    if (NULL == key) {
        return NULL;
    }
    return getenv(key);
}

static int parse_port(const char *value) {
    if (NULL == value) {
        return 0;
    }
    char *endptr = NULL;
    long port = strtol(value, &endptr, 10);
    if (NULL != endptr && endptr[0] != '\0') {
        return 0;
    }
    if (port < 0 || port > 65535) {
        return 0;
    }
    return (int)port;
}

err_t args_parse(args_t *args_out, int argc, char *argv[]) {
    err_t rc = E__SUCCESS;
    char *listen_ip = NULL;
    int listen_port = ARGS_PORT_DEFAULT;
    int ip_set = 0;
    int port_set = 0;

    if (NULL == args_out) {
        FAIL(E__ARGS__INVALID_ARGUMENTS);
    }

    char *env_ip = get_env(ARGS_ENV_LISTEN_IP);
    char *env_port = get_env(ARGS_ENV_LISTEN_PORT);
    if (NULL != env_ip) {
        LOG_TRACE("Using LISTEN_IP from environment: %s", env_ip);
        FAIL_IF(NULL != listen_ip,
                E__ARGS__CONFLICTING_ARGUMENTS);
        listen_ip = env_ip;
        ip_set = 1;
    }
    if (NULL != env_port) {
        LOG_TRACE("Using LISTEN_PORT from environment: %s", env_port);
        int port = parse_port(env_port);
        FAIL_IF(0 == port && '0' != env_port[0],
                E__ARGS__INVALID_VALUE);
        listen_port = port;
        port_set = 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (is_positional(arg)) {
            LOG_TRACE("Using listen IP from positional argument: %s", arg);
            if (NULL != listen_ip) {
                LOG_WARNING("IP already set, cannot specify multiple IPs");
FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
            }
            if (ip_set) {
                FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
            }
            listen_ip = (char *)arg;
            ip_set = 1;
        } else if (is_flag_short(arg) || is_flag_long(arg)) {
            if (strcmp(arg, ARGS_FLAG_PORT_SHORT) == 0 || strcmp(arg, ARGS_FLAG_PORT_LONG) == 0) {
                LOG_TRACE("Port flag detected: %s", arg);
                FAIL_IF(i >= argc - 1,
                        E__ARGS__MISSING_VALUE);
                if (port_set) {
                    LOG_ERROR("Port already set via LISTEN_PORT env, cannot override with CLI -p/--port");
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                int port = parse_port(argv[i]);
                FAIL_IF(0 == port && '0' != argv[i][0],
                        E__ARGS__INVALID_VALUE);
                LOG_TRACE("Using port from CLI argument: %d", port);
                listen_port = port;
                port_set = 1;
            } else {
                LOG_ERROR("Unknown argument: %s", arg);
                FAIL(E__ARGS__INVALID_ARGUMENT);
            }
        }
    }

    FAIL_IF(NULL == listen_ip,
            E__ARGS__MISSING_VALUE);

    args_out->listen_ip = listen_ip;
    args_out->listen_port = listen_port;

l_cleanup:
    return rc;
}