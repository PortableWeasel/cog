/**
 * @file test_cli_regression.c
 * @brief Critic regression tests for the CLI application.
 *
 * Covers gaps not exercised by the Writer's suite:
 *  - non-retryable provider errors propagate out of cog_cli_app_run
 *    (fail loudly, no retry) and the process-facing contract holds;
 *  - NULL-pointer handling across the app API surface;
 *  - the durable store preserves both completed and cancelled turn state.
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

/* ------------------------------------------------------------------ */
/* Non-retryable error propagates out of run (fail loudly)             */
/* ------------------------------------------------------------------ */

static void test_nonretryable_error_propagates(void)
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

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hi";
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* A non-retryable error must NOT be retried: run returns the error
     * and the process-facing contract (exit non-zero) holds. */
    CHECK(cog_cli_app_run(app) == COG_ERROR_UNAVAILABLE);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT((long long)st.send_count, 1LL); /* no retry */
    CHECK(!st.last_error.retryable);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* NULL-pointer handling across the app API                            */
/* ------------------------------------------------------------------ */

static void test_null_handling(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;

    (void)memset(&opts, 0, sizeof(opts));
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(NULL) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_submit(NULL, "x", 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_submit(app, NULL, 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_queue(NULL, "x", 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_queue(app, NULL, 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_cancel(NULL) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_shutdown(NULL) == COG_ERROR_NULL_PTR);
    cog_cli_app_stop(NULL); /* must be a no-op */

    /* Accessors on NULL return NULL / zero. */
    CHECK(cog_cli_app_core(NULL) == NULL);
    CHECK(cog_cli_app_session(NULL) == NULL);
    CHECK(cog_cli_app_provider(NULL) == NULL);
    CHECK(cog_cli_app_obs(NULL) == NULL);
    CHECK(cog_cli_app_store(NULL) == NULL);
    CHECK(cog_cli_app_state(NULL) == (cog_state_t)0U);

    /* app_new NULL validation. */
    CHECK(cog_cli_app_new(NULL, &opts) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_app_new(&app, NULL) == COG_ERROR_NULL_PTR);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Durable state: completed and cancelled turns are preserved      */
/* ------------------------------------------------------------------ */

static void test_store_preserves_complete_and_cancelled(void)
{
    char dir_ok[] = "/tmp/cog-cli-reg-store-XXXXXX";
    char dir_cancel[] = "/tmp/cog-cli-reg-store-XXXXXX";
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    const char *sid;

    if (mkdtemp(dir_ok) == NULL || mkdtemp(dir_cancel) == NULL) {
        (void)fprintf(stderr, "mkdtemp failed\n");
        ++failures;
        return;
    }

    /* Completed turn: the application saves the complete aggregate. */
    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.chunks = "persisted";
    opts.store_dir = dir_ok;
    app = make_app(&opts);
    CHECK(app != NULL);
    CHECK(cog_cli_app_run(app) == COG_OK);
    sid = cog_session_id(cog_cli_app_session(app));
    CHECK(sid != NULL);
    CHECK(cog_store_exists(cog_cli_app_store(app), sid));
    cog_cli_app_free(app);

    /* Cancelled turn: explicit CANCELLED state is also saved. A
     * fresh store dir avoids any session-id collision with the run above. */
    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.mock_mode = COG_CLI_MOCK_CANCEL;
    opts.store_dir = dir_cancel;
    app = make_app(&opts);
    CHECK(app != NULL);
    CHECK(cog_cli_app_run(app) == COG_OK);
    sid = cog_session_id(cog_cli_app_session(app));
    CHECK(sid != NULL);
    CHECK(cog_store_exists(cog_cli_app_store(app), sid));
    cog_cli_display_snapshot_t display;
    CHECK(cog_cli_app_display_snapshot(app, &display) == COG_OK);
    CHECK(display.item_count == 2U && display.items[1].status == COG_MESSAGE_CANCELLED);
    cog_cli_app_free(app);

    (void)rmdir(dir_ok);
    (void)rmdir(dir_cancel);
}

int main(void)
{
    test_nonretryable_error_propagates();
    test_null_handling();
    test_store_preserves_complete_and_cancelled();

    if (failures != 0) {
        (void)fprintf(stderr, "test_cli_regression: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_cli_regression: all checks passed\n");
    return 0;
}
