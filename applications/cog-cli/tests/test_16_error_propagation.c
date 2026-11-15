/**
 * @file test_16_error_propagation.c
 * @brief Scenario 16: provider failure -> core error state/handler ->
 *        UI-visible error -> retry or abort.
 *
 * Drives the real CLI harness (cog::cli) with a scripted mock provider.
 * Two paths are exercised:
 *
 *  - Retryable failure (HTTP 429, retryable=true): the core handler
 *    reports the error, sleeps a bounded backoff, re-sends the request
 *    rebuilt from session history, and the retry's first receive
 *    succeeds.  The recovered chunk lands in the session tree and the
 *    run loop returns COG_OK.
 *  - Non-retryable failure (HTTP 500, retryable=false): the handler
 *    reports the error on stderr (UI-visible), transitions back to IDLE,
 *    and propagates the error out of cog_cli_app_run (fail loudly, no
 *    retry).
 *
 * The UI-visible error is verified by capturing stderr during the run
 * and asserting the provider error line is present.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/** Build a single-prompt app from options (fills parse defaults). */
static cog_cli_app_t *make_app(cog_cli_options_t *opts)
{
    cog_cli_app_t *app = NULL;

    if (opts->model_id == NULL) {
        opts->model_id = "mock-model";
    }
    if (opts->endpoint_name == NULL) {
        opts->endpoint_name = "mock";
    }
    opts->interactive = false;
    opts->backend = COG_CLI_BACKEND_MOCK;
    opts->explicit_options |= COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_ENDPOINT;
    if (opts->store_dir != NULL) {
        opts->store_mode = COG_CLI_STORE_DURABLE;
        opts->explicit_options |= COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_STORE_DIR;
    }
    if (cog_cli_app_new(&app, opts) != COG_OK) {
        return NULL;
    }
    return app;
}

/** Read the session history into a caller array (max @p cap). */
static size_t fixture_history(cog_cli_app_t *app, cog_message_t *out,
                              size_t cap)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;
    size_t i;

    if (cog_session_get_history(cog_cli_app_session(app),
                                cog_session_active(cog_cli_app_session(app)),
                                &msgs, &count) != COG_OK) {
        return 0U;
    }
    if (count > cap) {
        count = cap;
    }
    for (i = 0U; i < count; ++i) {
        out[i] = msgs[i];
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Path A: retryable provider error -> retry -> recovered              */
/* ------------------------------------------------------------------ */

static void test_retryable_error_retries(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_ERROR, NULL, COG_ERROR_UNAVAILABLE, 429, true,
         5U, "rate limited"},
        {COG_MOCK_STEP_CHUNK, "recovered", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 2U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_message_t hist[8];
    size_t n;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "retry me";
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* A retryable error must be retried once; the run loop returns OK
     * and the recovered chunk is in the session tree. */
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[1].content_len == 9U);
    CHECK(memcmp(hist[1].content, "recovered", 9U) == 0);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT((long long)st.send_count, 2LL); /* original + retry */
    CHECK(st.last_error.code == COG_OK);
    CHECK(st.last_error.http_status == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Path B: non-retryable provider error -> abort, UI-visible, no retry */
/* ------------------------------------------------------------------ */

static void test_nonretryable_error_aborts(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_ERROR, NULL, COG_ERROR_UNAVAILABLE, 500, false,
         0U, "hard fail"},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_mock_state_t st;
    char err_path[] = "/tmp/cog-16-stderr-XXXXXX";
    int saved_err;
    FILE *err_file;
    char buf[1024];
    size_t got;
    int fd;

    if (mkstemp(err_path) == -1) {
        (void)fprintf(stderr, "mkstemp failed\n");
        ++failures;
        return;
    }

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hi";
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Capture stderr so we can assert the UI-visible error line. */
    saved_err = dup(STDERR_FILENO);
    CHECK(saved_err >= 0);
    err_file = fopen(err_path, "w");
    CHECK(err_file != NULL);
    fd = fileno(err_file);
    CHECK(dup2(fd, STDERR_FILENO) >= 0);

    /* Non-retryable: no retry, error propagates out of run.  The
     * handler requests a transition back to IDLE, but because it
     * returned a non-OK status the core fails closed and does NOT apply
     * the transition, so the state machine stays in AWAITING_LLM. */
    CHECK(cog_cli_app_run(app) == COG_ERROR_UNAVAILABLE);
    CHECK(cog_cli_app_state(app) == (cog_state_t)1); /* AWAITING_LLM */

    /* Restore stderr. */
    (void)fflush(err_file);
    (void)dup2(saved_err, STDERR_FILENO);
    (void)close(saved_err);
    (void)fclose(err_file);

    /* The UI-visible error line was written to the captured stderr. */
    err_file = fopen(err_path, "r");
    CHECK(err_file != NULL);
    got = fread(buf, 1U, sizeof(buf) - 1U, err_file);
    buf[got] = '\0';
    (void)fclose(err_file);
    (void)unlink(err_path);
    CHECK(strstr(buf, "provider error") != NULL);
    CHECK(strstr(buf, "unavailable") != NULL);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT((long long)st.send_count, 1LL); /* no retry */
    CHECK(!st.last_error.retryable);
    CHECK_EQ_INT(st.last_error.http_status, 500);

    cog_cli_app_free(app);
}

int main(void)
{
    test_retryable_error_retries();
    test_nonretryable_error_aborts();

    if (failures != 0) {
        (void)fprintf(stderr, "test_16_error_propagation: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_16_error_propagation: all checks passed\n");
    return 0;
}
