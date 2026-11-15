/** @file configuration.c @brief Owned effective configuration and secret lifetime. */
#define _POSIX_C_SOURCE 200809L
#include "configuration.h"
#include "options_internal.h"
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define FILE_BYTES 65536U
#define FIELD_BYTES 4096U
#define SECRET_BYTES 16384U
#define STRING_BYTES 65536U
struct cog_cli_configuration {
    cog_cli_options_t options;
    cog_endpoint_t endpoint;
    cog_model_t model;
    cog_model_table_t models;
    cog_openrouter_config_t adapter;
    uint64_t generation;
    bool bound, endpoint_present;
    size_t used;
    char strings[STRING_BYTES];
    char secret[SECRET_BYTES];
    char file[FILE_BYTES + 1U];
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
    FILE *f;
    size_t len, i, start = 0U;
    uint32_t seen = 0U;
    if (path == NULL)
        return COG_OK;
    if (*path == '\0')
        return COG_ERROR_INVALID_ARG;
    f = fopen(path, "rb");
    if (f == NULL)
        return COG_ERROR_IO;
    len = fread(c->file, 1U, FILE_BYTES + 1U, f);
    if (ferror(f)) {
        (void)fclose(f);
        return COG_ERROR_IO;
    }
    if (fclose(f) != 0)
        return COG_ERROR_IO;
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
/* Validate protocol identifiers without imposing Unicode rules on filesystem
 * paths. Range checks reject overlong scalars, surrogates and values >10FFFF. */
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0U) {
        size_t width, i;
        unsigned char first = *p;
        if (first < 0x80U) {
            ++p;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU)
            width = 2U;
        else if (first >= 0xe0U && first <= 0xefU)
            width = 3U;
        else if (first >= 0xf0U && first <= 0xf4U)
            width = 4U;
        else
            return false;
        for (i = 1U; i < width; ++i)
            if (p[i] < 0x80U || p[i] > 0xbfU)
                return false;
        if ((first == 0xe0U && p[1] < 0xa0U) || (first == 0xedU && p[1] >= 0xa0U) ||
            (first == 0xf0U && p[1] < 0x90U) || (first == 0xf4U && p[1] > 0x8fU))
            return false;
        p += width;
    }
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
static cog_result_t validate(cog_cli_configuration_t *c)
{
    cog_cli_options_t *o = &c->options;
    size_t i;
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
    if (!token(o->model_id) || !utf8_valid(o->model_id) || !token(o->endpoint_name))
        return COG_ERROR_INVALID_ARG;
    if (o->store_mode == COG_CLI_STORE_DEFAULT)
        o->store_mode = o->backend == COG_CLI_BACKEND_REAL ? COG_CLI_STORE_DURABLE
                                                           : COG_CLI_STORE_DISABLED;
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
    if (o->backend == COG_CLI_BACKEND_MOCK)
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
        if (!((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') ||
              *name == '_'))
            return COG_ERROR_INVALID_ARG;
        for (i = 1U; name[i] != '\0'; ++i)
            if (!((name[i] >= 'A' && name[i] <= 'Z') ||
                  (name[i] >= 'a' && name[i] <= 'z') ||
                  (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
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
cog_result_t cog_cli_configuration_new(cog_cli_configuration_t **out,
                                       const cog_cli_options_t *input,
                                       uint64_t generation)
{
    cog_cli_configuration_t *c;
    cog_result_t rc;
    size_t i;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (input == NULL)
        return COG_ERROR_NULL_PTR;
    if (generation == 0U)
        return COG_ERROR_INVALID_ARG;
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
    c->options.net = input->net;
    c->options.credential_read = input->credential_read;
    c->options.credential_arg = input->credential_arg;
    rc = validate(c);
    if (rc != COG_OK)
        goto fail;
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
                goto fail;
        }
#define OWN(member)                                                                    \
    do {                                                                               \
        rc = owned(c, &c->options.member);                                             \
        if (rc != COG_OK)                                                              \
            goto fail;                                                                 \
    } while (0)
    OWN(prompt);
    OWN(session_id);
    OWN(config_path);
    OWN(log_path);
    OWN(socket_path);
    OWN(chunks);
    OWN(queue_text);
#undef OWN
    rc = credential(c);
    if (rc != COG_OK)
        goto fail;
    /* Resolver authority is construction-only; neither callback nor context is
     * retained for later service or exposed through the effective options. */
    c->options.credential_read = NULL;
    c->options.credential_arg = NULL;
    clear(c->file, sizeof(c->file));
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
    clear(c, sizeof(*c));
    free(c);
}
const cog_cli_options_t *cog_cli_configuration_options(const cog_cli_configuration_t *c)
{
    return c != NULL ? &c->options : NULL;
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
    if (c->bound)
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
        c->options.workspace_root != NULL)
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
    return c != NULL && c->bound ? &c->endpoint : NULL;
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
                 c->options.store_mode == COG_CLI_STORE_DURABLE &&
                         c->options.workspace_root != NULL
                     ? "policy-controlled"
                     : "disabled",
                 c->options.backend == COG_CLI_BACKEND_REAL ? "[REDACTED]" : "none",
                 (unsigned long long)c->generation);
    if (n < 0 || (size_t)n >= sizeof(text))
        return COG_ERROR_LIMIT;
    *required = (size_t)n + 1U;
    if (capacity < *required)
        return COG_ERROR_OVERFLOW;
    memcpy(output, text, *required);
    return COG_OK;
}
