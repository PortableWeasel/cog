/**
 * @file test_04_cold_start.c
 * @brief Scenario 04: Cold start -> init -> first event.
 *
 * Verifies that a freshly constructed CLI app (the real cog-core /
 * cog-provider / cog-obs stack) starts in a correct, well-defined
 * initial state with all resources wired, and that the first event
 * (a user prompt) is dispatched correctly.
 *
 * Before the first event:
 *   - the core exists and is in the initial IDLE state;
 *   - the session exists, is empty, and has a non-NULL id;
 *   - the provider is connected (not disconnected, zero sends);
 *   - the obs context exists;
 *   - the app context is threaded through the core's borrowed user
 *     pointer.
 *
 * After the first event:
 *   - the user message is appended and the provider is asked once;
 *   - the streamed response completes and the state machine returns to
 *     IDLE.
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

/* ------------------------------------------------------------------ */
/* Scenario 04: cold start -> init -> first event                      */
/* ------------------------------------------------------------------ */

static void scenario_cold_start(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "booted", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    cog_session_t *session;
    cog_provider_t *provider;
    cog_obs_t *obs;
    cog_mock_state_t st;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    (void)memset(&opts, 0, sizeof(opts));
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Initial resources are all wired. */
    core = cog_cli_app_core(app);
    session = cog_cli_app_session(app);
    provider = cog_cli_app_provider(app);
    obs = cog_cli_app_obs(app);
    CHECK(core != NULL);
    CHECK(session != NULL);
    CHECK(provider != NULL);
    CHECK(obs != NULL);

    /* Initial state: IDLE, empty session with an id, provider connected
     * and idle, app context threaded through the core. */
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */
    CHECK(cog_session_is_empty(session));
    CHECK(cog_session_id(session) != NULL);
    CHECK(cog_core_user(core) == (void *)app);
    CHECK(cog_mock_provider_state(provider, &st) == COG_OK);
    CHECK(!st.disconnected);
    CHECK_EQ_INT((long long)st.send_count, 0LL);
    CHECK_EQ_INT((long long)st.chunk_count, 0LL);

    /* First event: a user prompt. */
    CHECK(cog_cli_app_submit(app, "first", 5U) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* back to IDLE */

    /* The first event was dispatched: user + assistant in history. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 2LL);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[0].content_len == 5U);
    CHECK(memcmp(msgs[0].content, "first", 5U) == 0);
    CHECK(msgs[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[1].content_len == 6U);
    CHECK(memcmp(msgs[1].content, "booted", 6U) == 0);

    /* The provider was asked exactly once for the first event. */
    CHECK(cog_mock_provider_state(provider, &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 1LL);
    CHECK_EQ_INT((long long)st.chunk_count, 1LL);

    cog_cli_app_free(app);
}

int main(void)
{
    (void)printf("Scenario 04: cold start -> init -> first event\n");
    scenario_cold_start();

    if (failures != 0) {
        (void)fprintf(stderr, "test_04_cold_start: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_04_cold_start: all checks passed\n");
    return 0;
}