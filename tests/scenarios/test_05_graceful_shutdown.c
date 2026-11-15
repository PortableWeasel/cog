/**
 * @file test_05_graceful_shutdown.c
 * @brief Scenario 05: Graceful shutdown with in-flight / pending work.
 *
 * Verifies that a shutdown request (COG_EVENT_SHUTDOWN) stops the run
 * loop cleanly — without loss of in-flight work, without hanging, and
 * without leaks (the binary runs under ASan/UBSan).
 *
 * Paths exercised:
 *
 *   A. In-flight turn: a prompt is submitted and then shutdown is
 *      requested.  The USER_INPUT handler starts the turn (appending the
 *      user message and sending to the provider), then the SHUTDOWN
 *      event stops the loop.  The submitted user message is preserved
 *      (no loss of user work), run returns COG_OK (no hang), and the
 *      app frees cleanly (no leak).
 *
 *   B. Nothing pending: shutdown is requested on an idle app.  Run
 *      returns COG_OK promptly (no hang).
 *
 *   C. Pending queued message: a prompt is submitted, a second message
 *      is queued for the next turn, and shutdown is requested.  The
 *      loop stops before the queued message is processed; the submitted
 *      user message is preserved and run returns COG_OK.
 *
 * In every path the app is freed afterwards; ASan/UBSan report any leak
 * or use-after-free.
 *
 * @note The current single-process CLI's SHUTDOWN handler stops the run
 *       loop immediately rather than draining in-flight LLM work to
 *       completion.  The submitted user message is preserved, so there
 *       is no loss of user work, but an in-flight assistant response is
 *       not produced.  Draining in-flight work to completion is the
 *       orchestrator layer's job (Stage 12.5) and is not yet integrated;
 *       this test pins the current graceful-shutdown contract (no loss
 *       of user input, no hang, no leak).
 *
 * Ticket: fe958ce (Stage 12.6b — Scenario tests 01-05).
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
    if (cog_cli_app_new(&app, opts) != COG_OK) {
        return NULL;
    }
    return app;
}

/** Copy the session history into a caller array (max @p cap). */
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
/* Path A: shutdown with an in-flight turn                             */
/* ------------------------------------------------------------------ */

static void scenario_shutdown_inflight(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "done", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_message_t hist[8];
    size_t n;

    (void)memset(&opts, 0, sizeof(opts));
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Submit the prompt (in-flight turn), then request shutdown.  The
     * USER_INPUT handler starts the turn; the SHUTDOWN event then stops
     * the loop before streaming completes.  The submitted user message
     * is preserved (no loss of user work). */
    CHECK(cog_cli_app_submit(app, "finish me", 9U) == COG_OK);
    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);

    /* No loss: the submitted user message is preserved.  The in-flight
     * assistant response is not produced (shutdown stops promptly; see
     * the file header note). */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 1LL);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 9U);
    CHECK(memcmp(hist[0].content, "finish me", 9U) == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Path B: shutdown with nothing pending                               */
/* ------------------------------------------------------------------ */

static void scenario_shutdown_idle(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;

    (void)memset(&opts, 0, sizeof(opts));
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Nothing in flight: shutdown stops the loop immediately. */
    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Path C: shutdown with a pending queued message                      */
/* ------------------------------------------------------------------ */

static void scenario_shutdown_pending(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "first reply", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_message_t hist[8];
    size_t n;

    (void)memset(&opts, 0, sizeof(opts));
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Submit a prompt, queue a second message for the next turn, then
     * request shutdown.  The loop stops before the queued message is
     * processed; the submitted user message is preserved. */
    CHECK(cog_cli_app_submit(app, "first", 5U) == COG_OK);
    CHECK(cog_cli_app_queue(app, "second", 6U) == COG_OK);
    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);

    /* The submitted user message is preserved; the queued second message
     * was not processed (the loop stopped first). */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 1LL);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 5U);
    CHECK(memcmp(hist[0].content, "first", 5U) == 0);

    cog_cli_app_free(app);
}

int main(void)
{
    (void)printf("Scenario 05: graceful shutdown with in-flight/pending "
                 "work\n");
    scenario_shutdown_inflight();
    scenario_shutdown_idle();
    scenario_shutdown_pending();

    if (failures != 0) {
        (void)fprintf(stderr, "test_05_graceful_shutdown: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_05_graceful_shutdown: all checks passed\n");
    return 0;
}