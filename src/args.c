#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "common.h"
#include "logging.h"
#include "skin.h"

#ifdef __DEBUG__
void args_print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Try '%s --help' for more information.\n", prog_name);
}

void args_print_help(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -p, --port N             Listen port (shorthand for --listen <skin>:<ip>:<port>)\n");
    printf("  -c, --connect ADDR       Comma-separated list of ip:port[:skin] to connect\n");
    printf("  -i, --node-id N          Node ID (0 or greater)\n");
    printf("  -w, --connect-timeout N  Connect timeout in seconds (default: %d)\n", ARGS_CONNECT_TIMEOUT_DEFAULT);
    printf("  --reconnect-retries N    Reconnect retries (default: %d, 0=disable, always=unlimited)\n", ARGS_RECONNECT_RETRIES_DEFAULT);
    printf("  --reconnect-delay N      Delay between reconnects in seconds (default: %d)\n", ARGS_RECONNECT_DELAY_DEFAULT);
    printf("  --lb-strategy STR        Load balancing: round-robin (default), all-routes, sticky\n");
    printf("  --rr-count N             Routes per round-robin step (default: %d)\n", ARGS_RR_COUNT_DEFAULT);
    printf("  --reorder-timeout N      Out-of-order buffering timeout ms (default: %d)\n", ARGS_REORDER_TIMEOUT_DEFAULT);
    printf("  --tcp-rcvbuf N           TCP receive buffer size in bytes (0=system default)\n");
    printf("  --file-chunk-size N      File upload/download chunk size in bytes (default: %d)\n", ARGS_FILE_CHUNK_SIZE_DEFAULT);
    printf("  --reorder                Enable packet reordering buffering\n");
    printf("  --skin NAME              Skin for -p/--port listener (default: tcp-monocypher)\n");
    printf("  --default-skin NAME      Skin for outbound --connect entries (default: tcp-monocypher)\n");
    printf("  --listen SKIN:IP:PORT    Add a listener (repeatable; if used, -p/--port is ignored)\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nEnvironment variables:\n");
    printf("  LISTEN_IP, LISTEN_PORT, CONNECT, NODE_ID, CONNECT_TIMEOUT\n");
    printf("  RECONNECT_RETRIES, RECONNECT_DELAY, LB_STRATEGY, RR_COUNT\n");
    printf("  REORDER_TIMEOUT, TCP_RCVBUF, REORDER, SKIN, DEFAULT_SKIN, LISTEN\n");
    printf("  FILE_CHUNK_SIZE\n");
}

static bool_t is_help_flag(const char *arg) {
    if (NULL == arg) return false;
    return (0 == strcmp(arg, ARGS_FLAG_HELP_SHORT) ||
            0 == strcmp(arg, ARGS_FLAG_HELP_LONG));
}
#endif /* #ifdef __DEBUG__ */

static bool_t is_flag_short(const char *arg) {
    if (NULL == arg) return false;
    return ('-' == arg[0] && '-' != arg[1] && '\0' != arg[1]);
}

static bool_t is_flag_long(const char *arg) {
    if (NULL == arg) return false;
    return ('-' == arg[0] && '-' == arg[1] && '\0' != arg[2]);
}

static bool_t is_positional(const char *arg) {
    if (NULL == arg) return false;
    return ('-' != arg[0]);
}

#ifdef __DEBUG__
static int count_v_flags(const char *arg) {
    if (NULL == arg || '-' != arg[0]) return 0;
    int count = 0;
    const char *p = arg + 1;
    while ('v' == *p) { count++; p++; }
    if ('\0' != *p) return 0;
    return count;
}
#endif /* #ifdef __DEBUG__ */

static char *get_env(const char *key) {
    return (NULL != key) ? getenv(key) : NULL;
}

static int parse_port(const char *value) {
    if (NULL == value) return 0;
    char *endptr = NULL;
    long port = strtol(value, &endptr, 10);
    if (NULL != endptr && '\0' != endptr[0]) return 0;
    if (1 > port || 65535 < port) return 0;
    return (int)port;
}

static int parse_node_id(const char *value) {
    if (NULL == value) return -1;
    char *endptr = NULL;
    long id = strtol(value, &endptr, 10);
    if (NULL != endptr && '\0' != endptr[0]) return -1;
    if (0 > id) return -1;
    return (int)id;
}

static int parse_int(const char *value) {
    if (NULL == value) return -1;
    char *endptr = NULL;
    long val = strtol(value, &endptr, 10);
    if (NULL != endptr && '\0' != endptr[0]) return -1;
    if (0 > val) return -1;
    return (int)val;
}

static bool_t validate_ip(const char *ip) {
    if (NULL == ip || '\0' == ip[0] || '.' == ip[0]) return false;
    int dots = 0, part_idx = 0, part_value = 0;
    const char *p = ip;
    while ('\0' != *p && 4 > part_idx) {
        if ('.' == *p) {
            if (255 < part_value) return false;
            dots++; part_idx++; part_value = 0;
            if (3 < part_idx) return false;
            if ('.' == *(p+1) || '\0' == *(p+1)) return false;
            p++; continue;
        }
        if ('0' > *p || '9' < *p) return false;
        part_value = part_value * 10 + (*p - '0');
        if (255 < part_value) return false;
        p++;
    }
    if (3 != dots || 3 != part_idx || '\0' != *p) return false;
    return true;
}

static bool_t validate_listen_ip(const char *ip) {
    if (NULL == ip) return false;
    if (0 == strcmp(ip, "0.0.0.0") || 0 == strcmp(ip, "*")) return true;
    return validate_ip(ip);
}

/*
 * Parse ip:port[:skin_name] into connect_entry_t.
 * skin_id in the entry is 0 when not specified (caller fills in the default).
 */
static err_t parse_connect_entry(const char *token, connect_entry_t *entry_out) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(token, entry_out);

    entry_out->addr.ip   = NULL;
    entry_out->addr.port = 0;
    entry_out->skin_id   = 0;

    /* Find first colon → ip:rest */
    const char *first_colon = strchr(token, ':');
    if (NULL == first_colon) {
        LOG_ERROR("Invalid connect entry (no colon): %s", token);
        FAIL(E__ARGS__CONNECT_PARSING_ERROR);
    }

    int ip_len = (int)(first_colon - token);
    if (15 < ip_len || 0 == ip_len) {
        FAIL(E__ARGS__CONNECT_PARSING_ERROR);
    }
    char ip_part[16];
    strncpy(ip_part, token, (size_t)ip_len);
    ip_part[ip_len] = '\0';

    if (!validate_ip(ip_part)) {
        LOG_ERROR("Invalid IP in connect entry: %s", ip_part);
        FAIL(E__ARGS__CONNECT_PARSING_ERROR);
    }

    /* rest is "port" or "port:skin_name" */
    const char *rest = first_colon + 1;
    const char *second_colon = strchr(rest, ':');

    int port;
    if (NULL != second_colon) {
        /* "port:skin_name" */
        char port_str[16];
        int port_len = (int)(second_colon - rest);
        if (0 == port_len || 15 < port_len) {
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }
        strncpy(port_str, rest, (size_t)port_len);
        port_str[port_len] = '\0';
        port = parse_port(port_str);
        if (0 == port) {
            LOG_ERROR("Invalid port in connect entry: %s", port_str);
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }

        const char *skin_name = second_colon + 1;
        const skin_ops_t *skin = SKIN__by_name(skin_name);
        if (NULL == skin) {
            LOG_ERROR("Unknown skin '%s' in connect entry", skin_name);
            FAIL(E__ARGS__INVALID_FORMAT);
        }
        entry_out->skin_id = skin->skin_id;
    } else {
        port = parse_port(rest);
        if (0 == port) {
            LOG_ERROR("Invalid port in connect entry: %s", rest);
            FAIL(E__ARGS__CONNECT_PARSING_ERROR);
        }
    }

    entry_out->addr.ip = malloc((size_t)ip_len + 1);
    FAIL_IF(NULL == entry_out->addr.ip, E__INVALID_ARG_NULL_POINTER);
    memcpy(entry_out->addr.ip, ip_part, (size_t)ip_len + 1);
    entry_out->addr.port = port;

l_cleanup:
    return rc;
}

static err_t parse_connect_list(const char *list,
                                 connect_entry_t *entries_out,
                                 int *count_out, int max_count) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(list, entries_out, count_out);

    int count = 0;
    char *list_copy = malloc(strlen(list) + 1);
    FAIL_IF(NULL == list_copy, E__INVALID_ARG_NULL_POINTER);
    strcpy(list_copy, list);

    char *token = strtok(list_copy, ",");
    while (NULL != token) {
        if (count >= max_count) {
            LOG_ERROR("Too many connect entries (max: %d)", max_count);
            FREE(list_copy);
            FAIL(E__ARGS__TOO_MANY_CONNECT_ENTRIES);
        }
        /* Trim whitespace */
        while (' ' == *token) token++;
        char *end = token + strlen(token) - 1;
        while (end > token && ' ' == *end) { *end = '\0'; end--; }

        if ('\0' != *token) {
            rc = parse_connect_entry(token, &entries_out[count]);
            if (E__SUCCESS != rc) {
                for (int i = 0; i < count; i++) FREE(entries_out[i].addr.ip);
                FREE(list_copy);
                goto l_cleanup;
            }
            count++;
        }
        token = strtok(NULL, ",");
    }
    FREE(list_copy);
    *count_out = count;

l_cleanup:
    return rc;
}

/*
 * Parse a single --listen entry: "skin:port" or "skin:ip:port".
 */
static err_t parse_listener_entry(const char *token, listener_entry_t *entry_out,
                                   const char *default_listen_ip) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(token, entry_out);

    entry_out->addr.ip   = NULL;
    entry_out->addr.port = 0;
    entry_out->skin_id   = 0;

    const char *first_colon = strchr(token, ':');
    if (NULL == first_colon) {
        LOG_ERROR("Invalid --listen entry (expected skin:port or skin:ip:port): %s", token);
        FAIL(E__ARGS__INVALID_FORMAT);
    }

    /* Skin name is before the first colon */
    int skin_len = (int)(first_colon - token);
    if (0 == skin_len || 63 < skin_len) {
        FAIL(E__ARGS__INVALID_FORMAT);
    }
    char skin_name[64];
    strncpy(skin_name, token, (size_t)skin_len);
    skin_name[skin_len] = '\0';

    const skin_ops_t *skin = SKIN__by_name(skin_name);
    if (NULL == skin) {
        LOG_ERROR("Unknown skin '%s' in --listen entry", skin_name);
        FAIL(E__ARGS__INVALID_FORMAT);
    }
    entry_out->skin_id = skin->skin_id;

    /* After the first colon: "port" or "ip:port" */
    const char *rest = first_colon + 1;
    const char *second_colon = strchr(rest, ':');

    char   listen_ip[64]  = "0.0.0.0";
    int    listen_port    = 0;

    if (NULL != second_colon) {
        /* "ip:port" */
        int ip_len = (int)(second_colon - rest);
        if (0 == ip_len || 63 < ip_len) {
            FAIL(E__ARGS__INVALID_FORMAT);
        }
        strncpy(listen_ip, rest, (size_t)ip_len);
        listen_ip[ip_len] = '\0';
        if (!validate_listen_ip(listen_ip)) {
            LOG_ERROR("Invalid IP in --listen entry: %s", listen_ip);
            FAIL(E__ARGS__INVALID_FORMAT);
        }
        listen_port = parse_port(second_colon + 1);
    } else {
        /* just "port" — use default listen IP */
        listen_port = parse_port(rest);
        if (NULL != default_listen_ip) {
            strncpy(listen_ip, default_listen_ip, sizeof(listen_ip) - 1);
            listen_ip[sizeof(listen_ip) - 1] = '\0';
        }
    }

    if (0 == listen_port) {
        LOG_ERROR("Invalid port in --listen entry: %s", token);
        FAIL(E__ARGS__INVALID_FORMAT);
    }

    entry_out->addr.ip = strdup(listen_ip);
    FAIL_IF(NULL == entry_out->addr.ip, E__INVALID_ARG_NULL_POINTER);
    entry_out->addr.port = listen_port;

l_cleanup:
    return rc;
}

err_t ARGS__parse(args_t *args_out, int argc, char *argv[]) {
    err_t rc = E__SUCCESS;

    char *listen_ip = NULL;
    int listen_port = ARGS_PORT_DEFAULT;
    int ip_set    = 0;
    int port_set  = 0;
    int node_id   = -1;
    int node_id_set = 0;
    int connect_timeout = ARGS_CONNECT_TIMEOUT_DEFAULT;
    int connect_timeout_set = 0;
    int reconnect_retries = ARGS_RECONNECT_RETRIES_DEFAULT;
    int reconnect_retries_set = 0;
    int reconnect_delay = ARGS_RECONNECT_DELAY_DEFAULT;
    int reconnect_delay_set = 0;
    lb_strategy_t lb_strategy = LB_STRATEGY_ROUND_ROBIN;
    int lb_strategy_set = 0;
    int rr_count = ARGS_RR_COUNT_DEFAULT;
    int rr_count_set = 0;
    int tcp_rcvbuf = ARGS_TCP_RCVBUF_DEFAULT;
    int tcp_rcvbuf_set = 0;
    int reorder = ARGS_REORDER_DEFAULT;
    int reorder_set = 0;
    int file_chunk_size = ARGS_FILE_CHUNK_SIZE_DEFAULT;
    int file_chunk_size_set = 0;
    uint32_t listener_skin_id  = SKIN_ID__TCP_MONOCYPHER;
    uint32_t default_skin_id   = SKIN_ID__TCP_MONOCYPHER;
    int explicit_listen        = 0;  /* 1 if --listen was used */

    if (NULL == args_out) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }
    memset(args_out, 0, sizeof(*args_out));
    args_out->node_id          = -1;
    args_out->connect_timeout  = ARGS_CONNECT_TIMEOUT_DEFAULT;
    args_out->reconnect_retries = ARGS_RECONNECT_RETRIES_DEFAULT;
    args_out->reconnect_delay  = ARGS_RECONNECT_DELAY_DEFAULT;
    args_out->reorder_timeout  = ARGS_REORDER_TIMEOUT_DEFAULT;
    args_out->lb_strategy      = LB_STRATEGY_ROUND_ROBIN;
    args_out->rr_count         = ARGS_RR_COUNT_DEFAULT;
    args_out->tcp_rcvbuf       = ARGS_TCP_RCVBUF_DEFAULT;
    args_out->reorder          = ARGS_REORDER_DEFAULT;
    args_out->file_chunk_size  = ARGS_FILE_CHUNK_SIZE_DEFAULT;
    args_out->default_skin_id  = SKIN_ID__TCP_MONOCYPHER;
    args_out->listener_skin_id = SKIN_ID__TCP_MONOCYPHER;

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
        if (v > 0) { v_count += v; }
        if (v_count >= 2) { v_count = 2; break; }
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
            LOG_ERROR("Invalid LOG_LEVEL: %s", log_level_env);
            FAIL(E__ARGS__INVALID_FORMAT);
        }
    }
#endif /* #ifdef __DEBUG__ */

    /* ---- Environment variables ---- */

    char *env_ip      = get_env(ARGS_ENV_LISTEN_IP);
    char *env_port    = get_env(ARGS_ENV_LISTEN_PORT);
    char *env_connect = get_env(ARGS_ENV_CONNECT);
    char *env_listen  = get_env(ARGS_ENV_LISTEN);
    char *env_skin    = get_env(ARGS_ENV_SKIN);
    char *env_default_skin = get_env(ARGS_ENV_DEFAULT_SKIN);

    if (NULL != env_ip) {
        FAIL_IF(NULL != listen_ip, E__ARGS__CONFLICTING_ARGUMENTS);
        listen_ip = env_ip;
        ip_set = 1;
    }
    if (NULL != env_port) {
        int port = parse_port(env_port);
        if (0 == port) { LOG_ERROR("Invalid LISTEN_PORT: %s", env_port); FAIL(E__ARGS__INVALID_FORMAT); }
        listen_port = port;
        port_set = 1;
    }
    if (NULL != env_skin) {
        const skin_ops_t *s = SKIN__by_name(env_skin);
        if (NULL == s) { LOG_ERROR("Unknown skin '%s' in SKIN env", env_skin); FAIL(E__ARGS__INVALID_FORMAT); }
        listener_skin_id = s->skin_id;
    }
    if (NULL != env_default_skin) {
        const skin_ops_t *s = SKIN__by_name(env_default_skin);
        if (NULL == s) { LOG_ERROR("Unknown skin '%s' in DEFAULT_SKIN env", env_default_skin); FAIL(E__ARGS__INVALID_FORMAT); }
        default_skin_id = s->skin_id;
    }
    if (NULL != env_connect) {
        int count = 0;
        rc = parse_connect_list(env_connect,
                                &args_out->connect_addrs[args_out->connect_count],
                                &count,
                                ARGS_MAX_CONNECT_ENTRIES - args_out->connect_count);
        if (E__SUCCESS != rc) {
            for (int i = 0; i < count; i++) FREE(args_out->connect_addrs[i].addr.ip);
            goto l_cleanup;
        }
        args_out->connect_count += count;
    }
    if (NULL != env_listen) {
        /* Comma-separated list of "skin:ip:port" or "skin:port" */
        char *list_copy = strdup(env_listen);
        FAIL_IF(NULL == list_copy, E__INVALID_ARG_NULL_POINTER);
        char *tok = strtok(list_copy, ",");
        while (NULL != tok) {
            while (' ' == *tok) tok++;
            if ('\0' != *tok) {
                if (args_out->listener_count >= ARGS_MAX_LISTENERS) {
                    LOG_ERROR("Too many listeners (max %d)", ARGS_MAX_LISTENERS);
                    free(list_copy);
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                rc = parse_listener_entry(
                    tok,
                    &args_out->listeners[args_out->listener_count],
                    listen_ip ? listen_ip : "0.0.0.0");
                if (E__SUCCESS != rc) { free(list_copy); goto l_cleanup; }
                args_out->listener_count++;
                explicit_listen = 1;
            }
            tok = strtok(NULL, ",");
        }
        free(list_copy);
    }

    char *env_node_id = get_env(ARGS_ENV_NODE_ID);
    if (NULL != env_node_id) {
        FAIL_IF(0 <= node_id, E__ARGS__CONFLICTING_ARGUMENTS);
        int id = parse_node_id(env_node_id);
        if (0 > id) { LOG_ERROR("Invalid NODE_ID: %s", env_node_id); FAIL(E__ARGS__INVALID_NODE_ID); }
        node_id = id; node_id_set = 1;
    }
    char *env_connect_timeout = get_env(ARGS_ENV_CONNECT_TIMEOUT);
    if (NULL != env_connect_timeout) {
        FAIL_IF(connect_timeout_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int t = parse_int(env_connect_timeout);
        if (1 > t) { LOG_ERROR("Invalid CONNECT_TIMEOUT: %s", env_connect_timeout); FAIL(E__ARGS__INVALID_CONNECT_TIMEOUT); }
        connect_timeout = t; connect_timeout_set = 1;
    }
    char *env_reconnect_retries = get_env(ARGS_ENV_RECONNECT_RETRIES);
    if (NULL != env_reconnect_retries) {
        FAIL_IF(reconnect_retries_set, E__ARGS__CONFLICTING_ARGUMENTS);
        if (0 == strcmp(env_reconnect_retries, "always")) {
            reconnect_retries = -1;
        } else {
            int r = parse_int(env_reconnect_retries);
            if (0 > r) { FAIL(E__ARGS__INVALID_RECONNECT_RETRIES); }
            reconnect_retries = r;
        }
        reconnect_retries_set = 1;
    }
    char *env_reconnect_delay = get_env(ARGS_ENV_RECONNECT_DELAY);
    if (NULL != env_reconnect_delay) {
        FAIL_IF(reconnect_delay_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int d = parse_int(env_reconnect_delay);
        if (1 > d) { FAIL(E__ARGS__INVALID_RECONNECT_DELAY); }
        reconnect_delay = d; reconnect_delay_set = 1;
    }
    char *env_lb_strategy = get_env(ARGS_ENV_LB_STRATEGY);
    if (NULL != env_lb_strategy) {
        FAIL_IF(lb_strategy_set, E__ARGS__CONFLICTING_ARGUMENTS);
        if (0 == strcmp(env_lb_strategy, "round-robin"))  { lb_strategy = LB_STRATEGY_ROUND_ROBIN; }
        else if (0 == strcmp(env_lb_strategy, "all-routes")) { lb_strategy = LB_STRATEGY_ALL_ROUTES; }
        else if (0 == strcmp(env_lb_strategy, "sticky"))     { lb_strategy = LB_STRATEGY_STICKY; }
        else { LOG_ERROR("Invalid LB_STRATEGY: %s", env_lb_strategy); FAIL(E__ARGS__INVALID_FORMAT); }
        lb_strategy_set = 1;
    }
    char *env_reorder_timeout = get_env(ARGS_ENV_REORDER_TIMEOUT);
    if (NULL != env_reorder_timeout) {
        int t = parse_int(env_reorder_timeout);
        if (0 > t) { FAIL(E__ARGS__INVALID_FORMAT); }
        args_out->reorder_timeout = t;
    }
    char *env_rr_count = get_env(ARGS_ENV_RR_COUNT);
    if (NULL != env_rr_count) {
        FAIL_IF(rr_count_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int n = parse_int(env_rr_count);
        if (1 > n) { FAIL(E__ARGS__INVALID_FORMAT); }
        rr_count = n; rr_count_set = 1;
    }
    char *env_tcp_rcvbuf = get_env(ARGS_ENV_TCP_RCVBUF);
    if (NULL != env_tcp_rcvbuf) {
        FAIL_IF(tcp_rcvbuf_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int n = parse_int(env_tcp_rcvbuf);
        if (0 > n) { FAIL(E__ARGS__INVALID_FORMAT); }
        tcp_rcvbuf = n; tcp_rcvbuf_set = 1;
    }
    char *env_reorder = get_env(ARGS_ENV_REORDER);
    if (NULL != env_reorder) {
        FAIL_IF(reorder_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int n = parse_int(env_reorder);
        if (0 != n && 1 != n) { FAIL(E__ARGS__INVALID_FORMAT); }
        reorder = n; reorder_set = 1;
    }
    char *env_file_chunk_size = get_env(ARGS_ENV_FILE_CHUNK_SIZE);
    if (NULL != env_file_chunk_size) {
        FAIL_IF(file_chunk_size_set, E__ARGS__CONFLICTING_ARGUMENTS);
        int n = parse_int(env_file_chunk_size);
        if (0 >= n) { FAIL(E__ARGS__INVALID_FORMAT); }
        file_chunk_size = n; file_chunk_size_set = 1;
    }

    /* ---- CLI arguments ---- */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (is_positional(arg)) {
            if (NULL != listen_ip || ip_set) {
                LOG_WARNING("IP already set, cannot specify multiple IPs");
                FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
            }
            listen_ip = (char *)arg;
            ip_set = 1;

        } else if (is_flag_short(arg) || is_flag_long(arg)) {

            if (0 == strcmp(arg, ARGS_FLAG_PORT_SHORT) ||
                0 == strcmp(arg, ARGS_FLAG_PORT_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (port_set) { LOG_ERROR("Port already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int port = parse_port(argv[i]);
                if (0 == port) { LOG_ERROR("Invalid port: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                listen_port = port; port_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_CONNECT_SHORT) ||
                       0 == strcmp(arg, ARGS_FLAG_CONNECT_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                i++;
                int count = 0;
                rc = parse_connect_list(argv[i],
                                        &args_out->connect_addrs[args_out->connect_count],
                                        &count,
                                        ARGS_MAX_CONNECT_ENTRIES - args_out->connect_count);
                if (E__SUCCESS != rc) {
                    for (int j = 0; j < args_out->connect_count + count; j++)
                        FREE(args_out->connect_addrs[j].addr.ip);
                    args_out->connect_count = 0;
                    goto l_cleanup;
                }
                args_out->connect_count += count;

            } else if (0 == strcmp(arg, ARGS_FLAG_NODE_ID_SHORT) ||
                       0 == strcmp(arg, ARGS_FLAG_NODE_ID_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (node_id_set) { LOG_ERROR("Node ID already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int id = parse_node_id(argv[i]);
                if (0 > id) { LOG_ERROR("Invalid node ID: %s", argv[i]); FAIL(E__ARGS__INVALID_NODE_ID); }
                node_id = id; node_id_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_CONNECT_TIMEOUT_SHORT) ||
                       0 == strcmp(arg, ARGS_FLAG_CONNECT_TIMEOUT_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (connect_timeout_set) { LOG_ERROR("Connect timeout already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int t = parse_int(argv[i]);
                if (1 > t) { LOG_ERROR("Invalid connect timeout: %s", argv[i]); FAIL(E__ARGS__INVALID_CONNECT_TIMEOUT); }
                connect_timeout = t; connect_timeout_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_RECONNECT_RETRIES_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (reconnect_retries_set) { LOG_ERROR("Reconnect retries already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                if (0 == strcmp(argv[i], "always")) {
                    reconnect_retries = -1;
                } else {
                    int r = parse_int(argv[i]);
                    if (0 > r) { LOG_ERROR("Invalid reconnect retries: %s", argv[i]); FAIL(E__ARGS__INVALID_RECONNECT_RETRIES); }
                    reconnect_retries = r;
                }
                reconnect_retries_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_RECONNECT_DELAY_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (reconnect_delay_set) { LOG_ERROR("Reconnect delay already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int d = parse_int(argv[i]);
                if (1 > d) { LOG_ERROR("Invalid reconnect delay: %s", argv[i]); FAIL(E__ARGS__INVALID_RECONNECT_DELAY); }
                reconnect_delay = d; reconnect_delay_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_LB_STRATEGY_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (lb_strategy_set) { LOG_ERROR("LB strategy already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                if (0 == strcmp(argv[i], "round-robin"))  { lb_strategy = LB_STRATEGY_ROUND_ROBIN; }
                else if (0 == strcmp(argv[i], "all-routes")) { lb_strategy = LB_STRATEGY_ALL_ROUTES; }
                else if (0 == strcmp(argv[i], "sticky"))     { lb_strategy = LB_STRATEGY_STICKY; }
                else { LOG_ERROR("Invalid lb-strategy: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                lb_strategy_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_REORDER_TIMEOUT_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                i++;
                int t = parse_int(argv[i]);
                if (0 > t) { LOG_ERROR("Invalid reorder timeout: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                args_out->reorder_timeout = t;

            } else if (0 == strcmp(arg, ARGS_FLAG_RR_COUNT_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (rr_count_set) { LOG_ERROR("RR count already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int n = parse_int(argv[i]);
                if (1 > n) { LOG_ERROR("Invalid rr-count: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                rr_count = n; rr_count_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_TCP_RCVBUF_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (tcp_rcvbuf_set) { LOG_ERROR("TCP rcvbuf already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int n = parse_int(argv[i]);
                if (0 > n) { LOG_ERROR("Invalid tcp-rcvbuf: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                tcp_rcvbuf = n; tcp_rcvbuf_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_REORDER_LONG)) {
                if (reorder_set) { LOG_ERROR("Reorder already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                reorder = 1; reorder_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_FILE_CHUNK_SIZE_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (file_chunk_size_set) { LOG_ERROR("File chunk size already set"); FAIL(E__ARGS__CONFLICTING_ARGUMENTS); }
                i++;
                int n = parse_int(argv[i]);
                if (0 >= n) { LOG_ERROR("Invalid file-chunk-size: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                file_chunk_size = n; file_chunk_size_set = 1;

            } else if (0 == strcmp(arg, ARGS_FLAG_SKIN_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                i++;
                const skin_ops_t *s = SKIN__by_name(argv[i]);
                if (NULL == s) { LOG_ERROR("Unknown skin: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                listener_skin_id = s->skin_id;

            } else if (0 == strcmp(arg, ARGS_FLAG_DEFAULT_SKIN_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                i++;
                const skin_ops_t *s = SKIN__by_name(argv[i]);
                if (NULL == s) { LOG_ERROR("Unknown default-skin: %s", argv[i]); FAIL(E__ARGS__INVALID_FORMAT); }
                default_skin_id = s->skin_id;

            } else if (0 == strcmp(arg, ARGS_FLAG_LISTEN_LONG)) {
                if (i >= argc - 1) FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
                if (args_out->listener_count >= ARGS_MAX_LISTENERS) {
                    LOG_ERROR("Too many --listen entries (max %d)", ARGS_MAX_LISTENERS);
                    FAIL(E__ARGS__CONFLICTING_ARGUMENTS);
                }
                i++;
                rc = parse_listener_entry(argv[i],
                                          &args_out->listeners[args_out->listener_count],
                                          listen_ip ? listen_ip : "0.0.0.0");
                FAIL_IF(E__SUCCESS != rc, rc);
                args_out->listener_count++;
                explicit_listen = 1;

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

    /* ---- Validate required fields ---- */

    if (0 > node_id) {
        LOG_ERROR("Node ID is required (use -i/--node-id or NODE_ID env)");
        FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
    }

    if (NULL == listen_ip) {
        listen_ip = "0.0.0.0";
    }
    if (!validate_listen_ip(listen_ip)) {
        LOG_ERROR("Invalid listen IP: %s", listen_ip);
        FAIL(E__ARGS__INVALID_FORMAT);
    }

    /* If no explicit --listen entries, synthesise one from -p/--port */
    if (!explicit_listen && port_set) {
        if (args_out->listener_count == 0) {
            args_out->listeners[0].addr.ip   = listen_ip;
            args_out->listeners[0].addr.port  = listen_port;
            args_out->listeners[0].skin_id    = listener_skin_id;
            args_out->listener_count = 1;
        }
    }


    if (args_out->listener_count == 0 && args_out->connect_count == 0) {
        LOG_ERROR("No listen port or connection addresses specified");
        FAIL(E__ARGS__MISSING_REQUIRED_ARGUMENT);
    }
    args_out->node_id          = node_id;
    args_out->connect_timeout  = connect_timeout;
    args_out->reconnect_retries = reconnect_retries;
    args_out->reconnect_delay  = reconnect_delay;
    args_out->lb_strategy      = lb_strategy;
    args_out->rr_count         = rr_count;
    args_out->tcp_rcvbuf       = tcp_rcvbuf;
    args_out->reorder          = reorder;
    args_out->file_chunk_size  = file_chunk_size;
    args_out->default_skin_id  = default_skin_id;
    args_out->listener_skin_id = listener_skin_id;

l_cleanup:
    return rc;
}
