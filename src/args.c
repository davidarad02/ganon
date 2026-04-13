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
    printf("  -c, --connect   Comma-separated list of IP:port to connect (default port: 5555)\n");
    printf("  -i, --node-id N Node ID (0 or greater)\n");
    printf("  -w, --connect-timeout N  Connect timeout in seconds (default: 5)\n");
    printf("  --reconnect-retries N    Reconnect retries on disconnect (default: 5, 0 to disable)\n");
    printf("  --reconnect-delay N       Delay between reconnect attempts (default: 5 seconds)\n");
    printf("  -h, --help      Show this help message\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  LISTEN_IP       Listen IP address (alternative to positional argument)\n");
    printf("  LISTEN_PORT     Listen port number (alternative to -p/--port)\n");
    printf("  CONNECT         Comma-separated list of IP:port to connect (alternative to -c/--connect)\n");
    printf("  NODE_ID         Node ID (alternative to -i/--node-id)\n");
    printf("  CONNECT_TIMEOUT  Connect timeout in seconds (alternative to -w/--connect-timeout)\n");
    printf("  RECONNECT_RETRIES   Reconnect retries (alternative to --reconnect-retries)\n");
    printf("  RECONNECT_DELAY     Delay between reconnect attempts (alternative to --reconnect-delay)\n");
}

static bool_t is_help_flag(const char *arg) {
    if (NULL == arg) {
        return false;
    }
    if (0 == strcmp(arg, ARGS_FLAG_HELP_SHORT) || 0 == strcmp(arg, ARGS_FLAG_HELP_LONG)) {
        return true;
    }
    return false;
}
#endif /* #ifdef __DEBUG__ */

static bool_t is_flag_short(const char *arg) {
    if (NULL == arg) {
        return false;
    }
    return ('-' == arg[0] && '-' != arg[1] && '\0' != arg[1]);
}

static bool_t is_flag_long(const char *arg) {
    if (NULL == arg) {
        return false;
    }
    return ('-' == arg[0] && '-' == arg[1] && '\0' != arg[2]);
}

static bool_t is_positional(const char *arg) {
    if (NULL == arg) {
        return false;
    }
    return ('-' != arg[0]);
}

static int count_v_flags(const char *arg) {
    if (NULL == arg || '-' != arg[0]) {
        return 0;
    }
    int count = 0;
    const char *p = arg + 1;
    while ('v' == *p) {
        count++;
        p++;
    }
    if ('\0' != *p) {
        return 0;
    }
    return count;
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

static int parse_node_id(const char *value) {
    if (NULL == value) {
        return -1;
    }
    char *endptr = NULL;
    long id = strtol(value, &endptr, 10);
    if (NULL != endptr && '\0' != endptr[0]) {
        return -1;
    }
    if (0 > id) {
        return -1;
    }
    return (int)id;
}

static int parse_int(const char *value) {
    if (NULL == value) {
        return -1;
    }
    char *endptr = NULL;
    long val = strtol(value, &endptr, 10);
    if (NULL != endptr && '\0' != endptr[0]) {
        return -1;
    }
    if (0 > val) {
        return -1;
    }
    return (int)val;
}

static bool_t validate_ip(const char *ip) {
    if (NULL == ip) {
        return false;
    }
    if ('\0' == ip[0]) {
        return false;
    }
    if ('.' == ip[0]) {
        return false;
    }
    int dots = 0;
    int part_idx = 0;
    int part_value = 0;
    const char *p = ip;
    while ('\0' != *p && 4 > part_idx) {
        if ('.' == *p) {
            if (255 < part_value) {
                return false;
            }
            dots++;
            part_idx++;
            part_value = 0;
            if (3 < part_idx) {
                return false;
            }
            if ('.' == *(p + 1) || '\0' == *(p + 1)) {
                return false;
            }
            p++;
            continue;
        }
        if ('0' > *p || '9' < *p) {
            return false;
        }
        part_value = part_value * 10 + (*p - '0');
        if (255 < part_value) {
            return false;
        }
        p++;
    }
    if (3 != dots || 3 != part_idx) {
        return false;
    }
    if ('\0' != *p) {
        return false;
    }
    return true;
}

static bool_t validate_listen_ip(const char *ip) {
    if (NULL == ip) {
        return false;
    }
    if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "*")) {
        return true;
    }
    return validate_ip(ip);
}

static err_t parse_connect_entry(const char *entry, addr_t *addr_out) {
    err_t rc = E__SUCCESS;

    if (NULL == entry || NULL == addr_out) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    const char *colon = strchr(entry, ':');
    char ip_part[16];
    int ip_len;

    if (NULL != colon) {
        ip_len = (int)(colon - entry);
        if (15 < ip_len) {
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }
        strncpy(ip_part, entry, ip_len);
        ip_part[ip_len] = '\0';

        if (!validate_ip(ip_part)) {
            LOG_ERROR("Invalid IP in connect entry: %s", ip_part);
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }

        int port = parse_port(colon + 1);
        if (0 == port) {
            LOG_ERROR("Invalid port in connect entry: %s", colon + 1);
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }
        addr_out->port = port;
    } else {
        ip_len = strlen(entry);
        if (15 < ip_len) {
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }
        strcpy(ip_part, entry);

        if (!validate_ip(ip_part)) {
            LOG_ERROR("Invalid IP in connect entry: %s", ip_part);
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }
        addr_out->port = ARGS_CONNECT_DEFAULT_PORT;
    }

    addr_out->ip = malloc(ip_len + 1);
    if (NULL == addr_out->ip) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    strcpy(addr_out->ip, ip_part);

l_cleanup:
    return rc;
}

static err_t parse_connect_list(const char *list, addr_t *addrs_out, int *count_out, int max_count) {
    err_t rc = E__SUCCESS;

    if (NULL == list || NULL == addrs_out || NULL == count_out) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    int count = 0;
    char *list_copy = malloc(strlen(list) + 1);
    if (NULL == list_copy) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    strcpy(list_copy, list);

    char *token = strtok(list_copy, ",");
    while (NULL != token) {
        if (count >= max_count) {
            LOG_ERROR("Too many connect entries (max: %d)", max_count);
            free(list_copy);
            FAIL(E__ARGS__TOO_MANY_CONNECT_ENTRIES);
        }

        while (' ' == *token) {
            token++;
        }

        char *end = token + strlen(token) - 1;
        while (end > token && ' ' == *end) {
            *end = '\0';
            end--;
        }

        if ('\0' != *token) {
            rc = parse_connect_entry(token, &addrs_out[count]);
            if (E__SUCCESS != rc) {
                for (int i = 0; i < count; i++) {
                    free(addrs_out[i].ip);
                }
                free(list_copy);
                goto l_cleanup;
            }
            count++;
        }

        token = strtok(NULL, ",");
    }

    free(list_copy);
    *count_out = count;

l_cleanup:
    return rc;
}

err_t ARGS__parse(args_t *args_out, int argc, char *argv[]) {
    err_t rc = E__SUCCESS;

    char *listen_ip = NULL;
    int listen_port = ARGS_PORT_DEFAULT;
    int ip_set = 0;
    int port_set = 0;
    int node_id = -1;
    int node_id_set = 0;
    int connect_timeout = ARGS_CONNECT_TIMEOUT_DEFAULT;
    int connect_timeout_set = 0;
    int reconnect_retries = ARGS_RECONNECT_RETRIES_DEFAULT;
    int reconnect_retries_set = 0;
    int reconnect_delay = ARGS_RECONNECT_DELAY_DEFAULT;
    int reconnect_delay_set = 0;

    if (NULL == args_out) {
        rc = E__INVALID_ARG_NULL_POINTER;
        goto l_cleanup;
    }
    args_out->connect_count = 0;
    args_out->node_id = -1;
    args_out->connect_timeout = ARGS_CONNECT_TIMEOUT_DEFAULT;
    args_out->reconnect_retries = ARGS_RECONNECT_RETRIES_DEFAULT;
    args_out->reconnect_delay = ARGS_RECONNECT_DELAY_DEFAULT;

#ifdef __DEBUG__
    for (int i = 1; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            args_print_help(argv[0]);
            exit(0);
        }
    }

    int v_count = 0;
    for (int i = 1; i < argc; i++) {
        int v = count_v_flags(argv[i]);
        if (v > 0) {
            v_count += v;
        }
        if (v_count >= 2) {
            v_count = 2;
        }
    }

    const char *log_level_env = getenv("LOG_LEVEL");
    if (NULL != log_level_env && v_count > 0) {
        LOG_ERROR("Cannot use both LOG_LEVEL env var and -v flag");
        FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
    }

    if (v_count >= 2) {
        g_log_level = LOG_LEVEL_TRACE;
    } else if (v_count >= 1) {
        g_log_level = LOG_LEVEL_DEBUG;
    } else if (NULL != log_level_env) {
        if (0 == strcmp(log_level_env, "info")) {
            g_log_level = LOG_LEVEL_INFO;
        } else if (0 == strcmp(log_level_env, "debug")) {
            g_log_level = LOG_LEVEL_DEBUG;
        } else if (0 == strcmp(log_level_env, "trace")) {
            g_log_level = LOG_LEVEL_TRACE;
        } else {
            LOG_ERROR("Invalid LOG_LEVEL value: %s (must be info, debug, or trace)", log_level_env);
            FAIL(E__ARGS__INVALID_FORMAT);
        }
    }
#endif /* #ifdef __DEBUG__ */

    char *env_ip = get_env(ARGS_ENV_LISTEN_IP);
    char *env_port = get_env(ARGS_ENV_LISTEN_PORT);
    char *env_connect = get_env(ARGS_ENV_CONNECT);
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
    if (NULL != env_connect) {
        LOG_TRACE("Using CONNECT from environment: %s", env_connect);
        int count = 0;
        rc = parse_connect_list(env_connect, args_out->connect_addrs, &count, ARGS_MAX_CONNECT_ENTRIES);
        if (E__SUCCESS != rc) {
            for (int i = 0; i < count; i++) {
                free(args_out->connect_addrs[i].ip);
            }
            goto l_cleanup;
        }
        args_out->connect_count = count;
    }
    char *env_node_id = get_env(ARGS_ENV_NODE_ID);
    if (NULL != env_node_id) {
        LOG_TRACE("Using NODE_ID from environment: %s", env_node_id);
        FAIL_IF(0 <= node_id, E__ARGS__CONFLICTING_ARGUMENTS);
        int id = parse_node_id(env_node_id);
        if (0 > id) {
            LOG_ERROR("Invalid NODE_ID value: %s (must be 0 or greater)", env_node_id);
            FAIL(E__ARGS__INVALID_NODE_ID);
        }
        node_id = id;
        node_id_set = 1;
    }
    char *env_connect_timeout = get_env(ARGS_ENV_CONNECT_TIMEOUT);
    if (NULL != env_connect_timeout) {
        LOG_TRACE("Using CONNECT_TIMEOUT from environment: %s", env_connect_timeout);
        FAIL_IF(0 != connect_timeout_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int t = parse_int(env_connect_timeout);
        if (1 > t) {
            LOG_ERROR("Invalid CONNECT_TIMEOUT value: %s (must be 1 or greater)", env_connect_timeout);
            FAIL(E__ARGS__INVALID_CONNECT_TIMEOUT);
        }
        connect_timeout = t;
        connect_timeout_set = 1;
    }
    char *env_reconnect_retries = get_env(ARGS_ENV_RECONNECT_RETRIES);
    if (NULL != env_reconnect_retries) {
        LOG_TRACE("Using RECONNECT_RETRIES from environment: %s", env_reconnect_retries);
        FAIL_IF(0 != reconnect_retries_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int r = parse_int(env_reconnect_retries);
        if (0 > r) {
            LOG_ERROR("Invalid RECONNECT_RETRIES value: %s (must be 0 or greater)", env_reconnect_retries);
            FAIL(E__ARGS__INVALID_RECONNECT_RETRIES);
        }
        reconnect_retries = r;
        reconnect_retries_set = 1;
    }
    char *env_reconnect_delay = get_env(ARGS_ENV_RECONNECT_DELAY);
    if (NULL != env_reconnect_delay) {
        LOG_TRACE("Using RECONNECT_DELAY from environment: %s", env_reconnect_delay);
        FAIL_IF(0 != reconnect_delay_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int d = parse_int(env_reconnect_delay);
        if (1 > d) {
            LOG_ERROR("Invalid RECONNECT_DELAY value: %s (must be 1 or greater)", env_reconnect_delay);
            FAIL(E__ARGS__INVALID_RECONNECT_DELAY);
        }
        reconnect_delay = d;
        reconnect_delay_set = 1;
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
            if (0 == strcmp(arg, ARGS_FLAG_PORT_SHORT) || 0 == strcmp(arg, ARGS_FLAG_PORT_LONG)) {
                LOG_TRACE("Port flag detected: %s", arg);
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
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
            } else if (0 == strcmp(arg, ARGS_FLAG_CONNECT_SHORT) || 0 == strcmp(arg, ARGS_FLAG_CONNECT_LONG)) {
                LOG_TRACE("Connect flag detected: %s", arg);
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
                    FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                }
                i++;
                int count = 0;
                rc = parse_connect_list(argv[i], &args_out->connect_addrs[args_out->connect_count], &count,
                                        ARGS_MAX_CONNECT_ENTRIES - args_out->connect_count);
                if (E__SUCCESS != rc) {
                    for (int j = 0; j < args_out->connect_count + count; j++) {
                        free(args_out->connect_addrs[j].ip);
                    }
                    args_out->connect_count = 0;
                    goto l_cleanup;
                }
                args_out->connect_count += count;
            } else if (0 == strcmp(arg, ARGS_FLAG_NODE_ID_SHORT) || 0 == strcmp(arg, ARGS_FLAG_NODE_ID_LONG)) {
                LOG_TRACE("Node ID flag detected: %s", arg);
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
                    FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                }
                if (node_id_set) {
                    LOG_ERROR("Node ID already set via NODE_ID env, cannot override with CLI -i/--node-id");
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                int id = parse_node_id(argv[i]);
                if (0 > id) {
                    LOG_ERROR("Invalid node ID value: %s (must be 0 or greater)", argv[i]);
                    FAIL(E__ARGS__INVALID_NODE_ID);
                }
                LOG_TRACE("Using node ID from CLI argument: %d", id);
                node_id = id;
                node_id_set = 1;
            } else if (0 == strcmp(arg, ARGS_FLAG_CONNECT_TIMEOUT_SHORT) || 0 == strcmp(arg, ARGS_FLAG_CONNECT_TIMEOUT_LONG)) {
                LOG_TRACE("Connect timeout flag detected: %s", arg);
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
                    FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                }
                if (connect_timeout_set) {
                    LOG_ERROR("Connect timeout already set via CONNECT_TIMEOUT env, cannot override with CLI -w/--connect-timeout");
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                int t = parse_int(argv[i]);
                if (1 > t) {
                    LOG_ERROR("Invalid connect timeout value: %s (must be 1 or greater)", argv[i]);
                    FAIL(E__ARGS__INVALID_CONNECT_TIMEOUT);
                }
                LOG_TRACE("Using connect timeout from CLI argument: %d", t);
                connect_timeout = t;
                connect_timeout_set = 1;
            } else if (0 == strcmp(arg, ARGS_FLAG_RECONNECT_RETRIES_LONG)) {
                LOG_TRACE("Reconnect retries flag detected: %s", arg);
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
                    FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                }
                if (reconnect_retries_set) {
                    LOG_ERROR("Reconnect retries already set via RECONNECT_RETRIES env, cannot override with CLI --reconnect-retries");
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                int r = parse_int(argv[i]);
                if (0 > r) {
                    LOG_ERROR("Invalid reconnect retries value: %s (must be 0 or greater)", argv[i]);
                    FAIL(E__ARGS__INVALID_RECONNECT_RETRIES);
                }
                LOG_TRACE("Using reconnect retries from CLI argument: %d", r);
                reconnect_retries = r;
                reconnect_retries_set = 1;
            } else if (0 == strcmp(arg, ARGS_FLAG_RECONNECT_DELAY_LONG)) {
                LOG_TRACE("Reconnect delay flag detected: %s", arg);
                if (i >= argc - 1) {
#ifdef __DEBUG__
                    args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
                    FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                }
                if (reconnect_delay_set) {
                    LOG_ERROR("Reconnect delay already set via RECONNECT_DELAY env, cannot override with CLI --reconnect-delay");
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                int d = parse_int(argv[i]);
                if (1 > d) {
                    LOG_ERROR("Invalid reconnect delay value: %s (must be 1 or greater)", argv[i]);
                    FAIL(E__ARGS__INVALID_RECONNECT_DELAY);
                }
                LOG_TRACE("Using reconnect delay from CLI argument: %d", d);
                reconnect_delay = d;
                reconnect_delay_set = 1;
#ifdef __DEBUG__
            } else if (count_v_flags(arg) > 0) {
                continue;
#endif /* #ifdef __DEBUG__ */
            } else {
                LOG_ERROR("Unknown argument: %s", arg);
                FAIL(E__ARGS__UNKNOWN_FLAG);
            }
        }
    }

    if (NULL == listen_ip) {
#ifdef __DEBUG__
        args_print_usage(argv[0]);
#endif /* #ifdef __DEBUG__ */
        FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
    }

    if (!validate_listen_ip(listen_ip)) {
        LOG_ERROR("Invalid IP address format: %s (expected IPv4: 0-255.0-255.0-255.0-255)", listen_ip);
        FAIL(E__ARGS__INVALID_FORMAT);
    }

    if (0 > node_id) {
        LOG_ERROR("Node ID is required (use -i or --node-id, or NODE_ID env var)");
        FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
    }

    args_out->listen_addr.ip = listen_ip;
    args_out->listen_addr.port = listen_port;
    args_out->node_id = node_id;
    args_out->connect_timeout = connect_timeout;
    args_out->reconnect_retries = reconnect_retries;
    args_out->reconnect_delay = reconnect_delay;

l_cleanup:
    return rc;
}