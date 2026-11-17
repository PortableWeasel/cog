/** @file options.c @brief Bounded CLI parsing with explicit presence. */
#include "options_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define FIELD(flag, shortflag, key, env, bit, member, kind, zero)                      \
    {flag, shortflag, key, env, bit, offsetof(cog_cli_options_t, member), kind, zero}
const cli_field_t cog_cli_fields[] = {
    FIELD("--backend", NULL, "client.backend", NULL, COG_CLI_OPT_BACKEND, backend,
          CLI_BACKEND, false),
    FIELD("--model", "-m", "models.default", "COG_MODEL", COG_CLI_OPT_MODEL, model_id,
          CLI_STRING, false),
    FIELD("--endpoint", "-e", "providers.selected", "COG_ENDPOINT",
          COG_CLI_OPT_ENDPOINT, endpoint_name, CLI_STRING, false),
    FIELD("--base-url", NULL, "providers.openrouter.base-url", "COG_BASE_URL",
          COG_CLI_OPT_BASE_URL, base_url, CLI_STRING, false),
    FIELD("--credential-ref", NULL, "providers.openrouter.credential-ref",
          "COG_CREDENTIAL_REF", COG_CLI_OPT_CREDENTIAL, credential_ref, CLI_STRING,
          false),
    FIELD("--workspace", NULL, "workspace.root", "COG_WORKSPACE", COG_CLI_OPT_WORKSPACE,
          workspace_root, CLI_STRING, false),
    FIELD("--tool-policy", NULL, "tools.policy", "COG_TOOL_POLICY", COG_CLI_OPT_POLICY,
          tool_policy, CLI_POLICY, false),
    FIELD("--max-output-tokens", NULL, "limits.max-output-tokens",
          "COG_MAX_OUTPUT_TOKENS", COG_CLI_OPT_OUTPUT_TOKENS, max_output_tokens,
          CLI_U32, false),
    FIELD("--max-tool-rounds", NULL, "limits.max-tool-rounds", "COG_MAX_TOOL_ROUNDS",
          COG_CLI_OPT_TOOL_ROUNDS, max_tool_rounds, CLI_U32, false),
    FIELD("--max-turn-ms", NULL, "limits.max-turn-ms", "COG_MAX_TURN_MS",
          COG_CLI_OPT_TURN_MS, max_turn_ms, CLI_U32, false),
    FIELD("--max-tool-ms", NULL, "limits.max-tool-ms", "COG_MAX_TOOL_MS",
          COG_CLI_OPT_TOOL_MS, max_tool_ms, CLI_U32, false),
    FIELD("--max-tool-output-bytes", NULL, "limits.max-tool-output-bytes",
          "COG_MAX_TOOL_OUTPUT_BYTES", COG_CLI_OPT_TOOL_OUTPUT, max_tool_output_bytes,
          CLI_SIZE, false),
    FIELD("--max-context-bytes", NULL, "limits.max-context-bytes",
          "COG_MAX_CONTEXT_BYTES", COG_CLI_OPT_CONTEXT, max_context_bytes, CLI_SIZE,
          false),
    FIELD("--max-retries", NULL, "limits.max-retries", "COG_MAX_RETRIES",
          COG_CLI_OPT_RETRIES, max_retries, CLI_U32, true),
    FIELD("--max-retry-elapsed-ms", NULL, "limits.max-retry-elapsed-ms",
          "COG_MAX_RETRY_ELAPSED_MS", COG_CLI_OPT_RETRY_MS, max_retry_elapsed_ms,
          CLI_U32, false),
    FIELD("--store-mode", NULL, "store.mode", "COG_STORE_MODE", COG_CLI_OPT_STORE_MODE,
          store_mode, CLI_STORE, false),
    FIELD("--store", "-d", "store.directory", "COG_STORE_DIR", COG_CLI_OPT_STORE_DIR,
          store_dir, CLI_STRING, false)};
const size_t cog_cli_field_count = sizeof(cog_cli_fields) / sizeof(cog_cli_fields[0]);
void cog_cli_options_defaults(cog_cli_options_t *o)
{
    memset(o, 0, sizeof(*o));
    o->endpoint_name = "openrouter";
    o->base_url = "https://openrouter.ai/api/v1";
    o->credential_ref = "env:OPENROUTER_API_KEY";
    o->chunks = "Hello from cog-cli!";
    o->interactive = true;
    o->max_output_tokens = 4096U;
    o->max_tool_rounds = 8U;
    o->max_turn_ms = 120000U;
    o->max_tool_ms = 30000U;
    o->max_tool_output_bytes = 65536U;
    o->max_context_bytes = 1024U * 1024U;
    o->max_retries = 1U;
    o->max_retry_elapsed_ms = 10000U;
}
/* Validate protocol identifiers without imposing Unicode rules on filesystem
 * paths. Range checks reject overlong scalars, surrogates and values >10FFFF. */
bool cog_cli_utf8_valid(const char *s)
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
/* Session command inputs are deliberately outside the precedence table. */
static cog_result_t source_id_valid(const char *id)
{
    size_t i, n;
    if (id == NULL || *id == '\0')
        return COG_ERROR_INVALID_ARG;
    n = strlen(id);
    if (n > 127U)
        return COG_ERROR_LIMIT;
    if (strcmp(id, ".") == 0 || strcmp(id, "..") == 0 ||
        strcmp(id, ".cog-domain.lock") == 0 || !cog_cli_utf8_valid(id))
        return COG_ERROR_INVALID_ARG;
    for (i = 0U; i < n; ++i)
        if ((unsigned char)id[i] < 32U || (unsigned char)id[i] == 127U ||
            id[i] == '/' || id[i] == '\\')
            return COG_ERROR_INVALID_ARG;
    return COG_OK;
}
cog_result_t cog_cli_session_options_validate(const cog_cli_options_t *o)
{
    if (o == NULL)
        return COG_ERROR_NULL_PTR;
    switch (o->session_action) {
    case COG_CLI_SESSION_NEW:
        if (o->source_session_id != NULL || o->fork_node_id != 0U ||
            o->previous_processes_fenced)
            return COG_ERROR_INVALID_ARG;
        return COG_OK;
    case COG_CLI_SESSION_LIST:
        if (o->session_id != NULL || o->source_session_id != NULL ||
            o->fork_node_id != 0U || o->previous_processes_fenced ||
            o->prompt != NULL || o->queue_text != NULL)
            return COG_ERROR_INVALID_ARG;
        return COG_OK;
    case COG_CLI_SESSION_RESUME:
    case COG_CLI_SESSION_FORK:
    case COG_CLI_SESSION_RECOVER:
        if (o->session_id != NULL ||
            (o->session_action != COG_CLI_SESSION_FORK && o->fork_node_id != 0U) ||
            (o->session_action == COG_CLI_SESSION_FORK && o->queue_text != NULL) ||
            (o->session_action == COG_CLI_SESSION_RECOVER &&
             (o->prompt != NULL || o->queue_text != NULL)))
            return COG_ERROR_INVALID_ARG;
        {
            cog_result_t rc = source_id_valid(o->source_session_id);
            if (rc != COG_OK)
                return rc;
        }
        if (o->session_action == COG_CLI_SESSION_RECOVER &&
            !o->previous_processes_fenced)
            return COG_ERROR_DENIED;
        return COG_OK;
    }
    return COG_ERROR_INVALID_ARG;
}
static cog_result_t number(const char *s, size_t ceiling, size_t *out)
{
    size_t n = 0U;
    if (*s == '\0')
        return COG_ERROR_INVALID_ARG;
    while (*s != '\0') {
        unsigned digit;
        if (*s < '0' || *s > '9')
            return COG_ERROR_INVALID_ARG;
        digit = (unsigned)(*s - '0');
        if (n > (ceiling - digit) / 10U)
            return COG_ERROR_INVALID_ARG;
        n = n * 10U + digit;
        ++s;
    }
    *out = n;
    return COG_OK;
}
cog_result_t cog_cli_field_set(cog_cli_options_t *o, const cli_field_t *f,
                               const char *s)
{
    unsigned char *field = (unsigned char *)o + f->offset;
    size_t n;
    cog_result_t rc;
    cog_cli_backend_t backend;
    cog_cli_tool_policy_t policy;
    cog_cli_store_mode_t store;
    uint32_t u32;
    if (s == NULL || *s == '\0')
        return COG_ERROR_INVALID_ARG;
    switch (f->kind) {
    case CLI_STRING:
        memcpy(field, &s, sizeof(s));
        return COG_OK;
    case CLI_BACKEND:
        if (strcmp(s, "real") == 0)
            backend = COG_CLI_BACKEND_REAL;
        else if (strcmp(s, "mock") == 0)
            backend = COG_CLI_BACKEND_MOCK;
        else
            return COG_ERROR_INVALID_ARG;
        memcpy(field, &backend, sizeof(backend));
        return COG_OK;
    case CLI_POLICY:
        if (strcmp(s, "ask") == 0)
            policy = COG_CLI_TOOL_POLICY_ASK;
        else if (strcmp(s, "deny") == 0)
            policy = COG_CLI_TOOL_POLICY_DENY;
        else if (strcmp(s, "allow") == 0)
            policy = COG_CLI_TOOL_POLICY_ALLOW;
        else
            return COG_ERROR_INVALID_ARG;
        memcpy(field, &policy, sizeof(policy));
        return COG_OK;
    case CLI_STORE:
        if (strcmp(s, "durable") == 0)
            store = COG_CLI_STORE_DURABLE;
        else if (strcmp(s, "disabled") == 0)
            store = COG_CLI_STORE_DISABLED;
        else
            return COG_ERROR_INVALID_ARG;
        memcpy(field, &store, sizeof(store));
        return COG_OK;
    case CLI_U32:
    case CLI_SIZE:
        rc = number(s, f->kind == CLI_U32 ? UINT32_MAX : SIZE_MAX, &n);
        if (rc != COG_OK)
            return rc;
        if (n == 0U && !f->zero)
            return COG_ERROR_LIMIT;
        if (f->kind == CLI_U32) {
            u32 = (uint32_t)n;
            memcpy(field, &u32, sizeof(u32));
        } else
            memcpy(field, &n, sizeof(n));
        return COG_OK;
    }
    return COG_ERROR_INVALID_ARG;
}
const char *cog_cli_result_name(cog_result_t rc)
{
    switch (rc) {
    case COG_OK:
        return "ok";
    case COG_ERROR_INVALID_ARG:
        return "invalid-argument";
    case COG_ERROR_NULL_PTR:
        return "null-pointer";
    case COG_ERROR_NOMEM:
        return "out-of-memory";
    case COG_ERROR_NOT_FOUND:
        return "not-found";
    case COG_ERROR_OVERFLOW:
        return "overflow";
    case COG_ERROR_UNHANDLED:
        return "unhandled";
    case COG_ERROR_TRANSITION_DENIED:
        return "transition-denied";
    case COG_ERROR_AGAIN:
        return "again";
    case COG_ERROR_INVALID_STATE:
        return "invalid-state";
    case COG_ERROR_IO:
        return "io-error";
    case COG_ERROR_ABORTED:
        return "aborted";
    case COG_ERROR_TIMEOUT:
        return "timeout";
    case COG_ERROR_UNAVAILABLE:
        return "unavailable";
    case COG_ERROR_LIMIT:
        return "limit";
    case COG_ERROR_UNSUPPORTED:
        return "unsupported";
    case COG_ERROR_PROTOCOL:
        return "protocol";
    case COG_ERROR_DENIED:
        return "denied";
    case COG_ERROR_INDETERMINATE:
        return "indeterminate";
    }
    return "unknown";
}
