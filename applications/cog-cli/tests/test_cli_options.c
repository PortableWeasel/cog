/**
 * @file test_cli_options.c
 * @brief Contract test for cog_cli_options_parse.
 *
 * Covers defaults, every value option (short, long, and --long=value
 * forms), flag options, --help/--version exit codes, unknown-option and
 * missing-value errors, and invalid --mock-mode.
 */
#include "cog_cli/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cast a string literal to non-const char* for argv arrays (string
 * literals are const under -Wwrite-strings). */
#define S(x) ((char *)(x))

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void test_defaults(void)
{
    cog_cli_options_t o;
    int exit_code = 99;

    CHECK(cog_cli_options_parse(&o, 1, (char *[]){S("cog-cli")}, &exit_code)
          == COG_OK);
    CHECK(o.prompt == NULL);
    CHECK(o.interactive);
    CHECK(!o.verbose);
    CHECK(o.session_id == NULL);
    CHECK(o.store_dir == NULL);
    CHECK(o.log_path == NULL);
    CHECK(o.socket_path == NULL);
    CHECK(o.queue_text == NULL);
    CHECK(o.model_id == NULL);
    CHECK(o.backend == COG_CLI_BACKEND_REAL);
    CHECK(o.explicit_options == 0U);
    CHECK(o.endpoint_name != NULL && strcmp(o.endpoint_name, "openrouter") == 0);
    CHECK(o.chunks != NULL && strcmp(o.chunks, "Hello from cog-cli!") == 0);
    CHECK(o.mock_mode == COG_CLI_MOCK_STREAM);
    CHECK_EQ_INT(exit_code, 0);
}

static void test_prompt_short_and_long(void)
{
    cog_cli_options_t o;
    int exit_code = 99;

    CHECK(cog_cli_options_parse(&o, 3, (char *[]){S("cog-cli"), S("-p"), S("hi")},
                                &exit_code) == COG_OK);
    CHECK(o.prompt != NULL && strcmp(o.prompt, "hi") == 0);
    CHECK(!o.interactive);

    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("--prompt"), S("hello")},
                                &exit_code) == COG_OK);
    CHECK(o.prompt != NULL && strcmp(o.prompt, "hello") == 0);
    CHECK(!o.interactive);

    CHECK(cog_cli_options_parse(&o, 2,
                                (char *[]){S("cog-cli"), S("--prompt=world")},
                                &exit_code) == COG_OK);
    CHECK(o.prompt != NULL && strcmp(o.prompt, "world") == 0);
    CHECK(!o.interactive);
}

static void test_value_options(void)
{
    cog_cli_options_t o;
    int exit_code = 99;
    char *argv[] = {
        S("cog-cli"),
        S("-s"), S("sess-1"),
        S("--store"), S("/tmp/store"),
        S("-l"), S("/tmp/cog.log"),
        S("--socket=/tmp/cog.sock"),
        S("-m"), S("mock-model-2"),
        S("--endpoint"), S("mock"),
        S("-c"), S("a,b,c"),
        S("-q"), S("queued"),
        S("-M"), S("rate-limit"),
        S("-v"),
    };

    CHECK(cog_cli_options_parse(&o, (int)(sizeof(argv) / sizeof(argv[0])),
                                argv, &exit_code) == COG_OK);
    CHECK(o.session_id != NULL && strcmp(o.session_id, "sess-1") == 0);
    CHECK(o.store_dir != NULL && strcmp(o.store_dir, "/tmp/store") == 0);
    CHECK(o.log_path != NULL && strcmp(o.log_path, "/tmp/cog.log") == 0);
    CHECK(o.socket_path != NULL &&
          strcmp(o.socket_path, "/tmp/cog.sock") == 0);
    CHECK(o.model_id != NULL && strcmp(o.model_id, "mock-model-2") == 0);
    CHECK(o.endpoint_name != NULL && strcmp(o.endpoint_name, "mock") == 0);
    CHECK(o.chunks != NULL && strcmp(o.chunks, "a,b,c") == 0);
    CHECK(o.queue_text != NULL && strcmp(o.queue_text, "queued") == 0);
    CHECK(o.mock_mode == COG_CLI_MOCK_RATE_LIMIT);
    CHECK(o.verbose);
}

static void test_mock_modes(void)
{
    cog_cli_options_t o;
    int exit_code = 99;

    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("-M"), S("stream")},
                                &exit_code) == COG_OK);
    CHECK(o.mock_mode == COG_CLI_MOCK_STREAM);

    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("-M"), S("error")},
                                &exit_code) == COG_OK);
    CHECK(o.mock_mode == COG_CLI_MOCK_ERROR);

    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("-M"), S("cancel")},
                                &exit_code) == COG_OK);
    CHECK(o.mock_mode == COG_CLI_MOCK_CANCEL);

    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("-M"), S("rate-limit")},
                                &exit_code) == COG_OK);
    CHECK(o.mock_mode == COG_CLI_MOCK_RATE_LIMIT);
}

static void test_help_version(void)
{
    cog_cli_options_t o;
    int exit_code = 99;

    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("--help")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 0);

    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("-h")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 0);

    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("--version")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 0);

    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("-V")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 0);
}

static void test_errors(void)
{
    cog_cli_options_t o;
    int exit_code = 99;

    /* Unknown option. */
    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("--bogus")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 2);

    /* Missing value. */
    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("--prompt")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 2);

    /* Invalid mock mode. */
    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("-M"), S("bogus")},
                                &exit_code) == COG_ERROR_ABORTED);
    CHECK_EQ_INT(exit_code, 2);

    /* NULL out. */
    CHECK(cog_cli_options_parse(NULL, 1, (char *[]){S("cog-cli")}, NULL)
          == COG_ERROR_NULL_PTR);
}

static void test_result_name(void)
{
    CHECK(strcmp(cog_cli_result_name(COG_OK), "ok") == 0);
    CHECK(strcmp(cog_cli_result_name(COG_ERROR_UNAVAILABLE),
                 "unavailable") == 0);
    CHECK(strcmp(cog_cli_result_name((cog_result_t)999), "unknown") == 0);
}

static void test_session_actions(void)
{
    cog_cli_options_t o;
    int code = 99;
    CHECK(cog_cli_options_parse(&o, 2, (char *[]){S("cog-cli"), S("--sessions")},
                                &code) == COG_OK);
    CHECK(o.session_action == COG_CLI_SESSION_LIST && o.source_session_id == NULL);
    CHECK(o.explicit_options == 0U);
    CHECK(cog_cli_options_parse(&o, 4,
                                (char *[]){S("cog-cli"), S("--resume=session"),
                                           S("--fenced"), S("--usage")},
                                &code) == COG_OK);
    CHECK(o.session_action == COG_CLI_SESSION_RESUME &&
          strcmp(o.source_session_id, "session") == 0);
    CHECK(o.previous_processes_fenced && o.show_usage && o.explicit_options == 0U);
    CHECK(cog_cli_options_parse(&o, 7,
                                (char *[]){S("cog-cli"), S("--fork"), S("session"),
                                           S("--from-node=18446744073709551615"),
                                           S("--fenced"), S("--prompt"), S("fresh")},
                                &code) == COG_OK);
    CHECK(o.session_action == COG_CLI_SESSION_FORK && o.fork_node_id == UINT64_MAX);
    CHECK(o.previous_processes_fenced && strcmp(o.prompt, "fresh") == 0 &&
          o.explicit_options == 0U);
    CHECK(cog_cli_options_parse(
              &o, 4,
              (char *[]){S("cog-cli"), S("--recover"), S("session"), S("--fenced")},
              &code) == COG_OK);
    CHECK(o.session_action == COG_CLI_SESSION_RECOVER && o.previous_processes_fenced);
    /* Missing recovery fencing is constructor DENIED, not an implicit assertion. */
    CHECK(cog_cli_options_parse(&o, 3,
                                (char *[]){S("cog-cli"), S("--recover"), S("session")},
                                &code) == COG_OK);
    CHECK(o.session_action == COG_CLI_SESSION_RECOVER && !o.previous_processes_fenced);
    CHECK(cog_cli_options_parse(
              &o, 4,
              (char *[]){S("cog-cli"), S("--fork=session"), S("--from-node"), S("0")},
              &code) == COG_OK);
    CHECK(o.fork_node_id == 0U && o.session_action == COG_CLI_SESSION_FORK);
}
static void test_session_action_conflicts(void)
{
    static const char *const cases[][8] = {
        {"cog-cli", "--sessions", "--sessions", NULL},
        {"cog-cli", "--sessions", "--resume", "source", NULL},
        {"cog-cli", "--resume=one", "--fork=two", NULL},
        {"cog-cli", "--session", "new-only", "--resume=source", NULL},
        {"cog-cli", "--sessions", "--prompt=", NULL},
        {"cog-cli", "--sessions", "--queue=queued", NULL},
        {"cog-cli", "--recover=source", "--fenced", "--prompt=text", NULL},
        {"cog-cli", "--recover=source", "--fenced", "--queue=text", NULL},
        {"cog-cli", "--fork=source", "--queue=text", NULL},
        {"cog-cli", "--from-node=0", NULL},
        {"cog-cli", "--resume=source", "--from-node=0", NULL},
        {"cog-cli", "--fork=source", "--from-node=18446744073709551616", NULL},
        {"cog-cli", "--fork=source", "--from-node=-1", NULL},
        {"cog-cli", "--fork=source", "--from-node=+1", NULL},
        {"cog-cli", "--fork=source", "--from-node=", NULL},
        {"cog-cli", "--fork=source", "--from-node=0", "--from-node=1", NULL},
        {"cog-cli", "--resume=", NULL},
        {"cog-cli", "--resume=path/id", NULL},
        {"cog-cli", "--resume=.cog-domain.lock", NULL},
        {"cog-cli", "--resume=bad\x80", NULL},
        {"cog-cli", "--resume", NULL},
        {"cog-cli", "--fenced", NULL},
        {"cog-cli", "--sessions", "--fenced", NULL}};
    size_t i;
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cog_cli_options_t o;
        char *args[8];
        int argc = 0, code = 0;
        while (cases[i][argc] != NULL) {
            args[argc] = (char *)cases[i][argc];
            ++argc;
        }
        CHECK(cog_cli_options_parse(&o, argc, args, &code) == COG_ERROR_ABORTED);
        CHECK_EQ_INT(code, 2);
    }
}


int main(void)
{
    test_session_actions();
    test_session_action_conflicts();
    test_defaults();
    test_prompt_short_and_long();
    test_value_options();
    test_mock_modes();
    test_help_version();
    test_errors();
    test_result_name();

    if (failures != 0) {
        (void)fprintf(stderr, "test_cli_options: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_cli_options: all checks passed\n");
    return 0;
}
