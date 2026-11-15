/**
 * @file test_05b_shutdown_cancel_contract.c
 * @brief Critic regression test: scenario 05 documented shutdown contract.
 *
 * docs/scenarios/05-graceful-shutdown.md ("Graceful Shutdown with
 * In-Flight Work") documents the shutdown flow as:
 *
 *   signal stop -> COG_EVENT_SHUTDOWN -> cancel in-flight requests
 *   (cog_net_cancel) -> broadcast -> disconnect provider -> close IPC
 *   -> free net/session/core.
 *
 * The Writer's test_05_graceful_shutdown.c pins the CLI's current
 * early-stop behaviour (the SHUTDOWN handler stops the run loop without
 * cancelling the in-flight provider request), which blesses a missing
 * requirement instead of validating the documented scenario.  This test
 * pins the documented contract for the single-process CLI (the parts
 * that are implementable today):
 *
 *   A. Shutdown with an in-flight turn:
 *      - the provider was asked (send_count == 1: in-flight work
 *        existed);
 *      - the in-flight request was cancelled (mock cancel armed or a
 *        cancelled receive observed) — the scenario's "cancel requests"
 *        step;
 *      - the submitted user message is preserved (no loss of user work);
 *      - the state machine is back in a consistent state (IDLE), not
 *        left mid-turn in AWAITING_LLM with an abandoned request.
 *
 *   B. Shutdown with nothing pending: run returns COG_OK promptly and
 *      the app stays IDLE.
 *
 * @note As of the Writer's submission (66cbfd5) this test FAILS on
 *       path A: the CLI's cli_on_shutdown (applications/cog-cli/src/
 *       cli.c) calls cog_core_stop without arming provider cancellation,
 *       so the in-flight request is abandoned (cancel_count == 0,
 *       cancel_requested == false) and the app is left in AWAITING_LLM.
 *       The fix belongs in the CLI shutdown handler (arm
 *       cog_mock_provider_request_cancel before stopping, mirroring
 *       cli_on_cancel) and is out of the Writer batch's scope; this test
 *       is the committed evidence of the gap.
 *
 * Ticket: fe958ce (Stage 12.6b — Scenario tests 01-05), review fc9db9a.
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
/* Path A: shutdown with an in-flight turn must cancel the request     */
/* ------------------------------------------------------------------ */

static void scenario_shutdown_cancels_inflight(void)
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
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Start an in-flight turn, then request shutdown. */
    CHECK(cog_cli_app_submit(app, "finish me", 9U) == COG_OK);
    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);

    /* The in-flight turn was actually started: the provider was asked. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 1LL);

    /* Documented "cancel requests" step: the in-flight request must be
     * cancelled, not abandoned. */
    CHECK(st.cancel_requested || st.cancel_count > 0U);

    /* No loss of user work. */
    n = fixture_history(app, hist, 8U);
    CHECK(n >= 1U);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 9U);
    CHECK(memcmp(hist[0].content, "finish me", 9U) == 0);

    /* Consistent final state: the turn is closed, not left mid-flight. */
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

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

    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0);

    cog_cli_app_free(app);
}

int main(void)
{
    (void)printf("Scenario 05b: documented shutdown contract — in-flight "
                 "work must be cancelled\n");
    scenario_shutdown_cancels_inflight();
    scenario_shutdown_idle();

    if (failures != 0) {
        (void)fprintf(stderr,
                      "test_05b_shutdown_cancel_contract: %d failure(s) — "
                      "the CLI shutdown handler does not cancel in-flight "
                      "work (see file header)\n",
                      failures);
        return 1;
    }
    (void)printf("test_05b_shutdown_cancel_contract: all checks passed\n");
    return 0;
}
