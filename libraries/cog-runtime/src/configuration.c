/** @file configuration.c @brief Owned effective configuration and secret lifetime. */
#define _POSIX_C_SOURCE 200809L
#include "configuration.h"
#include "child_profile.h"
#include <cog/workspace.h>
#include <limits.h>
#include "options_internal.h"
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define FILE_BYTES 65536U
#define FIELD_BYTES 4096U
#define SECRET_BYTES 16384U
#define STRING_BYTES 65536U
#define LABEL_BYTES 128U
typedef enum configuration_origin {
    ORIGIN_STARTUP = 0, ORIGIN_RELOAD, ORIGIN_CHILD, ORIGIN_BOOTSTRAP, ORIGIN_CLONE
} configuration_origin_t;
struct cog_cli_configuration {
    cog_cli_options_t options;
    const cog_endpoint_t *resolved_endpoint;
    bool instruction_attached;
    cog_cli_instruction_capsule_t instructions;
    cog_endpoint_t endpoint;
    cog_model_t model;
    cog_model_table_t models;
    cog_openrouter_config_t adapter;
    uint64_t generation;
    bool bound, endpoint_present, host_finished;
    configuration_origin_t origin;
    uint32_t tool_mask;
    size_t used;
    char strings[STRING_BYTES];
    char secret[SECRET_BYTES];
    char file[FILE_BYTES + 1U];
    cog_cli_safe_labels_t labels;
    char session_label[LABEL_BYTES], model_label[LABEL_BYTES];
};
static void clear(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size-- != 0U)
        *p++ = 0U;
}
static cog_result_t owned(cog_cli_configuration_t *c, const char **field)
{
    size_t len;
    if (*field == NULL)
        return COG_OK;
    len = strlen(*field) + 1U;
    if (len > FIELD_BYTES || len > STRING_BYTES - c->used)
        return COG_ERROR_LIMIT;
    memcpy(c->strings + c->used, *field, len);
    *field = c->strings + c->used;
    c->used += len;
    return COG_OK;
}
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r')
        ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    *end = '\0';
    return s;
}
static cog_result_t file_options(cog_cli_configuration_t *c, const char *path)
{
    int fd;
    size_t len, i, start = 0U;
    uint32_t seen = 0U;
    if (path == NULL)
        return COG_OK;
    if (*path == '\0')
        return COG_ERROR_INVALID_ARG;
    fd = open(path,O_RDONLY | O_CLOEXEC);
    if(fd<0) return COG_ERROR_IO;
    len=0U;
    while(len<FILE_BYTES+1U) {
        ssize_t n=read(fd,c->file+len,FILE_BYTES+1U-len);
        if(n<0 && errno==EINTR) continue;
        if(n<0) { (void)close(fd); return COG_ERROR_IO; }
        if(n==0) break;
        len+=(size_t)n;
    }
    if(close(fd)!=0) return COG_ERROR_IO;
    if (len > FILE_BYTES)
        return COG_ERROR_LIMIT;
    if (memchr(c->file, '\0', len) != NULL)
        return COG_ERROR_INVALID_ARG;
    c->file[len] = '\0';
    for (i = 0U; i <= len; ++i)
        if (c->file[i] == '\n' || i == len) {
            char *line, *eq, *key, *val;
            size_t j;
            if (i - start >= FIELD_BYTES)
                return COG_ERROR_LIMIT;
            c->file[i] = '\0';
            line = trim(c->file + start);
            start = i + 1U;
            if (*line == '\0' || *line == '#')
                continue;
            eq = strchr(line, '=');
            if (eq == NULL)
                return COG_ERROR_INVALID_ARG;
            *eq = '\0';
            key = trim(line);
            val = trim(eq + 1);
            for (j = 0U; j < cog_cli_field_count; ++j)
                if (strcmp(cog_cli_fields[j].key, key) == 0) {
                    const cli_field_t *field = &cog_cli_fields[j];
                    cog_result_t rc;
                    if ((seen & field->bit) != 0U)
                        return COG_ERROR_INVALID_ARG;
                    seen |= field->bit;
                    if (field->bit == COG_CLI_OPT_ENDPOINT)
                        c->endpoint_present = true;
                    rc = cog_cli_field_set(&c->options, field, val);
                    if (rc != COG_OK)
                        return rc;
                    break;
                }
            if (j == cog_cli_field_count)
                return COG_ERROR_INVALID_ARG;
        }
    return COG_OK;
}
static cog_result_t overlay(cog_cli_options_t *o, const cog_cli_options_t *input)
{
    size_t i;
    uint32_t known = 0U;
    for (i = 0U; i < cog_cli_field_count; ++i)
        known |= cog_cli_fields[i].bit;
    if ((input->explicit_options & ~known) != 0U)
        return COG_ERROR_INVALID_ARG;
    for (i = 0U; i < cog_cli_field_count; ++i) {
        const cli_field_t *f = &cog_cli_fields[i];
        unsigned char *dest = (unsigned char *)o + f->offset;
        const unsigned char *source = (const unsigned char *)input + f->offset;
        const char *env = f->environment != NULL ? getenv(f->environment) : NULL;
        if ((input->explicit_options & f->bit) != 0U) {
            size_t bytes = 0U;
            switch (f->kind) {
            case CLI_STRING:
                bytes = sizeof(const char *);
                break;
            case CLI_BACKEND:
                bytes = sizeof(cog_cli_backend_t);
                break;
            case CLI_POLICY:
                bytes = sizeof(cog_cli_tool_policy_t);
                break;
            case CLI_STORE:
                bytes = sizeof(cog_cli_store_mode_t);
                break;
            case CLI_U32:
                bytes = sizeof(uint32_t);
                break;
            case CLI_SIZE:
                bytes = sizeof(size_t);
                break;
            }
            memcpy(dest, source, bytes);
        } else if (env != NULL) {
            cog_result_t rc = cog_cli_field_set(o, f, env);
            if (rc != COG_OK)
                return rc;
        }
    }
    o->explicit_options = input->explicit_options;
    return COG_OK;
}
static bool token(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (s == NULL || *s == '\0')
        return false;
    for (; *p != 0U; ++p)
        if (*p <= 32U || *p == 127U)
            return false;
    return true;
}
static bool hostname_valid(const char *host, const char *end)
{
    const char *p = host;
    if (p == end)
        return false;
    while (p < end) {
        const char *label = p;
        while (p < end && *p != '.') {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-'))
                return false;
            ++p;
        }
        if (p == label || *label == '-' || p[-1] == '-')
            return false;
        if (p < end)
            ++p; /* A trailing dot is a valid absolute hostname. */
    }
    return true;
}
static bool port_valid(const char *port, const char *end)
{
    uint32_t n = 0U;
    if (port == end)
        return false;
    while (port < end) {
        uint32_t digit;
        if (*port < '0' || *port > '9')
            return false;
        digit = (uint32_t)(*port - '0');
        if (n > (65535U - digit) / 10U)
            return false;
        n = n * 10U + digit;
        ++port;
    }
    return n != 0U;
}
static bool https_url(const char *s)
{
    const char *host, *end, *port = NULL;
    if (s == NULL || strncmp(s, "https://", 8U) != 0 || !token(s))
        return false;
    host = s + 8;
    end = strchr(host, '/');
    if (end == NULL)
        end = host + strlen(host);
    if (host == end || strchr(host, '?') != NULL || strchr(host, '#') != NULL ||
        strchr(host, '\\') != NULL)
        return false;
    if (*host == '[') {
        const char *close = memchr(host, ']', (size_t)(end - host));
        char literal[INET6_ADDRSTRLEN];
        unsigned char address[16];
        size_t length;
        if (close == NULL)
            return false;
        length = (size_t)(close - host) - 1U;
        if (length == 0U || length >= sizeof(literal))
            return false;
        memcpy(literal, host + 1, length);
        literal[length] = '\0';
        if (inet_pton(AF_INET6, literal, address) != 1)
            return false;
        if (close + 1 != end) {
            if (close[1] != ':')
                return false;
            port = close + 2;
        }
    } else {
        const char *colon = memchr(host, ':', (size_t)(end - host));
        if (!hostname_valid(host, colon != NULL ? colon : end))
            return false;
        if (colon != NULL)
            port = colon + 1;
    }
    return port == NULL || port_valid(port, end);
}
static bool metadata_action(cog_cli_session_action_t action)
{
    return action == COG_CLI_SESSION_LIST || action == COG_CLI_SESSION_RECOVER;
}
static bool environment_name_valid(const char *name)
{
    size_t i;
    if (!((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') ||
          *name == '_'))
        return false;
    for (i = 1U; name[i] != '\0'; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return false;
    return true;
}
static cog_result_t validate(cog_cli_configuration_t *c)
{
    cog_cli_options_t *o = &c->options;
    size_t i;
    bool metadata = metadata_action(o->session_action);
    if (o->backend != COG_CLI_BACKEND_REAL && o->backend != COG_CLI_BACKEND_MOCK)
        return COG_ERROR_INVALID_ARG;
    if (o->tool_policy != COG_CLI_TOOL_POLICY_ASK &&
        o->tool_policy != COG_CLI_TOOL_POLICY_DENY &&
        o->tool_policy != COG_CLI_TOOL_POLICY_ALLOW)
        return COG_ERROR_INVALID_ARG;
    if (o->store_mode != COG_CLI_STORE_DEFAULT &&
        o->store_mode != COG_CLI_STORE_DURABLE &&
        o->store_mode != COG_CLI_STORE_DISABLED)
        return COG_ERROR_INVALID_ARG;
    for (i = 0U; i < cog_cli_field_count; ++i) {
        const cli_field_t *f = &cog_cli_fields[i];
        const unsigned char *p = (const unsigned char *)o + f->offset;
        uint32_t u32 = 0U;
        size_t bytes = 0U;
        const char *string = NULL;
        if (f->kind == CLI_U32)
            memcpy(&u32, p, sizeof(u32));
        if (f->kind == CLI_SIZE)
            memcpy(&bytes, p, sizeof(bytes));
        if (f->kind == CLI_STRING)
            memcpy(&string, p, sizeof(string));
        if (!f->zero &&
            ((f->kind == CLI_U32 && u32 == 0U) || (f->kind == CLI_SIZE && bytes == 0U)))
            return COG_ERROR_LIMIT;
        if (f->kind == CLI_STRING && string != NULL && *string == '\0')
            return COG_ERROR_INVALID_ARG;
        if (f->kind == CLI_STRING && (o->explicit_options & f->bit) != 0U &&
            string == NULL)
            return COG_ERROR_INVALID_ARG;
    }
    if (o->store_mode == COG_CLI_STORE_DEFAULT)
        o->store_mode = o->session_action != COG_CLI_SESSION_NEW ||
                                o->backend == COG_CLI_BACKEND_REAL
                            ? COG_CLI_STORE_DURABLE
                            : COG_CLI_STORE_DISABLED;
    if (o->session_action != COG_CLI_SESSION_NEW &&
        o->store_mode == COG_CLI_STORE_DISABLED)
        return COG_ERROR_INVALID_ARG;
    if (metadata) {
        /* Optional protocol settings are still checked without resolving a
         * credential, selecting a provider, or requiring a model. */
        if (o->model_id != NULL &&
            (!token(o->model_id) || !cog_cli_utf8_valid(o->model_id)))
            return COG_ERROR_INVALID_ARG;
        if (o->endpoint_name != NULL &&
            (!token(o->endpoint_name) || !cog_cli_utf8_valid(o->endpoint_name)))
            return COG_ERROR_INVALID_ARG;
        if (o->base_url != NULL && !https_url(o->base_url))
            return COG_ERROR_INVALID_ARG;
        if (o->credential_ref != NULL && strncmp(o->credential_ref, "env:", 4U) == 0 &&
            !environment_name_valid(o->credential_ref + 4))
            return COG_ERROR_INVALID_ARG;
    } else {
        if (o->backend == COG_CLI_BACKEND_MOCK) {
            if (!c->endpoint_present)
                o->endpoint_name = "mock";
            if (o->model_id == NULL)
                o->model_id = "mock-model";
            if (strcmp(o->endpoint_name, "mock") != 0)
                return COG_ERROR_NOT_FOUND;
        } else {
            if (o->model_id == NULL)
                return COG_ERROR_NOT_FOUND;
            if (strcmp(o->endpoint_name, "openrouter") != 0)
                return COG_ERROR_NOT_FOUND;
            if (!https_url(o->base_url))
                return COG_ERROR_INVALID_ARG;
        }
        if (!token(o->model_id) || !cog_cli_utf8_valid(o->model_id) ||
            !token(o->endpoint_name))
            return COG_ERROR_INVALID_ARG;
    }
    if (o->store_mode == COG_CLI_STORE_DURABLE && o->store_dir == NULL) {
        const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        const char *base, *suffix;
        size_t n, base_len;
        if (xdg != NULL && xdg[0] == '/') {
            base = xdg;
            suffix = "/cog";
        } else if (home != NULL && home[0] == '/') {
            base = home;
            suffix = "/.local/state/cog";
        } else
            return COG_ERROR_NOT_FOUND;
        base_len = strlen(base);
        while (base_len > 1U && base[base_len - 1U] == '/')
            --base_len;
        if (base_len == 1U)
            ++suffix;
        n = base_len + strlen(suffix) + 1U;
        if (n > FIELD_BYTES || n > STRING_BYTES - c->used)
            return COG_ERROR_LIMIT;
        o->store_dir = c->strings + c->used;
        memcpy(c->strings + c->used, base, base_len);
        memcpy(c->strings + c->used + base_len, suffix, strlen(suffix) + 1U);
        c->used += n;
    }
    return COG_OK;
}
static cog_result_t credential(cog_cli_configuration_t *c)
{
    cog_cli_options_t *o = &c->options;
    size_t required = 0U, len, i;
    cog_result_t rc;
    if (metadata_action(o->session_action) || o->backend == COG_CLI_BACKEND_MOCK)
        return COG_OK;
    if (o->credential_ref == NULL || *o->credential_ref == '\0')
        return COG_ERROR_NOT_FOUND;
    if (o->credential_read != NULL) {
        rc = o->credential_read(o->credential_arg, o->credential_ref, c->secret,
                                sizeof(c->secret), &required);
        if (rc == COG_ERROR_OVERFLOW)
            return required > sizeof(c->secret) ? COG_ERROR_LIMIT : COG_ERROR_PROTOCOL;
        if (rc != COG_OK)
            return rc;
        if (required == 0U || required > sizeof(c->secret))
            return COG_ERROR_PROTOCOL;
        if (memchr(c->secret, '\0', required) != c->secret + required - 1U)
            return COG_ERROR_PROTOCOL;
    } else {
        const char *name, *s;
        if (strncmp(o->credential_ref, "env:", 4U) != 0)
            return COG_ERROR_UNSUPPORTED;
        name = o->credential_ref + 4;
        if (!environment_name_valid(name))
            return COG_ERROR_INVALID_ARG;
        s = getenv(name);
        if (s == NULL || *s == '\0')
            return COG_ERROR_NOT_FOUND;
        len = strlen(s) + 1U;
        if (len > sizeof(c->secret))
            return COG_ERROR_LIMIT;
        memcpy(c->secret, s, len);
        required = len;
    }
    if (required == 1U)
        return COG_ERROR_NOT_FOUND;
    for (i = 0U; i + 1U < required; ++i)
        if ((unsigned char)c->secret[i] <= 32U || (unsigned char)c->secret[i] == 127U)
            return COG_ERROR_INVALID_ARG;
    return COG_OK;
}
static bool safe_label(const cog_cli_configuration_t *c, const char *input,
    char output[LABEL_BYTES])
{
    if (input == NULL || *input == '\0') return false;
    size_t length = strlen(input);
    if (length >= LABEL_BYTES) return false;
    /* A real credential that cannot be screened in this small label profile
       makes labels unavailable; it never makes the actual configuration fail. */
    if (c->options.backend == COG_CLI_BACKEND_REAL &&
        (c->secret[0] == '\0' || strlen(c->secret) >= LABEL_BYTES)) return false;
    if (c->secret[0] != '\0' && strstr(input, c->secret) != NULL) return false;
    if (c->options.credential_ref != NULL && c->options.credential_ref[0] != '\0' &&
        strstr(input, c->options.credential_ref) != NULL) return false;
    for (size_t i = 0U; i < length; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ':' || ch == '/' || ch == '-'))
            return false;
    }
    memcpy(output, input, length + 1U); return true;
}
static void cache_labels(cog_cli_configuration_t *c)
{
    const char *session = c->options.session_action == COG_CLI_SESSION_NEW ?
        c->options.session_id : c->options.source_session_id;
    c->labels.session_available = safe_label(c, session, c->session_label);
    c->labels.model_available = safe_label(c, c->options.model_id, c->model_label);
    c->labels.session = c->labels.session_available ? c->session_label : NULL;
    c->labels.model = c->labels.model_available ? c->model_label : NULL;
}
/* Both constructors deep-copy this exact set of effective string fields. */
static cog_result_t copy_option_strings(cog_cli_configuration_t *c)
{
    size_t i;
    cog_result_t rc;
    for (i = 0U; i < cog_cli_field_count; ++i)
        if (cog_cli_fields[i].kind == CLI_STRING) {
            unsigned char *field =
                (unsigned char *)&c->options + cog_cli_fields[i].offset;
            const char *string;
            memcpy(&string, field, sizeof(string));
            /* Default store path was already copied into this arena. */
            if (cog_cli_fields[i].bit == COG_CLI_OPT_STORE_DIR &&
                c->options.store_dir == c->strings)
                continue;
            rc = owned(c, &string);
            memcpy(field, &string, sizeof(string));
            if (rc != COG_OK)
                return rc;
        }
#define OWN(member)                                                                    \
    do {                                                                               \
        rc = owned(c, &c->options.member);                                             \
        if (rc != COG_OK)                                                              \
            return rc;                                                                 \
    } while (0)
    OWN(prompt);
    OWN(session_id);
    OWN(source_session_id);
    OWN(config_path);
    OWN(log_path);
    OWN(socket_path);
    OWN(chunks);
    OWN(queue_text);
#undef OWN
    return COG_OK;
}
cog_result_t cog_cli_configuration_new(cog_cli_configuration_t **out,
                                       const cog_cli_options_t *input,
                                       uint64_t generation)
{
    cog_cli_configuration_t *c;
    cog_result_t rc;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (input == NULL)
        return COG_ERROR_NULL_PTR;
    if (generation == 0U)
        return COG_ERROR_INVALID_ARG;
    rc = cog_cli_session_options_validate(input);
    if (rc != COG_OK)
        return rc;
    c = calloc(1U, sizeof(*c));
    if (c == NULL)
        return COG_ERROR_NOMEM;
    cog_cli_options_defaults(&c->options);
    c->generation = generation;
    rc = file_options(c, input->config_path);
    if (rc != COG_OK)
        goto fail;
    rc = overlay(&c->options, input);
    if (rc != COG_OK)
        goto fail;
    if ((input->explicit_options & COG_CLI_OPT_ENDPOINT) != 0U ||
        getenv("COG_ENDPOINT") != NULL)
        c->endpoint_present = true;
    c->options.prompt = input->prompt;
    c->options.session_id = input->session_id;
    c->options.session_action = input->session_action;
    c->options.source_session_id = input->source_session_id;
    c->options.fork_node_id = input->fork_node_id;
    c->options.previous_processes_fenced = input->previous_processes_fenced;
    c->options.show_usage = input->show_usage;
    c->options.config_path = input->config_path;
    c->options.log_path = input->log_path;
    c->options.socket_path = input->socket_path;
    c->options.chunks = input->chunks;
    c->options.queue_text = input->queue_text;
    c->options.mock_mode = input->mock_mode;
    c->options.script = input->script;
    c->options.mock_deltas = input->mock_deltas;
    c->options.mock_delta_count = input->mock_delta_count;
    c->options.interactive = input->interactive;
    c->options.verbose = input->verbose;
    c->options.net = metadata_action(input->session_action) ? NULL : input->net;
    c->options.credential_read = input->credential_read;
    c->options.credential_arg = input->credential_arg;
    rc = validate(c);
    if (rc != COG_OK)
        goto fail;
    rc = copy_option_strings(c);
    if (rc != COG_OK)
        goto fail;
    rc = credential(c);
    if (rc != COG_OK)
        goto fail;
    /* Resolver authority is construction-only; neither callback nor context is
     * retained for later service or exposed through the effective options. */
    c->options.credential_read = NULL;
    c->options.credential_arg = NULL;
    clear(c->file, sizeof(c->file));
    c->tool_mask = c->options.store_mode == COG_CLI_STORE_DURABLE &&
        c->options.workspace_root != NULL ? COG_CLI_CHILD_ALL : 0U;
    cache_labels(c);
    *out = c;
    return COG_OK;
fail:
    cog_cli_configuration_free(c);
    return rc;
}
cog_result_t cog_cli_configuration_clone_new(cog_cli_configuration_t **out,
    const cog_cli_configuration_t *current, cog_net_ctx_t *net)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (current == NULL) return COG_ERROR_NULL_PTR;
    if (net != NULL && (current->options.backend == COG_CLI_BACKEND_MOCK ||
        metadata_action(current->options.session_action))) return COG_ERROR_INVALID_ARG;
    cog_cli_configuration_t *c = calloc(1U, sizeof(*c));
    if (c == NULL) return COG_ERROR_NOMEM;
    c->options = current->options;
    c->options.net = net;
    c->options.credential_read = NULL; c->options.credential_arg = NULL;
    c->generation = current->generation; c->tool_mask = current->tool_mask;
    c->resolved_endpoint = current->resolved_endpoint;
    c->endpoint_present = current->endpoint_present; c->origin = ORIGIN_CLONE;
    cog_result_t rc = copy_option_strings(c);
    if (rc != COG_OK) { cog_cli_configuration_free(c); return rc; }
    memcpy(c->secret, current->secret, sizeof(c->secret));
    if (current->instruction_attached) {
        rc = cli_configuration_instruction_attach(c, &current->instructions);
        if (rc != COG_OK) { cog_cli_configuration_free(c); return rc; }
    }
    cache_labels(c);
    *out = c;
    return COG_OK;
}
cog_result_t cog_cli_configuration_reload_new(
    cog_cli_configuration_t **out, const cog_cli_configuration_t *current,
    const cog_cli_reload_options_t *replacement, uint64_t generation)
{
    cog_cli_configuration_t *c;
    cog_result_t rc;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (current == NULL || replacement == NULL)
        return COG_ERROR_NULL_PTR;
    if (generation == 0U || generation == current->generation ||
        metadata_action(current->options.session_action) ||
        (replacement->model_id == NULL && replacement->base_url == NULL &&
         replacement->credential_ref == NULL) ||
        (replacement->credential_ref == NULL &&
         (replacement->credential_read != NULL || replacement->credential_arg != NULL)))
        return COG_ERROR_INVALID_ARG;
    if (current->options.backend == COG_CLI_BACKEND_MOCK &&
        (replacement->base_url != NULL || replacement->credential_ref != NULL))
        return COG_ERROR_UNSUPPORTED;
    c = calloc(1U, sizeof(*c));
    if (c == NULL)
        return COG_ERROR_NOMEM;
    /* This is resolved state, not a second invocation of startup precedence.
     * The fresh zeroed owner has no endpoint/model/adapter aliases into current. */
    c->options = current->options;
    c->generation = generation;
    c->endpoint_present = current->endpoint_present;
    c->origin = ORIGIN_RELOAD;
    c->tool_mask = current->tool_mask;
    c->options.credential_read = NULL;
    c->options.credential_arg = NULL;
    if (replacement->model_id != NULL)
        c->options.model_id = replacement->model_id;
    if (replacement->base_url != NULL)
        c->options.base_url = replacement->base_url;
    if (replacement->credential_ref != NULL)
        c->options.credential_ref = replacement->credential_ref;
    /* Copy first to enforce both per-field and aggregate J byte ceilings before
     * protocol validation or an explicit resolver can observe the candidate. */
    rc = copy_option_strings(c);
    if (rc != COG_OK)
        goto fail;
    if (!token(c->options.model_id) || !cog_cli_utf8_valid(c->options.model_id) ||
        (c->options.backend == COG_CLI_BACKEND_REAL &&
         !https_url(c->options.base_url)) ||
        (replacement->credential_ref != NULL && *c->options.credential_ref == '\0')) {
        rc = COG_ERROR_INVALID_ARG;
        goto fail;
    }
    if (replacement->credential_ref == NULL) {
        memcpy(c->secret, current->secret, sizeof(c->secret));
    } else {
        c->options.credential_read = replacement->credential_read;
        c->options.credential_arg = replacement->credential_arg;
        rc = credential(c);
        c->options.credential_read = NULL;
        c->options.credential_arg = NULL;
        if (rc != COG_OK)
            goto fail;
    }
    cache_labels(c);
    *out = c;
    return COG_OK;
fail:
    cog_cli_configuration_free(c);
    return rc;
}
void cog_cli_configuration_free(cog_cli_configuration_t *c)
{
    if (c == NULL)
        return;
    if (c->instructions.bytes != NULL) {
        clear((void *)c->instructions.bytes, c->instructions.length);
        free((void *)c->instructions.bytes);
    }
    clear(c, sizeof(*c));
    free(c);
}
const cog_cli_options_t *cog_cli_configuration_options(const cog_cli_configuration_t *c)
{
    return c != NULL ? &c->options : NULL;
}
const cog_cli_safe_labels_t *cog_cli_configuration_labels(const cog_cli_configuration_t *c)
{
    return c != NULL ? &c->labels : NULL;
}
cog_result_t cog_cli_configuration_bind(cog_cli_configuration_t *c,
                                        const cog_provider_vtable_t *real,
                                        const cog_provider_vtable_t *mock,
                                        const cog_mock_script_t *script,
                                        const cog_provider_limits_t *limits)
{
    const cog_provider_vtable_t *vtable;
    if (c == NULL)
        return COG_ERROR_NULL_PTR;
    if (c->resolved_endpoint != NULL) return COG_OK;
    if (c->bound || metadata_action(c->options.session_action))
        return COG_ERROR_INVALID_STATE;
    if (c->options.backend == COG_CLI_BACKEND_REAL) {
        if (real == NULL || c->options.net == NULL || limits == NULL)
            return COG_ERROR_NULL_PTR;
#define NONZERO(member)                                                                \
    if (limits->member == 0U)                                                          \
    return COG_ERROR_INVALID_ARG
        NONZERO(request_bytes);
        NONZERO(parser_bytes);
        NONZERO(writer_bytes);
        NONZERO(max_json_depth);
        NONZERO(max_frame_bytes);
        NONZERO(retained_delta_bytes);
        NONZERO(retained_delta_count);
        NONZERO(max_text_bytes);
        NONZERO(max_tool_calls);
        NONZERO(max_tool_id_bytes);
        NONZERO(max_tool_name_bytes);
        NONZERO(max_tool_arguments_bytes);
        NONZERO(max_opaque_bytes);
#undef NONZERO
        c->adapter.net = c->options.net;
        c->adapter.limits = *limits;
        c->adapter.service_quantum = 16U;
        vtable = real;
    } else {
        if (mock == NULL || script == NULL)
            return COG_ERROR_NULL_PTR;
        vtable = mock;
    }
    c->model.id = c->options.model_id;
    c->model.name = c->options.model_id;
    c->model.capabilities = COG_MODEL_CAPABILITY_STREAMING;
    if (c->options.store_mode == COG_CLI_STORE_DURABLE &&
        (c->options.workspace_root != NULL || c->instruction_attached))
        c->model.capabilities |= COG_MODEL_CAPABILITY_TOOLS;
    c->models.models = &c->model;
    c->models.count = 1U;
    c->endpoint.name = c->options.endpoint_name;
    c->endpoint.base_url = c->options.base_url;
    c->endpoint.api_key = c->options.backend == COG_CLI_BACKEND_REAL ? c->secret : NULL;
    c->endpoint.vtable = vtable;
    c->endpoint.models = &c->models;
    c->endpoint.implementation_config = c->options.backend == COG_CLI_BACKEND_REAL
                                            ? (const void *)&c->adapter
                                            : (const void *)script;
    c->endpoint.generation = c->generation;
    c->bound = true;
    return COG_OK;
}
const cog_endpoint_t *cog_cli_configuration_endpoint(const cog_cli_configuration_t *c)
{
    return c != NULL && c->resolved_endpoint != NULL ? c->resolved_endpoint : c != NULL && c->bound ? &c->endpoint : NULL;
}
cog_result_t cog_cli_configuration_describe(const cog_cli_configuration_t *c,
                                            char *output, size_t capacity,
                                            size_t *required)
{
    char text[160];
    int n;
    if (c == NULL || required == NULL || (capacity != 0U && output == NULL))
        return COG_ERROR_NULL_PTR;
    n = snprintf(text, sizeof(text),
                 "backend=%s store=%s tools=%s credential=%s generation=%llu",
                 c->options.backend == COG_CLI_BACKEND_REAL ? "real" : "mock",
                 c->options.store_mode == COG_CLI_STORE_DURABLE ? "durable"
                                                                : "disabled",
                 !metadata_action(c->options.session_action) &&
                         c->options.store_mode == COG_CLI_STORE_DURABLE &&
                         c->options.workspace_root != NULL
                     ? "policy-controlled"
                     : "disabled",
                 !metadata_action(c->options.session_action) &&
                         c->options.backend == COG_CLI_BACKEND_REAL
                     ? "[REDACTED]"
                     : "none",
                 (unsigned long long)c->generation);
    if (n < 0 || (size_t)n >= sizeof(text))
        return COG_ERROR_LIMIT;
    *required = (size_t)n + 1U;
    if (capacity < *required)
        return COG_ERROR_OVERFLOW;
    memcpy(output, text, *required);
    return COG_OK;
}

/* Pure lexical paths: directory identity is the host's independently checked
 * precondition. Keep this separate from startup's permissive user path syntax. */
static bool canonical_path(const char *s)
{
    const char *p, *start;
    if (s == NULL || s[0] != '/' || !cog_cli_utf8_valid(s)) return false;
    if (s[1] == '\0') return true;
    start = s + 1;
    for (p = start;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        if (p == start || (p - start == 1 && start[0] == '.') ||
            (p - start == 2 && start[0] == '.' && start[1] == '.')) return false;
        if (*p == '\0') return true;
        start = p + 1;
    }
}
cog_result_t cog_cli_configuration_finish_host(cog_cli_configuration_t *c,
    cog_net_ctx_t *net, const char *workspace, const char *store)
{
    size_t workspace_bytes, store_bytes;
    if (c == NULL) return COG_ERROR_NULL_PTR;
    if (c->origin != ORIGIN_STARTUP || c->bound || c->host_finished ||
        c->options.net != NULL || metadata_action(c->options.session_action) ||
        c->options.store_mode != COG_CLI_STORE_DURABLE) return COG_ERROR_INVALID_STATE;
    if (store == NULL || (c->options.workspace_root != NULL && workspace == NULL))
        return COG_ERROR_NULL_PTR;
    if ((workspace != NULL) != (c->options.workspace_root != NULL) ||
        (c->options.backend == COG_CLI_BACKEND_REAL ? net == NULL :
         c->options.backend != COG_CLI_BACKEND_MOCK || net != NULL) ||
        !canonical_path(store) || (workspace != NULL && !canonical_path(workspace)))
        return COG_ERROR_INVALID_ARG;
    store_bytes = strlen(store) + 1U;
    workspace_bytes = workspace != NULL ? strlen(workspace) + 1U : 0U;
    if (store_bytes > FIELD_BYTES || workspace_bytes > FIELD_BYTES ||
        workspace_bytes > STRING_BYTES - c->used ||
        store_bytes > STRING_BYTES - c->used - workspace_bytes) return COG_ERROR_LIMIT;
    if (workspace != NULL) {
        memcpy(c->strings + c->used, workspace, workspace_bytes);
        c->options.workspace_root = c->strings + c->used;
        c->used += workspace_bytes;
    }
    memcpy(c->strings + c->used, store, store_bytes);
    c->options.store_dir = c->strings + c->used;
    c->used += store_bytes;
    c->options.net = net;
    c->host_finished = true;
    return COG_OK;
}
cog_result_t cog_cli_configuration_tool_mask(const cog_cli_configuration_t *c, uint32_t *mask)
{
    if (c == NULL || mask == NULL) return COG_ERROR_NULL_PTR;
    *mask = c->tool_mask;
    return COG_OK;
}
static void child_inputs(cog_cli_options_t *o)
{
    o->prompt = NULL; o->queue_text = NULL; o->socket_path = NULL;
    o->session_id = NULL; o->source_session_id = NULL;
    o->session_action = COG_CLI_SESSION_NEW; o->fork_node_id = 0U;
    o->previous_processes_fenced = false; o->show_usage = false;
    o->config_path = NULL; o->log_path = NULL; o->interactive = false;
    o->credential_read = NULL; o->credential_arg = NULL;
    o->chunks = NULL; o->script = NULL; o->mock_deltas = NULL; o->mock_delta_count = 0U;
}
static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
cog_result_t cog_cli_configuration_child_new(cog_cli_configuration_t **out,
    const cog_cli_configuration_t *parent, const cog_cli_child_profile_t *profile,
    uint64_t generation, uint64_t now_ms, uint64_t parent_deadline_ms,
    cog_net_ctx_t *net, uint64_t *deadline_ms)
{
    cog_cli_configuration_t *c;
    uint64_t remaining, deadline;
    cog_result_t rc;
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (parent == NULL || profile == NULL || net == NULL || deadline_ms == NULL)
        return COG_ERROR_NULL_PTR;
    if (generation == 0U || generation != parent->generation ||
        parent_deadline_ms == UINT64_MAX || profile->policy < COG_CLI_CHILD_POLICY_INHERIT ||
        profile->policy > COG_CLI_CHILD_POLICY_ASK || (profile->tools & ~15U) != 0U ||
        metadata_action(parent->options.session_action) ||
        parent->options.store_mode != COG_CLI_STORE_DURABLE) return COG_ERROR_INVALID_ARG;
    if (parent->options.backend != COG_CLI_BACKEND_REAL) return COG_ERROR_UNSUPPORTED;
    if (parent_deadline_ms <= now_ms) return COG_ERROR_TIMEOUT;
    remaining = parent_deadline_ms - now_ms;
    if (profile->timeout_ms != 0U && remaining > profile->timeout_ms)
        remaining = profile->timeout_ms;
    if (remaining > parent->options.max_turn_ms) remaining = parent->options.max_turn_ms;
    deadline = now_ms + remaining; /* bounded by the finite, checked parent deadline */
    c = calloc(1U, sizeof(*c));
    if (c == NULL) return COG_ERROR_NOMEM;
    c->options = parent->options;
    c->generation = generation; c->origin = ORIGIN_CHILD;
    c->endpoint_present = parent->endpoint_present;
    c->tool_mask = parent->tool_mask & profile->tools;
    child_inputs(&c->options); c->options.net = net;
    if (parent->options.tool_policy == COG_CLI_TOOL_POLICY_DENY ||
        profile->policy == COG_CLI_CHILD_POLICY_DENY) c->options.tool_policy = COG_CLI_TOOL_POLICY_DENY;
    else if (profile->policy == COG_CLI_CHILD_POLICY_ASK) c->options.tool_policy = COG_CLI_TOOL_POLICY_ASK;
#define NARROW_U32(member) if (profile->member != 0U) c->options.member = min_u32(c->options.member, profile->member)
    NARROW_U32(max_output_tokens);
    NARROW_U32(max_tool_rounds);
#undef NARROW_U32
    c->options.max_retries = min_u32(c->options.max_retries, profile->max_retries);
    if (profile->max_context_bytes != 0U)
        c->options.max_context_bytes = min_size(c->options.max_context_bytes, profile->max_context_bytes);
    if (profile->max_tool_output_bytes != 0U)
        c->options.max_tool_output_bytes = min_size(c->options.max_tool_output_bytes, profile->max_tool_output_bytes);
    c->options.max_turn_ms = (uint32_t)remaining;
    c->options.max_tool_ms = min_u32(c->options.max_tool_ms, c->options.max_turn_ms);
    c->options.max_retry_elapsed_ms = min_u32(c->options.max_retry_elapsed_ms, c->options.max_turn_ms);
    rc = copy_option_strings(c);
    if (rc != COG_OK) { cog_cli_configuration_free(c); return rc; }
    memcpy(c->secret, parent->secret, sizeof(c->secret));
    cache_labels(c); *deadline_ms = deadline; *out = c;
    return COG_OK;
}

/* One borrowed view serves both wire directions. No decoder writes caller output
 * until every later field, numeric predicate and capacity has been validated. */
enum { B_MODEL, B_ENDPOINT, B_URL, B_SECRET, B_REFERENCE, B_WORKSPACE, B_STORE,
       B_SESSION, B_CA, B_FIELDS };
typedef struct bootstrap_view {
    const unsigned char *field[B_FIELDS];
    size_t length[B_FIELDS];
    uint64_t generation;
    uint32_t mask;
    cog_cli_options_t options;
    const cog_endpoint_t *resolved_endpoint;
    bool instruction_attached;
    cog_cli_instruction_capsule_t instructions;
    cog_cli_bootstrap_probe_t probe;
    cog_net_config_t transport;
} bootstrap_view_t;
typedef struct bootstrap_reader { const unsigned char *p; size_t left; } bootstrap_reader_t;
static bool read_number(bootstrap_reader_t *r, size_t bytes, uint64_t *value)
{
    uint64_t result = 0U;
    if (r->left < bytes) return false;
    for (size_t i = 0U; i < bytes; ++i) result = (result << 8U) | r->p[i];
    r->p += bytes; r->left -= bytes; *value = result; return true;
}
static bool read_field(bootstrap_reader_t *r, bootstrap_view_t *v, size_t field)
{
    uint64_t len;
    if (!read_number(r, 4U, &len) || len > r->left) return false;
    v->field[field] = r->p; v->length[field] = (size_t)len;
    r->p += (size_t)len; r->left -= (size_t)len; return true;
}
static cog_result_t bootstrap_valid(const bootstrap_view_t *v)
{
    char text[FIELD_BYTES];
    size_t aggregate = 0U;
    bool unsupported_endpoint = false;
    const cog_cli_options_t *o = &v->options;
    const cog_net_config_t *n = &v->transport;
    if (v->generation == 0U || v->mask > 15U ||
        (v->length[B_WORKSPACE] == 0U && v->mask != 0U) ||
        o->tool_policy < COG_CLI_TOOL_POLICY_ASK || o->tool_policy > COG_CLI_TOOL_POLICY_ALLOW ||
        o->max_output_tokens == 0U || o->max_tool_rounds == 0U || o->max_turn_ms == 0U ||
        o->max_tool_ms == 0U || o->max_tool_output_bytes == 0U || o->max_context_bytes == 0U ||
        o->max_retry_elapsed_ms == 0U || v->probe.owner.epoch == 0U ||
        v->probe.owner.instance_id == 0U || v->probe.checkpoint.generation == 0U ||
        n->max_connections == 0U || n->connect_timeout_ms == 0U || n->stall_timeout_ms == 0U ||
        n->max_request_bytes == 0U || n->max_header_bytes == 0U || n->chunk_bytes == 0U ||
        n->chunks_per_request == 0U || n->service_quantum == 0U) return COG_ERROR_PROTOCOL;
    if (o->max_context_bytes > (SIZE_MAX - 65536U) / 32U ||
        o->max_tool_output_bytes > SIZE_MAX - 512U ||
        n->max_connections > (size_t)LONG_MAX || n->max_request_bytes == SIZE_MAX ||
        n->max_header_bytes == SIZE_MAX || n->chunks_per_request == SIZE_MAX ||
        n->chunks_per_request + 1U > SIZE_MAX / n->chunk_bytes ||
        n->chunks_per_request + 1U > SIZE_MAX / (n->max_header_bytes + 1U)) return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < B_FIELDS; ++i) {
        size_t len = v->length[i];
        if (i == B_SECRET) {
            if (len == 0U) return COG_ERROR_PROTOCOL;
            if (len >= SECRET_BYTES) return COG_ERROR_LIMIT;
            for (size_t j = 0U; j < len; ++j)
                if (v->field[i][j] <= 32U || v->field[i][j] == 127U) return COG_ERROR_PROTOCOL;
            continue;
        }
        if (len >= FIELD_BYTES || (i == B_SESSION && len > 127U)) return COG_ERROR_LIMIT;
        if (len == 0U && i != B_WORKSPACE && i != B_CA) return COG_ERROR_PROTOCOL;
        if (memchr(v->field[i], '\0', len) != NULL) return COG_ERROR_PROTOCOL;
        aggregate += len + 1U;
        if (aggregate > STRING_BYTES) return COG_ERROR_LIMIT;
        memcpy(text, v->field[i], len); text[len] = '\0';
        if (!cog_cli_utf8_valid(text)) return COG_ERROR_PROTOCOL;
        if ((i == B_MODEL || i == B_ENDPOINT) && !token(text)) return COG_ERROR_PROTOCOL;
        if (i == B_ENDPOINT && strcmp(text, "openrouter") != 0) unsupported_endpoint = true;
        if (i == B_URL && !https_url(text)) return COG_ERROR_PROTOCOL;
        if (i == B_REFERENCE && strncmp(text, "env:", 4U) == 0 &&
            !environment_name_valid(text + 4)) return COG_ERROR_PROTOCOL;
        if ((i == B_WORKSPACE && len != 0U) || i == B_STORE)
            if (!canonical_path(text)) return COG_ERROR_PROTOCOL;
        if (i == B_SESSION) {
            cog_cli_options_t selector = {0};
            /* Probe identity is an EXISTING durable session. NEW validates
               only selector conflicts, so use the existing G/L source-ID path. */
            selector.session_action = COG_CLI_SESSION_RESUME;
            selector.source_session_id = text;
            if (cog_cli_session_options_validate(&selector) != COG_OK) return COG_ERROR_PROTOCOL;
        }
    }
    return unsupported_endpoint ? COG_ERROR_UNSUPPORTED : COG_OK;
}
/* Validate full outer framing/hash allocation-free, then borrow validated slices.
 * The measurement decoder validates before returning OVERFLOW. */
static cog_result_t bootstrap_unwrap(const void **input, size_t *length,
    cog_cli_instruction_capsule_t *capsule, bool *wrapped)
{
    *wrapped = false;
    *capsule = (cog_cli_instruction_capsule_t){0};
    if (*length < 4U || memcmp(*input, "CGB2", 4U) != 0) return COG_OK;
    cog_cli_instruction_bootstrap_t ignored;
    size_t required = 0;
    cog_result_t rc = cog_cli_instruction_bootstrap_decode(*input, *length,
        &ignored, NULL, 0U, &required);
    if (rc != COG_ERROR_OVERFLOW) return rc == COG_OK ? COG_ERROR_PROTOCOL : rc;
    bootstrap_reader_t r = {(const unsigned char *)*input + 4U, *length - 4U};
    uint64_t n = 0;
    if (!read_number(&r, 4U, &n) || n > r.left) return COG_ERROR_PROTOCOL;
    const void *configuration = r.p;
    size_t configuration_bytes = (size_t)n;
    r.p += configuration_bytes; r.left -= configuration_bytes;
    if (!read_number(&r, 8U, &capsule->identity.catalog_generation) ||
        !read_number(&r, 8U, &capsule->identity.snapshot_id) || r.left < 32U)
        return COG_ERROR_PROTOCOL;
    memcpy(capsule->identity.sha256, r.p, 32U); r.p += 32U; r.left -= 32U;
    if (!read_number(&r, 4U, &n) || n != r.left) return COG_ERROR_PROTOCOL;
    capsule->length = (size_t)n; capsule->bytes = n ? r.p : NULL;
    *input = configuration; *length = configuration_bytes; *wrapped = true;
    return COG_OK;
}
static cog_result_t bootstrap_read(const void *input, size_t length, bootstrap_view_t *v)
{
    bootstrap_reader_t r;
    uint64_t number;
    if (length < 4U) return COG_ERROR_PROTOCOL;
    if (memcmp(input, "CGB1", 4U) != 0)
        return memcmp(input, "CGB", 3U) == 0 ? COG_ERROR_UNSUPPORTED : COG_ERROR_PROTOCOL;
    memset(v, 0, sizeof(*v)); r.p = (const unsigned char *)input + 4U; r.left = length - 4U;
#define READ(bytes, target, type) do { if (!read_number(&r, bytes, &number)) return COG_ERROR_PROTOCOL; target = (type)number; } while (0)
#define READ_SIZE(target) do { if (!read_number(&r, 8U, &number)) return COG_ERROR_PROTOCOL; if (number > SIZE_MAX) return COG_ERROR_LIMIT; target = (size_t)number; } while (0)
    READ(8U, v->generation, uint64_t);
    READ(1U, v->options.tool_policy, cog_cli_tool_policy_t);
    READ(1U, v->mask, uint32_t);
    READ(4U, v->options.max_output_tokens, uint32_t);
    READ(4U, v->options.max_tool_rounds, uint32_t);
    READ(4U, v->options.max_turn_ms, uint32_t);
    READ(4U, v->options.max_tool_ms, uint32_t);
    READ_SIZE(v->options.max_tool_output_bytes);
    READ_SIZE(v->options.max_context_bytes);
    READ(4U, v->options.max_retries, uint32_t);
    READ(4U, v->options.max_retry_elapsed_ms, uint32_t);
    for (size_t i = B_MODEL; i <= B_SESSION; ++i)
        if (!read_field(&r, v, i)) return COG_ERROR_PROTOCOL;
    READ(8U, v->probe.owner.epoch, uint64_t);
    READ(8U, v->probe.owner.instance_id, uint64_t);
    READ(8U, v->probe.checkpoint.generation, uint64_t);
    READ(4U, v->transport.max_connections, size_t);
    READ(4U, v->transport.connect_timeout_ms, uint32_t);
    READ(4U, v->transport.stall_timeout_ms, uint32_t);
    READ_SIZE(v->transport.max_request_bytes);
    READ_SIZE(v->transport.max_header_bytes);
    READ_SIZE(v->transport.chunk_bytes);
    READ(4U, v->transport.chunks_per_request, size_t);
    READ(4U, v->transport.service_quantum, uint32_t);
#undef READ
#undef READ_SIZE
    if (!read_field(&r, v, B_CA) || r.left != 0U) return COG_ERROR_PROTOCOL;
    return bootstrap_valid(v);
}
static void write_number(unsigned char **p, uint64_t number, size_t bytes)
{
    for (size_t i = bytes; i != 0U; --i) { (*p)[i - 1U] = (unsigned char)(number & 255U); number >>= 8U; }
    *p += bytes;
}
static void write_field(unsigned char **p, const bootstrap_view_t *v, size_t field)
{
    write_number(p, v->length[field], 4U);
    if (v->length[field] != 0U) memcpy(*p, v->field[field], v->length[field]);
    *p += v->length[field];
}
cog_result_t cog_cli_configuration_bootstrap_encode(const cog_cli_configuration_t *c,
    const cog_cli_bootstrap_probe_t *probe, const cog_net_config_t *transport,
    void *output, size_t capacity, size_t *required)
{
    bootstrap_view_t v = {0};
    const char *fields[B_FIELDS];
    cog_result_t rc;
    size_t bytes = 158U; /* fixed scalar bytes + all nine length prefixes */
    size_t total;
    unsigned char *p = output;
    if (c == NULL || probe == NULL || transport == NULL || required == NULL ||
        (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (c->options.backend != COG_CLI_BACKEND_REAL ||
        c->options.store_mode != COG_CLI_STORE_DURABLE || metadata_action(c->options.session_action))
        return COG_ERROR_UNSUPPORTED;
    if (!transport->host_runtime_initialized ||
        transport->dependency_memory != COG_NET_DEPENDENCY_MEMORY_DYNAMIC ||
        transport->dependency_memory_limit != 0U || transport->max_retries != 0U ||
        transport->retry_backoff_ms != 0U) return COG_ERROR_UNSUPPORTED;
    if (memchr(probe->session_id, '\0', sizeof(probe->session_id)) == NULL) return COG_ERROR_INVALID_ARG;
    fields[B_MODEL] = c->options.model_id; fields[B_ENDPOINT] = c->options.endpoint_name;
    fields[B_URL] = c->options.base_url; fields[B_SECRET] = c->secret;
    fields[B_REFERENCE] = c->options.credential_ref; fields[B_WORKSPACE] = c->options.workspace_root;
    fields[B_STORE] = c->options.store_dir; fields[B_SESSION] = probe->session_id;
    fields[B_CA] = transport->ca_bundle;
    v.generation = c->generation; v.mask = c->tool_mask; v.options = c->options;
    v.probe = *probe; v.transport = *transport;
#define DEFAULT(member, value) if (v.transport.member == 0U) v.transport.member = value
    DEFAULT(max_connections, 4U); DEFAULT(connect_timeout_ms, 10000U); DEFAULT(stall_timeout_ms, 30000U);
    DEFAULT(max_request_bytes, 1048576U); DEFAULT(max_header_bytes, 65536U); DEFAULT(chunk_bytes, 16384U);
    DEFAULT(chunks_per_request, 8U); DEFAULT(service_quantum, 16U);
#undef DEFAULT
    if (v.transport.max_connections > UINT32_MAX || v.transport.chunks_per_request > UINT32_MAX)
        return COG_ERROR_LIMIT;
    for (size_t i = 0U; i < B_FIELDS; ++i) {
        v.field[i] = (const unsigned char *)(fields[i] != NULL ? fields[i] : "");
        v.length[i] = strlen((const char *)v.field[i]);
        if (v.length[i] > STRING_BYTES) return COG_ERROR_LIMIT;
        bytes += v.length[i];
    }
    rc = bootstrap_valid(&v);
    if (rc != COG_OK) return rc == COG_ERROR_PROTOCOL ? COG_ERROR_INVALID_ARG : rc;
    total = bytes;
    if (c->instruction_attached) {
        if (bytes > UINT32_MAX || c->instructions.length > UINT32_MAX ||
            bytes > SIZE_MAX - 60U || c->instructions.length > SIZE_MAX - bytes - 60U)
            return COG_ERROR_LIMIT;
        total += 60U + c->instructions.length;
    }
    if (capacity < total) { *required = total; return COG_ERROR_OVERFLOW; }
    if (c->instruction_attached) {
        memcpy(p, "CGB2", 4U); p += 4U; write_number(&p, bytes, 4U);
    }
    memcpy(p, "CGB1", 4U); p += 4;
#define WRITE(member, width) write_number(&p, (uint64_t)(member), width)
    WRITE(v.generation, 8U); WRITE(v.options.tool_policy, 1U); WRITE(v.mask, 1U);
    WRITE(v.options.max_output_tokens, 4U); WRITE(v.options.max_tool_rounds, 4U);
    WRITE(v.options.max_turn_ms, 4U); WRITE(v.options.max_tool_ms, 4U);
    WRITE(v.options.max_tool_output_bytes, 8U); WRITE(v.options.max_context_bytes, 8U);
    WRITE(v.options.max_retries, 4U); WRITE(v.options.max_retry_elapsed_ms, 4U);
    for (size_t i = B_MODEL; i <= B_SESSION; ++i) write_field(&p, &v, i);
    WRITE(v.probe.owner.epoch, 8U); WRITE(v.probe.owner.instance_id, 8U); WRITE(v.probe.checkpoint.generation, 8U);
    WRITE(v.transport.max_connections, 4U); WRITE(v.transport.connect_timeout_ms, 4U);
    WRITE(v.transport.stall_timeout_ms, 4U); WRITE(v.transport.max_request_bytes, 8U);
    WRITE(v.transport.max_header_bytes, 8U); WRITE(v.transport.chunk_bytes, 8U);
    WRITE(v.transport.chunks_per_request, 4U); WRITE(v.transport.service_quantum, 4U);
#undef WRITE
    write_field(&p, &v, B_CA);
    if (c->instruction_attached) {
        write_number(&p, c->instructions.identity.catalog_generation, 8U);
        write_number(&p, c->instructions.identity.snapshot_id, 8U);
        memcpy(p, c->instructions.identity.sha256, 32U); p += 32U;
        write_number(&p, c->instructions.length, 4U);
        if (c->instructions.length) memcpy(p, c->instructions.bytes, c->instructions.length);
    }
    *required = total;
    return COG_OK;
}
cog_result_t cog_cli_bootstrap_transport_decode(const void *input, size_t input_len,
    cog_net_config_t *transport, char *storage, size_t capacity, size_t *required)
{
    bootstrap_view_t v;
    size_t bytes;
    cog_result_t rc;
    if (input == NULL || transport == NULL || required == NULL ||
        (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    cog_cli_instruction_capsule_t capsule;
    bool wrapped;
    rc = bootstrap_unwrap(&input, &input_len, &capsule, &wrapped);
    if (rc != COG_OK) return rc;
    rc = bootstrap_read(input, input_len, &v);
    if (rc != COG_OK) return rc;
    bytes = v.length[B_CA] != 0U ? v.length[B_CA] + 1U : 0U;
    if (capacity < bytes) { *required = bytes; return COG_ERROR_OVERFLOW; }
    if (bytes != 0U) {
        memcpy(storage, v.field[B_CA], bytes - 1U); storage[bytes - 1U] = '\0';
        v.transport.ca_bundle = storage;
    }
    *transport = v.transport; *required = bytes;
    return COG_OK;
}
cog_result_t cog_cli_configuration_bootstrap_new(cog_cli_configuration_t **out,
    const void *input, size_t input_len, cog_net_ctx_t *net, cog_cli_bootstrap_probe_t *probe)
{
    bootstrap_view_t v;
    cog_cli_configuration_t *c;
    cog_result_t rc;
    const char **destinations[7];
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (input == NULL || net == NULL || probe == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_instruction_capsule_t capsule;
    bool wrapped;
    rc = bootstrap_unwrap(&input, &input_len, &capsule, &wrapped);
    if (rc != COG_OK) return rc;
    rc = bootstrap_read(input, input_len, &v);
    if (rc != COG_OK) return rc;
    c = calloc(1U, sizeof(*c));
    if (c == NULL) return COG_ERROR_NOMEM;
    c->options = v.options; c->options.net = net;
    c->options.backend = COG_CLI_BACKEND_REAL; c->options.store_mode = COG_CLI_STORE_DURABLE;
    c->origin = ORIGIN_BOOTSTRAP; c->generation = v.generation;
    c->tool_mask = v.mask; c->endpoint_present = true;
    destinations[0] = &c->options.model_id; destinations[1] = &c->options.endpoint_name;
    destinations[2] = &c->options.base_url; destinations[3] = NULL;
    destinations[4] = &c->options.credential_ref; destinations[5] = &c->options.workspace_root;
    destinations[6] = &c->options.store_dir;
    for (size_t i = B_MODEL; i <= B_STORE; ++i) {
        if (i == B_SECRET) {
            memcpy(c->secret, v.field[i], v.length[i]);
        } else if (v.length[i] != 0U) {
            memcpy(c->strings + c->used, v.field[i], v.length[i]);
            *destinations[i] = c->strings + c->used;
            c->used += v.length[i] + 1U;
        }
    }
    memcpy(v.probe.session_id, v.field[B_SESSION], v.length[B_SESSION]);
    v.probe.session_id[v.length[B_SESSION]] = '\0';
    if (wrapped) {
        rc = cli_configuration_instruction_attach(c, &capsule);
        if (rc != COG_OK) { cog_cli_configuration_free(c); return rc; }
    }
    cache_labels(c); *probe = v.probe; *out = c;
    return COG_OK;
}

cog_result_t cli_configuration_runtime_paths(cog_cli_configuration_t *c,
    const char *workspace,const char *store)
{
    if(c==NULL || store==NULL)return COG_ERROR_NULL_PTR;
    if(c->origin!=ORIGIN_CLONE || c->resolved_endpoint==NULL || c->bound)return COG_ERROR_INVALID_STATE;
    if(!canonical_path(store) || (workspace!=NULL && !canonical_path(workspace)) ||
       (workspace!=NULL)!=(c->options.workspace_root!=NULL))return COG_ERROR_INVALID_ARG;
    size_t a=workspace==NULL?0U:strlen(workspace)+1U,b=strlen(store)+1U;
    if(a>FIELD_BYTES || b>FIELD_BYTES || a>STRING_BYTES-c->used || b>STRING_BYTES-c->used-a)return COG_ERROR_LIMIT;
    if(a!=0U){memcpy(c->strings+c->used,workspace,a);c->options.workspace_root=c->strings+c->used;c->used+=a;}
    memcpy(c->strings+c->used,store,b);c->options.store_dir=c->strings+c->used;c->used+=b;
    return COG_OK;
}
cog_result_t cli_configuration_runtime_new(cog_cli_configuration_t **out,const cog_runtime_config_t *config)
{
 if(out==NULL)return COG_ERROR_NULL_PTR;
 *out=NULL;
 if(config==NULL)return COG_ERROR_NULL_PTR;
 cog_cli_configuration_t *c=calloc(1U,sizeof(*c));if(c==NULL)return COG_ERROR_NOMEM;
 c->origin=ORIGIN_CLONE;c->host_finished=true;
 c->generation=config->provider.endpoint==NULL ? 1U : config->provider.endpoint->generation;
 c->resolved_endpoint=config->provider.endpoint;
 cog_cli_options_t *o=&c->options;
 o->backend=COG_CLI_BACKEND_REAL;
 o->session_action=config->mode==COG_RUNTIME_NEW ? COG_CLI_SESSION_NEW :
     config->mode==COG_RUNTIME_RECOVER ? COG_CLI_SESSION_RECOVER : COG_CLI_SESSION_RESUME;
 o->session_id=config->mode==COG_RUNTIME_NEW ? config->session_id : NULL;
 o->source_session_id=config->mode==COG_RUNTIME_NEW ? NULL : config->session_id;
 o->previous_processes_fenced=config->previous_processes_fenced;
 o->store_dir=config->store.base_dir;
 o->store_mode=config->store.backend==&cog_store_null_backend ? COG_CLI_STORE_DISABLED : COG_CLI_STORE_DURABLE;
 o->model_id=config->provider.model_id;
 o->endpoint_name=config->provider.endpoint==NULL ? NULL : config->provider.endpoint->name;
 o->base_url=config->provider.endpoint==NULL ? NULL : config->provider.endpoint->base_url;
 o->tool_policy=COG_CLI_TOOL_POLICY_ASK;
 if(config->workers!=NULL) {
   const cog_endpoint_t *endpoint=config->provider.endpoint;
   if(endpoint==NULL || endpoint->vtable!=&cog_openrouter_provider_vtable ||
      endpoint->implementation_config==NULL || endpoint->api_key==NULL || endpoint->api_key[0]=='\0') {
     cog_cli_configuration_free(c);return COG_ERROR_UNSUPPORTED;
   }
   const cog_model_t *model=NULL;
   cog_result_t model_result=cog_endpoint_model_lookup(endpoint,config->provider.model_id,&model);
   if(model_result!=COG_OK || model==NULL || model->capabilities!=(COG_MODEL_CAPABILITY_STREAMING|COG_MODEL_CAPABILITY_TOOLS)) {
     cog_cli_configuration_free(c);return model_result!=COG_OK?model_result:COG_ERROR_UNSUPPORTED;
   }
   if((config->instruction_mode!=COG_RUNTIME_INSTRUCTIONS_DISABLED || config->provider.prompt_template!=NULL) &&
      config->provider.prompt_template!=&cog_openrouter_prompt_template) {
     cog_cli_configuration_free(c);return COG_ERROR_UNSUPPORTED;
   }
   if((endpoint->api_version!=NULL && endpoint->api_version[0]!='\0') ||
      (model->context_window!=0U && config->limits.max_output_tokens>model->context_window)) {
     cog_cli_configuration_free(c);return COG_ERROR_UNSUPPORTED;
   }
   size_t secret_bytes=strlen(endpoint->api_key);
   if(secret_bytes>=sizeof(c->secret)){cog_cli_configuration_free(c);return COG_ERROR_LIMIT;}
   for(size_t i=0U;i<secret_bytes;i++)if((unsigned char)endpoint->api_key[i]<=32U ||
       (unsigned char)endpoint->api_key[i]==127U){cog_cli_configuration_free(c);return COG_ERROR_INVALID_ARG;}
   memcpy(c->secret,endpoint->api_key,secret_bytes+1U);
   o->credential_ref="resolved";
   o->endpoint_name="openrouter";
   o->workspace_root=config->workers->workspace_root;
   o->net=((const cog_openrouter_config_t *)endpoint->implementation_config)->net;
   bool allow=false,ask=false;
   for(size_t i=0U;i<config->tools.binding_count;i++) {
     const cog_prompt_tool_binding_t *binding=&config->tools.bindings[i];
     if(binding->policy==COG_PROMPT_TOOL_DENY)continue;
     allow|=binding->policy==COG_PROMPT_TOOL_ALLOW;ask|=binding->policy==COG_PROMPT_TOOL_ASK;
     for(size_t j=0U;j<cog_workspace_tools.count;j++)
       if(binding->descriptor==&cog_workspace_tools.tools[j])c->tool_mask|=(uint32_t)(1U<<j);
   }
   if(config->instruction_mode==COG_RUNTIME_INSTRUCTIONS_DISABLED && allow && ask) {
     cog_cli_configuration_free(c);return COG_ERROR_UNSUPPORTED;
   }
   o->tool_policy=allow?COG_CLI_TOOL_POLICY_ALLOW:ask?COG_CLI_TOOL_POLICY_ASK:COG_CLI_TOOL_POLICY_DENY;
   c->endpoint_present=true;
 }
 o->max_output_tokens=config->limits.max_output_tokens;o->max_tool_rounds=config->limits.max_tool_rounds;
 o->max_turn_ms=config->limits.max_turn_ms;o->max_tool_ms=config->limits.max_tool_ms;
 o->max_tool_output_bytes=config->limits.max_tool_output_bytes;o->max_context_bytes=config->limits.max_context_bytes;
 o->max_retries=config->limits.max_retries;o->max_retry_elapsed_ms=config->limits.max_retry_elapsed_ms;
 cog_result_t rc=copy_option_strings(c);
 if(rc!=COG_OK){cog_cli_configuration_free(c);return rc;}
 cache_labels(c);*out=c;return COG_OK;
}

cog_result_t cli_configuration_instruction_attach(cog_cli_configuration_t *c,
    const cog_cli_instruction_capsule_t *capsule)
{
    if (c == NULL || capsule == NULL) return COG_ERROR_NULL_PTR;
    if (c->bound || c->instruction_attached) return COG_ERROR_INVALID_STATE;
    static const unsigned char prefix[] = {'C','G','B','1'};
    cog_cli_instruction_bootstrap_t framing = {prefix, sizeof(prefix), *capsule};
    size_t required = 0;
    cog_result_t rc = cog_cli_instruction_bootstrap_encode(&framing, NULL, 0U, &required);
    if (rc != COG_ERROR_OVERFLOW) return rc;
    void *bytes = NULL;
    if (capsule->length) {
        bytes = malloc(capsule->length);
        if (bytes == NULL) return COG_ERROR_NOMEM;
        memcpy(bytes, capsule->bytes, capsule->length);
    }
    c->instructions = *capsule; c->instructions.bytes = bytes;
    c->instruction_attached = true;
    return COG_OK;
}
cog_result_t cli_configuration_instruction_replace(cog_cli_configuration_t *c,
    const cog_cli_instruction_capsule_t *capsule)
{
    if(c==NULL || capsule==NULL)return COG_ERROR_NULL_PTR;
    if(c->bound || c->origin!=ORIGIN_CLONE)return COG_ERROR_INVALID_STATE;
    cog_cli_configuration_t *temporary=calloc(1U,sizeof(*temporary));
    if(temporary==NULL)return COG_ERROR_NOMEM;
    cog_result_t rc=cli_configuration_instruction_attach(temporary,capsule);
    if(rc==COG_OK) {
        if(c->instructions.bytes!=NULL) {
            clear((void *)c->instructions.bytes,c->instructions.length);
            free((void *)c->instructions.bytes);
        }
        c->instructions=temporary->instructions;c->instruction_attached=true;
        temporary->instructions=(cog_cli_instruction_capsule_t){0};temporary->instruction_attached=false;
    }
    cog_cli_configuration_free(temporary);return rc;
}
const cog_cli_instruction_capsule_t *cli_configuration_instruction_capsule(
    const cog_cli_configuration_t *c)
{
    return c != NULL && c->instruction_attached ? &c->instructions : NULL;
}
uint64_t cli_configuration_generation(const cog_cli_configuration_t *c)
{
    return c == NULL ? 0U : c->generation;
}
