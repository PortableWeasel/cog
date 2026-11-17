/** @file host_options.c @brief Fixed installed roles, with one filtered argv owner. */
#define _POSIX_C_SOURCE 200809L
#include "host_options.h"
#include "options_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum host_flag {
    H_HEADLESS, H_ATTACH, H_WATCH, H_WORKER, H_WORKERS, H_WORKER_SOCKET,
    H_CHILDREN, H_REQUESTS, H_TOOLS, H_POLICY, H_RECONNECT, H_PROJECTION,
    H_CONTROL_SOCKET, H_INCARNATION, H_NUMBER, H_PARENT, H_SLOTS, H_FRAME, H_COUNT
};
typedef struct host_flag_spec { const char *name; bool value; } host_flag_spec_t;
static const host_flag_spec_t host_flags[H_COUNT] = {
    {"--headless", false}, {"--attach", true}, {"--watch", true}, {"--worker", false},
    {"--workers", true}, {"--worker-socket", true}, {"--children", true},
    {"--child-requests", true}, {"--child-tools", true}, {"--child-policy", true},
    {"--reconnect-ms", true}, {"--projection-bytes", true}, {"--control-socket", true},
    {"--worker-incarnation", true}, {"--worker-number", true}, {"--parent-pid", true},
    {"--executor-slots", true}, {"--frame-bytes", true}
};
static bool flag_match(const char *arg, const char *flag)
{
    size_t length;
    if (flag == NULL) return false;
    length = strlen(flag);
    return strncmp(arg, flag, length) == 0 && (arg[length] == '\0' || arg[length] == '=');
}
static bool public_value(const char *arg)
{
    static const char *const simple[] = { "--resume", "--fork", "--recover", "--from-node",
        "--prompt", "-p", "--session", "-s", "--log", "-l", "--socket", "-S",
        "--config", "--chunks", "-c", "--queue", "-q", "--mock-mode", "-M" };
    for (size_t i = 0U; i < cog_cli_field_count; ++i)
        if (flag_match(arg, cog_cli_fields[i].flag) || flag_match(arg, cog_cli_fields[i].short_flag))
            return true;
    for (size_t i = 0U; i < sizeof(simple) / sizeof(simple[0]); ++i)
        if (flag_match(arg, simple[i])) return true;
    return false;
}
static cog_result_t host_bad(int *exit_code)
{
    *exit_code = 2;
    if (fputs("cog-cli: invalid host options; use --help\n", stderr) == EOF ||
        fflush(stderr) != 0) return COG_ERROR_IO;
    return COG_ERROR_ABORTED;
}
static bool decimal(const char *s, uint64_t maximum, uint64_t *value)
{
    uint64_t n = 0U;
    if (s == NULL || *s == '\0') return false;
    for (; *s != '\0'; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch < '0' || ch > '9' || n > (maximum - (uint64_t)(ch - '0')) / 10U)
            return false;
        n = n * 10U + (uint64_t)(ch - '0');
    }
    *value = n; return true;
}
static bool tool_bits(const char *s, uint32_t *bits)
{
    static const char *const names[] = {"read", "search", "edit", "command"};
    uint32_t mask = 0U;
    if (strcmp(s, "none") == 0) { *bits = 0U; return true; }
    if (strcmp(s, "all") == 0) { *bits = 15U; return true; }
    for (;;) {
        const char *end = strchr(s, ',');
        size_t len = end != NULL ? (size_t)(end - s) : strlen(s), i;
        for (i = 0U; i < 4U; ++i)
            if (strlen(names[i]) == len && memcmp(s, names[i], len) == 0) break;
        if (i == 4U || (mask & (1U << i)) != 0U) return false;
        mask |= 1U << i;
        if (end == NULL) break;
        s = end + 1;
    }
    *bits = mask; return true;
}
static cog_result_t set_host(cog_cli_host_options_t *o, enum host_flag flag,
    const char *value)
{
    uint64_t n;
    switch (flag) {
    case H_HEADLESS: case H_ATTACH: case H_WATCH: case H_WORKER:
        if (o->role != COG_CLI_HOST_STANDALONE) return COG_ERROR_INVALID_ARG;
        o->role = flag == H_HEADLESS ? COG_CLI_HOST_HEADLESS : flag == H_ATTACH ?
            COG_CLI_HOST_ATTACH : flag == H_WATCH ? COG_CLI_HOST_WATCH : COG_CLI_HOST_WORKER;
        if (flag == H_ATTACH || flag == H_WATCH) o->client.socket_path = value;
        return COG_OK;
    case H_WORKER_SOCKET: o->runtime.worker_socket = value; return COG_OK;
    case H_CONTROL_SOCKET: o->worker.socket_path = value; return COG_OK;
    case H_TOOLS:
        return tool_bits(value, &o->runtime.child_profile.tools) ? COG_OK : COG_ERROR_INVALID_ARG;
    case H_POLICY:
        if (strcmp(value, "inherit") == 0) o->runtime.child_profile.policy = COG_CLI_CHILD_POLICY_INHERIT;
        else if (strcmp(value, "deny") == 0) o->runtime.child_profile.policy = COG_CLI_CHILD_POLICY_DENY;
        else if (strcmp(value, "ask") == 0) o->runtime.child_profile.policy = COG_CLI_CHILD_POLICY_ASK;
        else return COG_ERROR_INVALID_ARG;
        return COG_OK;
    case H_WORKERS: case H_CHILDREN: case H_REQUESTS: case H_RECONNECT: case H_PROJECTION:
    case H_INCARNATION: case H_NUMBER: case H_PARENT: case H_SLOTS: case H_FRAME:
        if (!decimal(value, UINT64_MAX, &n)) return COG_ERROR_INVALID_ARG;
        break;
    case H_COUNT: return COG_ERROR_INVALID_ARG;
    }
    switch (flag) {
    case H_WORKERS:
        if (n > 8U) return COG_ERROR_INVALID_ARG;
        o->runtime.workers = (size_t)n; break;
    case H_CHILDREN:
        if (n > 16U) return COG_ERROR_INVALID_ARG;
        o->runtime.children = (size_t)n; break;
    case H_REQUESTS: o->runtime.child_requests = n; break;
    case H_RECONNECT:
        if (n == 0U || n > 30000U) return COG_ERROR_INVALID_ARG;
        o->client.reconnect_ms = (uint32_t)n; break;
    case H_PROJECTION:
        if (n == 0U || n > 16U * 1024U * 1024U) return COG_ERROR_INVALID_ARG;
        o->client.projection_bytes = (size_t)n; break;
    case H_INCARNATION:
        if (n == 0U) return COG_ERROR_INVALID_ARG;
        o->worker.worker.incarnation = n; break;
    case H_NUMBER:
        if (n == 0U) return COG_ERROR_INVALID_ARG;
        o->worker.worker.number = n; break;
    case H_PARENT:
        if (n == 0U) return COG_ERROR_INVALID_ARG;
        o->worker.parent_pid = n; break;
    case H_SLOTS:
        if (n == 0U || n > 18U) return COG_ERROR_INVALID_ARG;
        o->worker.executor_slots = (size_t)n; break;
    case H_FRAME:
        if (n == 0U || n > UINT32_MAX) return COG_ERROR_INVALID_ARG;
        o->worker.frame_bytes = (size_t)n; break;
    default: return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}
static uint32_t bit(enum host_flag flag) { return UINT32_C(1) << (unsigned int)flag; }
static cog_result_t host_finish(cog_cli_host_options_t *o, uint32_t seen,
    char **filtered, int count, bool interactive, size_t sockets, int *exit_code)
{
    uint32_t permitted = 0U;
    cog_result_t rc;
    if (o->role == COG_CLI_HOST_HEADLESS) {
        for (enum host_flag f = H_WORKERS; f <= H_POLICY; f = (enum host_flag)(f + 1)) permitted |= bit(f);
        permitted |= bit(H_HEADLESS);
        if ((seen & ~permitted) != 0U) return host_bad(exit_code);
    } else if (o->role == COG_CLI_HOST_ATTACH || o->role == COG_CLI_HOST_WATCH) {
        permitted = bit(H_ATTACH) | bit(H_WATCH) | bit(H_RECONNECT) | bit(H_PROJECTION);
        if ((seen & ~permitted) != 0U) return host_bad(exit_code);
        if (count > 1) {
            if (count != 2 || (strcmp(filtered[1], "--help") != 0 && strcmp(filtered[1], "-h") != 0))
                return host_bad(exit_code);
            if (fputs("Usage: cog-cli --attach PATH | --watch PATH [--reconnect-ms N] [--projection-bytes N]\n", stdout) == EOF ||
                fflush(stdout) != 0) return COG_ERROR_IO;
            return COG_ERROR_ABORTED;
        }
        o->client.watch = o->role == COG_CLI_HOST_WATCH;
        o->client.terminal = isatty(STDOUT_FILENO) == 1;
        return COG_OK;
    } else if (o->role == COG_CLI_HOST_WORKER) {
        permitted = bit(H_WORKER);
        for (enum host_flag f = H_CONTROL_SOCKET; f < H_COUNT; f = (enum host_flag)(f + 1)) permitted |= bit(f);
        if (seen != permitted || count > 1) return host_bad(exit_code);
        return COG_OK;
    } else if (seen != 0U) return host_bad(exit_code);
    rc = cog_cli_options_parse(&o->application, count, filtered, exit_code);
    if (rc == COG_ERROR_ABORTED) {
        int output_error = fflush(stdout);
        int diagnostic_error = fflush(stderr);
        if (output_error != 0 || diagnostic_error != 0 || ferror(stdout) || ferror(stderr))
            return COG_ERROR_IO;
    }
    if (rc != COG_OK) return rc;
    if (o->role == COG_CLI_HOST_STANDALONE) {
        if (o->application.socket_path != NULL) return host_bad(exit_code);
        return COG_OK;
    }
    if (interactive || sockets != 1U || o->application.socket_path == NULL ||
        o->application.store_mode == COG_CLI_STORE_DISABLED ||
        o->application.session_action == COG_CLI_SESSION_LIST ||
        o->application.session_action == COG_CLI_SESSION_RECOVER ||
        (o->runtime.workers == 0U ? o->runtime.worker_socket != NULL :
            o->runtime.worker_socket == NULL || strcmp(o->runtime.worker_socket, o->application.socket_path) == 0))
        return host_bad(exit_code);
    o->application.interactive = false;
    o->runtime.ui_socket = o->application.socket_path;
    o->application.socket_path = NULL;
    o->runtime.root_options = &o->application;
    return COG_OK;
}
cog_result_t cog_cli_host_options_open(cog_cli_host_options_t *out,
    int argc, char **argv, int *exit_code)
{
    char **filtered;
    int count = 0;
    size_t bytes = 0U, sockets = 0U;
    uint32_t seen = 0U;
    bool interactive = false;
    if (out == NULL || exit_code == NULL) return COG_ERROR_NULL_PTR;
    *exit_code = 0;
    if (argc < 0 || (argc > 0 && argv == NULL)) return COG_ERROR_INVALID_ARG;
    if (argc > 1024) return COG_ERROR_LIMIT;
    for (int i = 0; i < argc; ++i) {
        size_t len;
        if (argv[i] == NULL) return COG_ERROR_INVALID_ARG;
        len = strnlen(argv[i], 65537U);
        if (len >= 65536U || len + 1U > 65536U - bytes) return COG_ERROR_LIMIT;
        bytes += len + 1U;
    }
    /* Explicit owner allocation is the only parser allocation; no copied
       strings or pointers outlive the caller's documented argv borrow. */
    filtered = calloc((size_t)argc + 1U, sizeof(*filtered));
    if (filtered == NULL) return COG_ERROR_NOMEM;
    out->owned_parse_storage = filtered;
    out->runtime.signal_fd = -1; out->runtime.diagnostic = stderr;
    out->worker.signal_fd = -1;
    out->client.input_fd = STDIN_FILENO; out->client.signal_fd = -1;
    out->client.output = stdout; out->client.diagnostic = stderr;
    out->client.projection_bytes = 8U * 1024U * 1024U; out->client.reconnect_ms = 3000U;
    if (argc > 0) filtered[count++] = argv[0];
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i], *value = NULL;
        size_t flag;
        for (flag = 0U; flag < H_COUNT; ++flag)
            if (flag_match(arg, host_flags[flag].name)) break;
        if (flag < H_COUNT) {
            enum host_flag f = (enum host_flag)flag;
            if ((seen & bit(f)) != 0U) return host_bad(exit_code);
            seen |= bit(f);
            if (host_flags[flag].value) {
                value = strchr(arg, '=');
                if (value != NULL) ++value;
                else if (i + 1 < argc) value = argv[++i];
                if (value == NULL || *value == '\0') return host_bad(exit_code);
            } else if (strchr(arg, '=') != NULL) return host_bad(exit_code);
            if (set_host(out, f, value) != COG_OK) return host_bad(exit_code);
            continue;
        }
        filtered[count++] = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0 ||
            strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) break;
        if (strcmp(arg, "--interactive") == 0 || strcmp(arg, "-i") == 0) interactive = true;
        if (flag_match(arg, "--socket") || flag_match(arg, "-S")) ++sockets;
        if (public_value(arg) && strchr(arg, '=') == NULL && i + 1 < argc)
            filtered[count++] = argv[++i];
    }
    return host_finish(out, seen, filtered, count, interactive, sockets, exit_code);
}
void cog_cli_host_options_close(cog_cli_host_options_t *options)
{
    if (options == NULL) return;
    free(options->owned_parse_storage);
    memset(options, 0, sizeof(*options));
}
