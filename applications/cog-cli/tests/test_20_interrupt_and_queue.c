/**
 * @file test_20_interrupt_and_queue.c
 * @brief Scenario 20: user interrupt and queued message.
 *
 * Path A (immediate interrupt): the user submits a prompt and cancels
 * while the turn is streaming.  The partial assistant response is
 * retained as an explicit cancelled aggregate, and the session
 * tree preserves both user work and cancellation evidence, and the core returns to IDLE.
 *
 * Path B (queued message): a message queued while a turn is pending is
 * auto-injected as the next turn after the current one completes.  Both
 * turns appear in the history in order, with no orphaned nodes.
 *
 * The pending-message FIFO is also exercised directly through the real
 * core API (cog_core_queue_message / cog_core_pending_count /
 * cog_core_dequeue_message).
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
/* Path A: immediate interrupt retains the cancelled assistant aggregate     */
/* ------------------------------------------------------------------ */

static void test_interrupt_retains_cancelled(void)
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
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Submit the prompt first, then cancel: the CANCEL event is
     * dispatched while the turn is streaming, arming provider
     * cancellation; the next receive aborts and the partial turn is
     * discarded. */
    CHECK(cog_cli_app_submit(app, "explain X", 9U) == COG_OK);
    CHECK(cog_cli_app_cancel(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* The user message and cancelled assistant aggregate remain visible. */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 2LL);
    CHECK(hist[1].status == COG_MESSAGE_CANCELLED);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 9U);
    CHECK(memcmp(hist[0].content, "explain X", 9U) == 0);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.cancel_count, 1LL);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Path B: queued message auto-injects after the current turn          */
/* ------------------------------------------------------------------ */

static void test_queue_auto_injects(void)
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
    app = make_app(&opts);
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
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[2].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[2].content_len == 6U);
    CHECK(memcmp(hist[2].content, "second", 6U) == 0);
    CHECK(hist[3].role == COG_MESSAGE_ROLE_ASSISTANT);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Pending-message FIFO via the real core API                          */
/* ------------------------------------------------------------------ */

static void test_pending_fifo(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "ok", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_core_t *core;
    const char *sid;
    char buf[64];
    size_t len;
    size_t count;

    (void)memset(&opts, 0, sizeof(opts));
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);
    core = cog_cli_app_core(app);
    sid = cog_session_id(cog_cli_app_session(app));
    CHECK(core != NULL);
    CHECK(sid != NULL);

    /* Queue two messages; the FIFO holds them in order. */
    CHECK(cog_core_queue_message(core, sid, "alpha", 5U) == COG_OK);
    CHECK(cog_core_queue_message(core, sid, "beta", 4U) == COG_OK);
    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 2LL);

    /* Dequeue in FIFO order. */
    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) == COG_OK);
    CHECK_EQ_INT((long long)len, 5LL);
    CHECK(memcmp(buf, "alpha", 5U) == 0);
    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) == COG_OK);
    CHECK_EQ_INT((long long)len, 4LL);
    CHECK(memcmp(buf, "beta", 4U) == 0);

    /* Empty queue. */
    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT((long long)count, 0LL);
    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) ==
          COG_ERROR_NOT_FOUND);

    cog_cli_app_free(app);
}

int main(void)
{
    test_interrupt_retains_cancelled();
    test_queue_auto_injects();
    test_pending_fifo();

    if (failures != 0) {
        (void)fprintf(stderr, "test_20_interrupt_and_queue: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_20_interrupt_and_queue: all checks passed\n");
    return 0;
}
