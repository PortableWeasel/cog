/**
 * @file test_01_user_prompt.c
 * @brief Scenario 01: User prompt -> LLM -> streaming -> UI.
 *
 * Drives the real cog-core / cog-provider / cog-obs stack through the
 * cog::cli app API (the same harness pattern documented in
 * applications/cog-cli/HARNESS.md and demonstrated by
 * test_cli_harness.c).  It verifies the full end-to-end flow:
 *
 *   1. a user prompt is submitted as a COG_EVENT_USER_INPUT;
 *   2. the IDLE handler appends the user message and sends the request
 *      to the provider (cog_provider_send);
 *   3. the provider streams scripted chunks back (cog_provider_receive),
 *      each appended to the session tree;
 *   4. the final chunk completes the turn (LLM_COMPLETE) and the state
 *      machine returns to IDLE;
 *   5. the session history holds the user message plus one assistant
 *      node per chunk, and the UI-visible output (the app's stdout
 *      stream) carries the concatenated response.
 *
 * This is a black-box scenario test: it uses only the public CLI and
 * mock-provider APIs, never the private core internals.
 *
 * Ticket: fe958ce (Stage 12.6b — Scenario tests 01-05).
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

/* ------------------------------------------------------------------ */
/* UI-visible output capture                                           */
/* ------------------------------------------------------------------ */

static int saved_stdout = -1;
static char out_path[64];

/** Redirect the process stdout to a temp file so the app's assistant
 *  output (app->out == stdout) can be inspected after the run. */
static int begin_capture(void)
{
    int fd;

    (void)snprintf(out_path, sizeof(out_path), "/tmp/cog-sc01-XXXXXX");
    fd = mkstemp(out_path);
    if (fd < 0) {
        return -1;
    }
    (void)fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        (void)close(fd);
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) < 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);
    return 0;
}

/** Restore stdout and return the captured bytes (NUL-terminated). */
static size_t end_capture(char *buf, size_t cap)
{
    size_t n = 0U;
    FILE *f;

    (void)fflush(stdout);
    if (saved_stdout >= 0) {
        (void)dup2(saved_stdout, STDOUT_FILENO);
        (void)close(saved_stdout);
        saved_stdout = -1;
    }
    f = fopen(out_path, "rb");
    if (f != NULL) {
        n = fread(buf, 1U, cap - 1U, f);
        buf[n] = '\0';
        (void)fclose(f);
    }
    (void)unlink(out_path);
    return n;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

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
/* Scenario 01: user prompt -> streaming -> session history + UI       */
/* ------------------------------------------------------------------ */

static void scenario_user_prompt(void)
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
    char ui[256];
    size_t ui_len;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "What is the answer?";
    opts.script = &script;
    app = make_app(&opts);
    CHECK(app != NULL);

    /* Capture the UI-visible output stream for the whole run. */
    CHECK(begin_capture() == 0);

    /* Step 1-4: run the full prompt -> send -> stream -> complete flow. */
    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    ui_len = end_capture(ui, sizeof(ui));

    /* Step 5a: session history = user + one assistant node per chunk. */
    n = fixture_history(app, hist, 8U);
    CHECK_EQ_INT((long long)n, 4LL);
    CHECK(hist[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(hist[0].content_len == 19U);
    CHECK(memcmp(hist[0].content, "What is the answer?", 19U) == 0);
    CHECK(hist[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[1].content_len == 3U);
    CHECK(memcmp(hist[1].content, "The", 3U) == 0);
    CHECK(hist[2].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[2].content_len == 7U);
    CHECK(memcmp(hist[2].content, " answer", 7U) == 0);
    CHECK(hist[3].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(hist[3].content_len == 7U);
    CHECK(memcmp(hist[3].content, " is 42.", 7U) == 0);

    /* Step 5b: provider was asked once and streamed exactly the chunks. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 1LL);
    CHECK_EQ_INT((long long)st.chunk_count, 3LL);
    CHECK_EQ_INT((long long)st.error_count, 0LL);
    CHECK(st.last_request != NULL);
    CHECK(st.last_request->stream);
    CHECK(st.last_request->message_count == 1U); /* only the user msg */

    /* Step 5c: the UI-visible output carries the concatenated response. */
    CHECK(ui_len > 0U);
    CHECK(strstr(ui, "The answer is 42.") != NULL);

    cog_cli_app_free(app);
}

int main(void)
{
    (void)printf("Scenario 01: user prompt -> LLM -> streaming -> UI\n");
    scenario_user_prompt();

    if (failures != 0) {
        (void)fprintf(stderr, "test_01_user_prompt: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_01_user_prompt: all checks passed\n");
    return 0;
}
