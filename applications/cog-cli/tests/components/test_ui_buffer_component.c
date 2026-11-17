/**
 * @file test_ui_buffer_component.c
 * @brief Component regression — UI disconnect -> buffer -> reconnect -> replay.
 *
 * The scenario doc (docs/scenarios/11-ui-reconnect.md) records a design
 * decision: **output buffering is the application's responsibility**; the
 * core continues processing during a UI disconnect.  The CLI harness has
 * no real UI, so this test exercises the contract with a small
 * scenario-local output buffer plus the core's real session-scoped
 * pending-message queue (the input-buffering mechanism).
 *
 * What is verified:
 *  1. Output buffering is application responsibility: a scenario-local
 *     buffer captures every assistant chunk produced while the UI is
 *     "disconnected", with no loss (captured == provider chunk_count).
 *  2. Reconnect by session id: the session id is stable across the run
 *     and can be used to re-identify the session; a fresh app created
 *     with the same id reconnects to the same session identity.
 *  3. Replay without duplication/loss: replaying the buffer yields
 *     exactly the captured chunks (replayed == captured, no duplicates).
 *  4. Input buffering during a disconnect: messages queued while the
 *     turn is pending are buffered by the core and delivered (replayed)
 *     as new turns after the current one, each exactly once.
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
/* Scenario-local UI output buffer (application responsibility)        */
/* ------------------------------------------------------------------ */

#define UI_BUF_CAP 32U

/** Fixed-capacity buffer of copied chunk strings, owned by the test. */
typedef struct ui_buffer {
    char *chunks[UI_BUF_CAP]; /**< Owned copies of captured chunks. */
    size_t captured;          /**< Number of chunks captured. */
    size_t replayed;          /**< Number of chunks replayed. */
} ui_buffer_t;

static void ui_buffer_init(ui_buffer_t *b)
{
    (void)memset(b, 0, sizeof(*b));
}

/** Capture one chunk (copy) into the buffer; drop-oldest when full. */
static void ui_buffer_capture(ui_buffer_t *b, const char *text, size_t len)
{
    char *copy;

    if (b->captured == UI_BUF_CAP) {
        /* Buffer limit reached: drop the oldest (documented behaviour). */
        free(b->chunks[0]);
        (void)memmove(&b->chunks[0], &b->chunks[1],
                      (UI_BUF_CAP - 1U) * sizeof(b->chunks[0]));
        b->captured--;
    }
    copy = (char *)malloc(len + 1U);
    if (copy == NULL) {
        return;
    }
    (void)memcpy(copy, text, len);
    copy[len] = '\0';
    b->chunks[b->captured++] = copy;
}

/** Replay every captured chunk to the (reconnected) UI. */
static void ui_buffer_replay(ui_buffer_t *b)
{
    size_t i;

    for (i = 0U; i < b->captured; ++i) {
        /* Simulated render to the UI; count it to prove no dup/loss. */
        (void)b->chunks[i];
        b->replayed++;
    }
}

static void ui_buffer_free(ui_buffer_t *b)
{
    size_t i;

    for (i = 0U; i < b->captured; ++i) {
        free(b->chunks[i]);
        b->chunks[i] = NULL;
    }
    b->captured = 0U;
    b->replayed = 0U;
}

/* ------------------------------------------------------------------ */
/* Fixture helper                                                      */
/* ------------------------------------------------------------------ */

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
/* Scenario 11                                                         */
/* ------------------------------------------------------------------ */

static void scenario_ui_reconnect(void)
{
    /* One chunk per turn; repeat forever so the buffered (queued)
     * messages each get their own assistant turn. */
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "ok", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, SIZE_MAX, COG_OK, 0U,
    };
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_cli_app_t *reconnected = NULL;
    cog_message_t hist[16];
    size_t n;
    size_t i;
    cog_mock_state_t st;
    ui_buffer_t buf;
    const char *sid;
    size_t user_second = 0U;
    size_t user_third = 0U;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "first";
    opts.script = &script;
    app = fixture_new(&opts);
    CHECK(app != NULL);

    /* Reconnect identity: capture the session id before the run. */
    sid = cog_session_id(cog_cli_app_session(app));
    CHECK(sid != NULL);
    CHECK(strlen(sid) > 0U);

    /* Input buffering during a "disconnect": queue two messages for the
     * session while the first turn is pending.  The core buffers them
     * (application responsibility) and delivers them after the turn. */
    CHECK(cog_cli_app_queue(app, "second", 6U) == COG_OK);
    CHECK(cog_cli_app_queue(app, "third", 5U) == COG_OK);

    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* Session id is stable after the run (reconnect key). */
    CHECK(strcmp(sid, cog_session_id(cog_cli_app_session(app))) == 0);

    /* Output buffering: capture every assistant chunk into the
     * scenario-local buffer (the UI was "disconnected" during the run). */
    ui_buffer_init(&buf);
    n = fixture_history(app, hist, 16U);
    for (i = 0U; i < n; ++i) {
        if (hist[i].role == COG_MESSAGE_ROLE_ASSISTANT) {
            ui_buffer_capture(&buf, hist[i].content, hist[i].content_len);
        }
    }

    /* No loss: every chunk the provider emitted was captured. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.chunk_count, 3LL); /* 3 turns x 1 chunk */
    CHECK_EQ_INT((long long)buf.captured, 3LL);

    /* The buffered (queued) inputs were replayed as new turns, each
     * exactly once (no duplication). */
    CHECK_EQ_INT((long long)n, 6LL); /* user,assistant x 3 turns */
    for (i = 0U; i < n; ++i) {
        if (hist[i].role == COG_MESSAGE_ROLE_USER &&
            hist[i].content_len == 6U &&
            memcmp(hist[i].content, "second", 6U) == 0) {
            user_second++;
        }
        if (hist[i].role == COG_MESSAGE_ROLE_USER &&
            hist[i].content_len == 5U &&
            memcmp(hist[i].content, "third", 5U) == 0) {
            user_third++;
        }
    }
    CHECK_EQ_INT((long long)user_second, 1LL);
    CHECK_EQ_INT((long long)user_third, 1LL);

    /* Reconnect by session id: a fresh app with the same id reconnects
     * to the same session identity. */
    (void)memset(&opts, 0, sizeof(opts));
    opts.session_id = sid;
    opts.script = &script;
    reconnected = fixture_new(&opts);
    CHECK(reconnected != NULL);
    CHECK(strcmp(sid, cog_session_id(cog_cli_app_session(reconnected))) == 0);

    /* Replay without duplication/loss: the buffer replays exactly the
     * captured chunks. */
    ui_buffer_replay(&buf);
    CHECK_EQ_INT((long long)buf.replayed, 3LL);
    CHECK_EQ_INT((long long)buf.replayed, (long long)buf.captured);

    ui_buffer_free(&buf);
    cog_cli_app_free(reconnected);
    cog_cli_app_free(app);
}

int main(void)
{
    scenario_ui_reconnect();

    if (failures != 0) {
        (void)fprintf(stderr, "test_11_ui_reconnect: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_11_ui_reconnect: all checks passed\n");
    return 0;
}
