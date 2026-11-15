/**
 * @file test_cli_harness.c
 * @brief Scenario-harness contract test.
 *
 * This is the template for the four scenario batches that follow
 * (Stages 12.6b–12.6e).  It demonstrates the documented harness pattern
 * (see applications/cog-cli/HARNESS.md):
 *
 *   1. Build a @c cog_cli_options_t with a custom @c cog_mock_script_t
 *      (or the chunk/mode options) that scripts the desired chunks,
 *      errors, rate limits, or cancellation.
 *   2. Create the app with @c cog_cli_app_new.
 *   3. Inject prompts with @c cog_cli_app_submit (or @p opts.prompt).
 *   4. Run with @c cog_cli_app_run.
 *   5. Observe the session tree with @c cog_session_get_history.
 *   6. Observe provider state with @c cog_mock_provider_state.
 *   7. Observe app/core state with @c cog_cli_app_state /
 *      @c cog_core_state.
 *
 * Each test below is a self-contained scenario fixture that a scenario
 * writer can copy and extend.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* Harness helpers (the fixture pattern scenario writers reuse)        */
/* ------------------------------------------------------------------ */

/** Build a single-prompt app from options. */
static cog_cli_app_t *fixture_new(cog_cli_options_t *opts)
{
    cog_cli_app_t *app = NULL;

    /* Options built by hand (not via cog_cli_options_parse) need the
     * defaults that parse would have supplied. */
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
    cog_cli_display_snapshot_t display;
    if (cog_cli_app_display_snapshot(app, &display) != COG_OK || display.item_count > cap) return 0U;
    for (size_t i = 0U; i < display.item_count; i++) {
        out[i] = (cog_message_t){.role = display.items[i].role,
            .content = display.items[i].text, .content_len = display.items[i].text_len,
            .status = display.items[i].status};
    }
    return display.item_count;
}

/* ------------------------------------------------------------------ */
/* Scenario 1: user prompt -> streaming -> session history             */
/* ------------------------------------------------------------------ */

static void scenario_streaming(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "The", COG_OK, 0, false, 0U, NULL},
        {COG_MOCK_STEP_CHUNK, " answer", COG_OK, 0, false, 0U, NULL},
        {COG_MOCK_STEP_CHUNK, " is 42.", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 3U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_message_t hist[8];
    size_t n;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "What is the answer?";
    opts.script = &script;
    app = fixture_new(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 19U);
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[1].content_len == 17U);
    CHECK(memcmp(hist[1].content, "The answer is 42.", 17U) == 0);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 1LL);
    CHECK_EQ_INT((long long)st.chunk_count, 3LL);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Scenario 12: rate limit -> backoff -> retry succeeds                */
/* ------------------------------------------------------------------ */

static void scenario_rate_limit(void)
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
    app = fixture_new(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);

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
/* Scenario 20 path A: cancel in-flight, discard partial response      */
/* ------------------------------------------------------------------ */

static void scenario_cancel(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "partial", COG_OK, 0, false, 0U, NULL},
        {COG_MOCK_STEP_CHUNK, " more", COG_OK, 0, false, 0U, NULL},
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
    opts.script = &script;
    app = fixture_new(&opts);
    CHECK(app != NULL);

    /* Submit the prompt first, then cancel: the CANCEL event is
     * dispatched while the turn is streaming (AWAITING_LLM), arming
     * provider cancellation; the next receive aborts and the partial
     * turn is discarded. */
    CHECK(cog_cli_app_submit(app, "explain X", 9U) == COG_OK);
    CHECK(cog_cli_app_cancel(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* Only the user message remains; no orphaned assistant node. */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[1].status == COG_MESSAGE_CANCELLED);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.cancel_count, 1LL);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Scenario 20 path B: queue a message for the next turn               */
/* ------------------------------------------------------------------ */

static void scenario_queue(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "ok", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_message_t hist[8];
    size_t n;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "first";
    opts.script = &script;
    app = fixture_new(&opts);
    CHECK(app != NULL);

    /* Queue a message while the first turn is about to run; it is
     * delivered after the current turn completes. */
    CHECK(cog_cli_app_queue(app, "second", 6U) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);

    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 4LL);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 5U);
    CHECK(memcmp(hist[0].content, "first", 5U) == 0);
    CHECK(hist[2].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[2].content_len == 6U);
    CHECK(memcmp(hist[2].content, "second", 6U) == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Scenario 5: graceful shutdown                                       */
/* ------------------------------------------------------------------ */

static void scenario_shutdown(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;

    (void)memset(&opts, 0, sizeof(opts));
    app = fixture_new(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);

    cog_cli_app_free(app);
}

int main(void)
{
    scenario_streaming();
    scenario_rate_limit();
    scenario_cancel();
    scenario_queue();
    scenario_shutdown();

    if (failures != 0) {
        (void)fprintf(stderr, "test_cli_harness: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_cli_harness: all checks passed\n");
    return 0;
}
