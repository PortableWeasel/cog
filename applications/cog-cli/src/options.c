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
    if (s == NULL || *s == '\0')
        return COG_ERROR_INVALID_ARG;
    switch (f->kind) {
    case CLI_STRING:
        *(const char **)field = s;
        return COG_OK;
    case CLI_BACKEND:
        if (strcmp(s, "real") == 0)
            *(cog_cli_backend_t *)field = COG_CLI_BACKEND_REAL;
        else if (strcmp(s, "mock") == 0)
            *(cog_cli_backend_t *)field = COG_CLI_BACKEND_MOCK;
        else
            return COG_ERROR_INVALID_ARG;
        return COG_OK;
    case CLI_POLICY:
        if (strcmp(s, "ask") == 0)
            *(cog_cli_tool_policy_t *)field = COG_CLI_TOOL_POLICY_ASK;
        else if (strcmp(s, "deny") == 0)
            *(cog_cli_tool_policy_t *)field = COG_CLI_TOOL_POLICY_DENY;
        else if (strcmp(s, "allow") == 0)
            *(cog_cli_tool_policy_t *)field = COG_CLI_TOOL_POLICY_ALLOW;
        else
            return COG_ERROR_INVALID_ARG;
        return COG_OK;
    case CLI_STORE:
        if (strcmp(s, "durable") == 0)
            *(cog_cli_store_mode_t *)field = COG_CLI_STORE_DURABLE;
        else if (strcmp(s, "disabled") == 0)
            *(cog_cli_store_mode_t *)field = COG_CLI_STORE_DISABLED;
        else
            return COG_ERROR_INVALID_ARG;
        return COG_OK;
    case CLI_U32:
    case CLI_SIZE:
        rc = number(s, f->kind == CLI_U32 ? UINT32_MAX : SIZE_MAX, &n);
        if (rc != COG_OK)
            return rc;
        if (n == 0U && !f->zero)
            return COG_ERROR_LIMIT;
        if (f->kind == CLI_U32)
            *(uint32_t *)field = (uint32_t)n;
        else
            *(size_t *)field = n;
        return COG_OK;
    }
    return COG_ERROR_INVALID_ARG;
}
static bool match(const char *arg, const char *flag)
{
    size_t n;
    if (flag == NULL)
        return false;
    n = strlen(flag);
    return strncmp(arg, flag, n) == 0 && (arg[n] == '\0' || arg[n] == '=');
}
static cog_result_t value(int argc, char **argv, int *index, const char **out)
{
    const char *equal = strchr(argv[*index], '=');
    if (equal != NULL) {
        *out = equal + 1;
        return COG_OK;
    }
    if (*index + 1 >= argc || argv[*index + 1] == NULL)
        return COG_ERROR_INVALID_ARG;
    *out = argv[++*index];
    return COG_OK;
}
static cog_result_t bad(int *code)
{
    /* Never echo arbitrary argv: it may contain a mistakenly supplied secret. */
    fputs("cog-cli: invalid option or value; use --help\n", stderr);
    if (code != NULL)
        *code = 2;
    return COG_ERROR_ABORTED;
}
cog_result_t cog_cli_options_parse(cog_cli_options_t *out, int argc, char **argv,
                                   int *exit_code)
{
    int i;
    bool store = false, no_store = false;
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    cog_cli_options_defaults(out);
    if (exit_code != NULL)
        *exit_code = 0;
    if (argc < 0 || (argc > 0 && argv == NULL))
        return bad(exit_code);
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i], *v = NULL;
        size_t j;
        if (arg == NULL)
            return bad(exit_code);
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            cog_cli_print_usage(stdout);
            return COG_ERROR_ABORTED;
        }
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            printf("cog-cli %s\n", COG_CLI_VERSION);
            return COG_ERROR_ABORTED;
        }
        if (strcmp(arg, "--mock") == 0) {
            out->backend = COG_CLI_BACKEND_MOCK;
            out->explicit_options |= COG_CLI_OPT_BACKEND;
            continue;
        }
        if (strcmp(arg, "--no-store") == 0) {
            no_store = true;
            if (store)
                return bad(exit_code);
            out->store_mode = COG_CLI_STORE_DISABLED;
            out->explicit_options |= COG_CLI_OPT_STORE_MODE;
            continue;
        }
        if (strcmp(arg, "--interactive") == 0 || strcmp(arg, "-i") == 0) {
            out->interactive = true;
            continue;
        }
        if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            out->verbose = true;
            continue;
        }
        for (j = 0U; j < cog_cli_field_count; ++j) {
            const cli_field_t *f = &cog_cli_fields[j];
            if (!match(arg, f->flag) && !match(arg, f->short_flag))
                continue;
            if (value(argc, argv, &i, &v) != COG_OK ||
                cog_cli_field_set(out, f, v) != COG_OK)
                return bad(exit_code);
            out->explicit_options |= f->bit;
            if (f->bit == COG_CLI_OPT_STORE_DIR) {
                store = true;
                if (no_store)
                    return bad(exit_code);
                out->store_mode = COG_CLI_STORE_DURABLE;
                out->explicit_options |= COG_CLI_OPT_STORE_MODE;
            }
            break;
        }
        if (j < cog_cli_field_count)
            continue;
#define SIMPLE(longflag, shortflag, member)                                            \
    if (match(arg, longflag) || match(arg, shortflag)) {                               \
        if (value(argc, argv, &i, &v) != COG_OK || *v == '\0')                         \
            return bad(exit_code);                                                     \
        out->member = v;                                                               \
        continue;                                                                      \
    }
        if (match(arg, "--prompt") || match(arg, "-p")) {
            if (value(argc, argv, &i, &v) != COG_OK)
                return bad(exit_code);
            out->prompt = v;
            out->interactive = false;
            continue;
        }
        SIMPLE("--session", "-s", session_id)
        SIMPLE("--log", "-l", log_path)
        SIMPLE("--socket", "-S", socket_path)
        SIMPLE("--config", NULL, config_path)
        SIMPLE("--chunks", "-c", chunks)
        SIMPLE("--queue", "-q", queue_text)
#undef SIMPLE
        if (match(arg, "--mock-mode") || match(arg, "-M")) {
            if (value(argc, argv, &i, &v) != COG_OK)
                return bad(exit_code);
            if (strcmp(v, "stream") == 0)
                out->mock_mode = COG_CLI_MOCK_STREAM;
            else if (strcmp(v, "error") == 0)
                out->mock_mode = COG_CLI_MOCK_ERROR;
            else if (strcmp(v, "cancel") == 0)
                out->mock_mode = COG_CLI_MOCK_CANCEL;
            else if (strcmp(v, "rate-limit") == 0)
                out->mock_mode = COG_CLI_MOCK_RATE_LIMIT;
            else
                return bad(exit_code);
            continue;
        }
        return bad(exit_code);
    }
    return COG_OK;
}
void cog_cli_print_usage(FILE *stream)
{
    if (stream == NULL)
        return;
    fputs("Usage: cog-cli [options]\n"
          "REAL OpenRouter is the default. Supply --model ID or COG_MODEL.\n"
          "Credentials default to env:OPENROUTER_API_KEY; keys are never arguments.\n"
          "  -p, --prompt TEXT       Run one prompt\n"
          "  -i, --interactive       Read stdin\n"
          "  -s, --session ID        Session identity\n"
          "  -m, --model ID          Explicit provider model ID\n"
          "  -e, --endpoint NAME     Registry name (default openrouter)\n"
          "      --base-url URL      HTTPS base URL\n"
          "      --config PATH       Bounded dotted-key=value file\n"
          "      --credential-ref R  env:NAME or explicit host resolver\n"
          "      --workspace PATH    Workspace root\n"
          "      --tool-policy P     ask|deny|allow (default ask)\n"
          "  -d, --store DIR         Durable session directory\n"
          "      --no-store          Text only; tools cannot execute\n"
          "      --max-output-tokens N   --max-tool-rounds N\n"
          "      --max-turn-ms N         --max-tool-ms N\n"
          "      --max-tool-output-bytes N --max-context-bytes N\n"
          "      --max-retries N     Additional attempts; zero disables\n"
          "      --max-retry-elapsed-ms N\n"
          "      --mock              Explicit deterministic fixture backend\n"
          "  -M, --mock-mode MODE    stream|error|cancel|rate-limit\n"
          "  -c, --chunks LIST       Mock chunks\n"
          "  -q, --queue TEXT        Next prompt\n"
          "  -l, --log PATH          Log destination\n"
          "  -S, --socket PATH       IPC listener\n"
          "  -v, --verbose           Debug logging\n"
          "  -h, --help              Help\n"
          "  -V, --version           Version\n",
          stream);
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
