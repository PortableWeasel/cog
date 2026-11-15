/**
 * @file test_12_rate_limit.c
 * @brief Scenario 12 — provider rate limit -> backoff -> queue.
 *
 * The scenario doc (docs/scenarios/12-rate-limit.md) fixes two gaps in
 * the headers: the retry metadata (retryable flag + retry_after_ms) is
 * surfaced through @c cog_provider_error_t, and request queuing during
 * backoff is the application's responsibility.  The CLI retries a
 * retryable error once with a bounded backoff derived from
 * @c retry_after_ms (capped at 5000 ms, see cli.c).
 *
 * What is verified:
 *  1. 429 metadata is surfaced: http_status == 429, retryable == true,
 *     retry_after_ms is reported through the mock provider state.
 *  2. Bounded application backoff: a huge retry_after_ms (60 s) does not
 *     stall the run — the CLI caps the backoff and still retries.
 *  3. No duplicate sends: exactly one original send + one retry
 *     (send_count == 2), never more.
 *  4. The retried request succeeds and the recovered chunk lands in the
 *     session history.
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

static cog_cli_app_t *fixture_new(cog_cli_options_t *opts)
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
/* Scenario 12 — 429 metadata surfaced, retry succeeds, no dup sends   */
/* ------------------------------------------------------------------ */

static void scenario_rate_limit_metadata(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_ERROR, NULL, COG_ERROR_UNAVAILABLE, 429, true,
         5000U, "rate limited"},
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

    /* 429 metadata surfaced through the provider error detail. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK(st.last_error.retryable);
    CHECK_EQ_INT(st.last_error.http_status, 429);
    CHECK_EQ_INT((long long)st.last_error.retry_after_ms, 5000LL);

    /* No duplicate sends: original + one retry. */
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT((long long)st.send_count, 2LL);

    /* The retried request succeeded: recovered chunk in history. */
    n = 0U;
    {
        const cog_message_t *msgs = NULL;
        size_t count = 0U;

        CHECK(cog_session_get_history(cog_cli_app_session(app),
                                      cog_session_active(
                                          cog_cli_app_session(app)),
                                      &msgs, &count) == COG_OK);
        n = count;
        if (count > 8U) {
            count = 8U;
        }
        for (size_t i = 0U; i < count; ++i) {
            hist[i] = msgs[i];
        }
    }
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[1].content_len == 9U);
    CHECK(memcmp(hist[1].content, "recovered", 9U) == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Scenario 12 — bounded backoff: huge retry_after_ms still retries    */
/* ------------------------------------------------------------------ */

static void scenario_rate_limit_bounded_backoff(void)
{
    /* 60 s suggested backoff: the CLI must cap it (5000 ms) so the test
     * completes quickly and still retries exactly once. */
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
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "bounded";
    opts.script = &script;
    app = fixture_new(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT((long long)st.send_count, 2LL); /* no duplicate sends */
    CHECK_EQ_INT((long long)st.last_error.retry_after_ms, 60000LL);

    cog_cli_app_free(app);
}

int main(void)
{
    scenario_rate_limit_metadata();
    scenario_rate_limit_bounded_backoff();

    if (failures != 0) {
        (void)fprintf(stderr, "test_12_rate_limit: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_12_rate_limit: all checks passed\n");
    return 0;
}
