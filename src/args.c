#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "common.h"
#include "logging.h"

#ifdef __DEBUG__
void args_print_usage(const char *prog_name) {
    printf("Usage: %s <LISTEN IP> [OPTIONS]\n", prog_name);
    printf("       %s [OPTIONS]\n", prog_name);
    printf("Try '%s --help' for more information.\n", prog_name);
}

void args_print_help(const char *prog_name) {
    printf("Usage: %s <LISTEN IP> [OPTIONS]\n", prog_name);
    printf("       %s [OPTIONS]\n", prog_name);
    printf("\n");
    printf("Arguments:\n");
    printf("  LISTEN IP       Listen IP address (IPv4 format: 0-255.0-255.0-255.0-255)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -p, --port N    Listen port number (1-65535)\n");
    printf("  -h, --help      Show this help message\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  LISTEN_IP       Listen IP address (alternative to positional argument)\n");
    printf("  LISTEN_PORT     Listen port number (alternative to -p/--port)\n");
}

static int is_help_flag(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    if (0 == strcmp(arg, ARGS_FLAG_HELP_SHORT) || 0 == strcmp(arg, ARGS_FLAG_HELP_LONG)) {
        return 1;
    }
    return 0;
}
#endif

static int is_flag_short(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    return ('-' == arg[0] && '-' != arg[1] && '\0' != arg[1]);
}

static int is_flag_long(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    return ('-' == arg[0] && '-' == arg[1] && '\0' != arg[2]);
}

static int is_positional(const char *arg) {
    if (NULL == arg) {
        return 0;
    }
    return ('-' != arg[0]);
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
    if (NULL != endptr && '\0' != endptr[0]) {
        return 0;
    }
    if (1 > port || 65535 < port) {
        return 0;
    }
    return (int)port;
}

static int validate_ip(const char *ip) {
    if (NULL == ip) {
        return 0;
    }
    if ('\0' == ip[0]) {
        return 0;
    }
    if ('.' == ip[0]) {
        return 0;
    }
    int dots = 0;
    int part_idx = 0;
    int part_value = 0;
    const char *p = ip;
    while ('\0' != *p && 4 > part_idx) {
        if ('.' == *p) {
            if (255 < part_value) {
                return 0;
            }
            dots++;
            part_idx++;
            part_value = 0;
            if (3 < part_idx) {
                return 0;
            }
            if ('.' == *(p + 1) || '\0' == *(p + 1)) {
                return 0;
            }
            p++;
            continue;
        }
        if ('0' > *p || '9' < *p) {
            return 0;
        }
        part_value = part_value * 10 + (*p - '0');
        if (255 < part_value) {
            return 0;
        }
        p++;
    }
    if (0 == part_value) {
        return 0;
    }
    if (3 != dots || 3 != part_idx) {
        return 0;
    }
    if ('\0' != *p) {
        return 0;
    }
    return 1;
}

err_t args_parse(args_t *args_out, int argc, char *argv[]) {
    err_t rc = E__SUCCESS;

    char *listen_ip = NULL;
    int listen_port = ARGS_PORT_DEFAULT;
    int ip_set = 0;
    int port_set = 0;

    if (NULL == args_out) {
        rc = E__INVALID_ARG_NULL_POINTER;
        goto l_cleanup;
    }

#ifdef __DEBUG__
    for (int i = 1; i < argc; i++) {
    for (int i = 1; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            args_print_help(argv[0]);
            exit(0);
        }
    }
#endif

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
        if (0 == port) {
            LOG_ERROR("Invalid LISTEN_PORT value: %s (must be 1-65535)", env_port);
            FAIL(E__ARGS__INVALID_FORMAT);
        }
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
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif
                    FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                }
                if (port_set) {
                    LOG_ERROR("Port already set via LISTEN_PORT env, cannot override with CLI -p/--port");
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                int port = parse_port(argv[i]);
                if (0 == port) {
                    LOG_ERROR("Invalid port value: %s (must be 1-65535)", argv[i]);
                    FAIL(E__ARGS__INVALID_FORMAT);
                }
                LOG_TRACE("Using port from CLI argument: %d", port);
                listen_port = port;
                port_set = 1;
            } else {
                LOG_ERROR("Unknown argument: %s", arg);
                FAIL(E__ARGS__UNKNOWN_FLAG);
            }
        }
    }

    if (NULL == listen_ip) {
#ifdef __DEBUG__
        args_print_usage(argv[0]);
#endif
        FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
    }

    if (!validate_ip(listen_ip)) {
        LOG_ERROR("Invalid IP address format: %s (expected IPv4: 0-255.0-255.0-255.0-255)", listen_ip);
        FAIL(E__ARGS__INVALID_FORMAT);
    }

    args_out->listen_ip = listen_ip;
    args_out->listen_port = listen_port;

l_cleanup:
    return rc;
}