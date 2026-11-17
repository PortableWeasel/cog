#include <cog_cli/cli.h>
#include <stdio.h>
#include "options_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static cog_result_t node_number(const char *s, uint64_t *out)
{
    uint64_t n = 0U;
    if (*s == '\0')
        return COG_ERROR_INVALID_ARG;
    while (*s != '\0') {
        uint64_t digit;
        if (*s < '0' || *s > '9')
            return COG_ERROR_INVALID_ARG;
        digit = (uint64_t)(*s - '0');
        if (n > (UINT64_MAX - digit) / UINT64_C(10))
            return COG_ERROR_INVALID_ARG;
        n = n * UINT64_C(10) + digit;
        ++s;
    }
    *out = n;
    return COG_OK;
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
    bool store = false, no_store = false, action_seen = false, from_node = false;
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
        if (strcmp(arg, "--sessions") == 0) {
            if (action_seen)
                return bad(exit_code);
            action_seen = true;
            out->session_action = COG_CLI_SESSION_LIST;
            continue;
        }
        if (match(arg, "--resume") || match(arg, "--fork") || match(arg, "--recover")) {
            if (action_seen || value(argc, argv, &i, &v) != COG_OK)
                return bad(exit_code);
            action_seen = true;
            out->session_action = match(arg, "--resume") ? COG_CLI_SESSION_RESUME
                                  : match(arg, "--fork") ? COG_CLI_SESSION_FORK
                                                         : COG_CLI_SESSION_RECOVER;
            out->source_session_id = v;
            continue;
        }
        if (match(arg, "--from-node")) {
            if (from_node || value(argc, argv, &i, &v) != COG_OK ||
                node_number(v, &out->fork_node_id) != COG_OK)
                return bad(exit_code);
            from_node = true;
            continue;
        }
        if (strcmp(arg, "--fenced") == 0) {
            out->previous_processes_fenced = true;
            continue;
        }
        if (strcmp(arg, "--usage") == 0) {
            out->show_usage = true;
            continue;
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
    if (from_node && out->session_action != COG_CLI_SESSION_FORK)
        return bad(exit_code);
    {
        cog_result_t rc = cog_cli_session_options_validate(out);
        /* The constructor reports a missing recovery assertion as DENIED. */
        if (rc != COG_OK && rc != COG_ERROR_DENIED)
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
          "  -s, --session ID        NEW session identity\n"
          "      --sessions          List durable IDs offline\n"
          "      --resume ID         Resume an existing durable session\n"
          "      --fork ID           Select a safe branch in this same session\n"
          "      --from-node NODE    FORK parent (0 selects active leaf)\n"
          "      --recover ID        Offline recovery; requires --fenced\n"
          "      --fenced            Assert prior writers/processes are fenced\n"
          "      --usage             Report available usage to stderr\n"
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
