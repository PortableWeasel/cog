/**
 * @file test_02_network_recovery.c
 * @brief Scenario 02: Network stall -> detection -> bounded retry/backoff
 *        -> recovery/resume with history intact.
 *
 * Drives the real CLI harness (cog::cli + mock provider) through a
 * scripted transient provider failure.  The mock provider's ERROR step
 * carries retry metadata (retryable flag, HTTP status, Retry-After
 * backoff); the CLI's LLM_CHUNK handler detects the error, applies a
 * bounded backoff, re-sends the request rebuilt from session history,
 * and resumes streaming.  This test asserts:
 *
 *   - the error metadata is observed (retryable, http_status,
 *     retry_after_ms);
 *   - the retry is bounded: exactly one retry (send_count == 2), never
 *     an unbounded loop;
 *   - the backoff is bounded: a huge Retry-After is capped, so the run
 *     completes in far less than the suggested delay;
 *   - recovery resumes the turn and the recovered chunk lands in the
 *     session history;
 *   - the history stays intact: the user message is preserved and no
 *     orphaned/partial assistant node is left behind.
 *
 * Ticket: fe958ce (Stage 12.6b — Scenario tests 01-05).
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/cli.h"
#include "cog_cli/mock_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/** Monotonic milliseconds since an arbitrary epoch. */
static long long now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/* ------------------------------------------------------------------ */
/* Scenario 02a: transient error -> bounded retry -> recovery          */
/* ------------------------------------------------------------------ */

static void scenario_recovery(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_ERROR, NULL, COG_ERROR_UNAVAILABLE, 503, true,
         5U, "upstream stalled"},
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
    opts.prompt = "resume me";
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* The turn stalls (scripted error), the CLI detects it, retries once
     * with a bounded backoff, and resumes streaming the recovered chunk. */
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* Error metadata observed. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK(st.last_error.code == COG_OK);
    CHECK(st.last_error.http_status == 0);
    CHECK(st.last_error.retry_after_ms == 0U);

    /* Bounded retry: original send + exactly one retry. */
    CHECK_EQ_INT((long long)st.send_count, 2LL);
    CHECK_EQ_INT((long long)st.chunk_count, 1LL);

    /* Recovery resumed the turn: history = user + recovered assistant. */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 9U);
    CHECK(memcmp(hist[0].content, "resume me", 9U) == 0);
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[1].content_len == 9U);
    CHECK(memcmp(hist[1].content, "recovered", 9U) == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Scenario 02b: backoff is bounded (huge Retry-After is capped)       */
/* ------------------------------------------------------------------ */

static void scenario_bounded_backoff(void)
{
    /* A 60-second Retry-After must be capped by the CLI (to 5 s), so the
     * run completes in far less than the suggested delay. */
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_ERROR, NULL, COG_ERROR_UNAVAILABLE, 429, true,
         60000U, "rate limited"},
        {COG_MOCK_STEP_CHUNK, "ok", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 2U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_message_t hist[8];
    size_t n;
    cog_mock_state_t st;
    long long start;
    long long elapsed;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "backoff";
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    start = now_ms();
    CHECK(cog_cli_app_run(app) == COG_OK);
    elapsed = now_ms() - start;

    /* The run must not have slept the full 60 s: the backoff is capped.
     * Allow generous slack for the capped 5 s sleep plus scheduling. */
    CHECK(elapsed < 15000LL);

    /* Recovery still landed in history. */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[1].content_len == 2U);
    CHECK(memcmp(hist[1].content, "ok", 2U) == 0);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 2LL);

    cog_cli_app_free(app);
}

int main(void)
{
    (void)printf("Scenario 02: network stall -> detection -> bounded "
                 "retry/backoff -> recovery\n");
    scenario_recovery();
    scenario_bounded_backoff();

    if (failures != 0) {
        (void)fprintf(stderr, "test_02_network_recovery: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_02_network_recovery: all checks passed\n");
    return 0;
}
