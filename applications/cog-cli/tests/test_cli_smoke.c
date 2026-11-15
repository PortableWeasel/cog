/**
 * @file test_cli_smoke.c
 * @brief End-to-end smoke tests for the CLI application.
 *
 * Drives the real cog-core / cog-provider / cog-obs / cog-store stack
 * through the cog::cli app API in single-prompt mode and asserts on the
 * session tree, the mock provider state, and the app state.  Covers
 * streaming, queueing, cancellation, error+retry, shutdown, and
 * persistence.
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
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/** Build an app in single-prompt mode with the given options. */
static cog_cli_app_t *make_app(cog_cli_options_t *opts)
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

/** Return the number of messages in the session history. */
static size_t history_count(cog_cli_app_t *app)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_get_history(cog_cli_app_session(app),
                                cog_session_active(cog_cli_app_session(app)),
                                &msgs, &count) != COG_OK) {
        return 0U;
    }
    return count;
}

/** Return the role of history message @p i, or -1 on failure. */
static int history_role(cog_cli_app_t *app, size_t i)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_get_history(cog_cli_app_session(app),
                                cog_session_active(cog_cli_app_session(app)),
                                &msgs, &count) != COG_OK) {
        return -1;
    }
    if (i >= count) {
        return -1;
    }
    return (int)msgs[i].role;
}

/** Return the content of history message @p i, or NULL. */
static const char *history_content(cog_cli_app_t *app, size_t i)
{
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (cog_session_get_history(cog_cli_app_session(app),
                                cog_session_active(cog_cli_app_session(app)),
                                &msgs, &count) != COG_OK) {
        return NULL;
    }
    if (i >= count) {
        return NULL;
    }
    return msgs[i].content;
}

/* ------------------------------------------------------------------ */
/* Streaming flow                                                      */
/* ------------------------------------------------------------------ */

static void test_streaming_flow(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.chunks = "Hello, world,!";
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* Session: user + three assistant chunks. */
    CHECK_EQ_INT((long long)history_count(app), 2LL);
    CHECK(history_role(app, 0U) == COG_MESSAGE_ROLE_USER);
    CHECK(history_content(app, 0U) != NULL &&
          strcmp(history_content(app, 0U), "hello") == 0);
    CHECK(history_role(app, 1U) == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(history_content(app, 1U) != NULL &&
          strcmp(history_content(app, 1U), "Hello world!") == 0);

    /* Provider state: one send, three chunks. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 1LL);
    CHECK_EQ_INT((long long)st.chunk_count, 3LL);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Queue flow (pending message handling)                               */
/* ------------------------------------------------------------------ */

static void test_queue_flow(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "first";
    opts.queue_text = "second";
    opts.chunks = "reply";
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);

    /* Two turns: user, assistant, user, assistant. */
    CHECK_EQ_INT((long long)history_count(app), 4LL);
    CHECK(history_role(app, 0U) == COG_MESSAGE_ROLE_USER);
    CHECK(history_content(app, 0U) != NULL &&
          strcmp(history_content(app, 0U), "first") == 0);
    CHECK(history_role(app, 1U) == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(history_role(app, 2U) == COG_MESSAGE_ROLE_USER);
    CHECK(history_content(app, 2U) != NULL &&
          strcmp(history_content(app, 2U), "second") == 0);
    CHECK(history_role(app, 3U) == COG_MESSAGE_ROLE_ASSISTANT);

    /* Two sends (one per turn). */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 2LL);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

static void test_cancel_flow(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.mock_mode = COG_CLI_MOCK_CANCEL;
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* The user message is preserved; no assistant node. */
    cog_cli_display_snapshot_t display;
    CHECK(cog_cli_app_display_snapshot(app, &display) == COG_OK);
    CHECK(display.item_count == 2U);
    CHECK(display.items[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(display.items[1].status == COG_MESSAGE_CANCELLED);

    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.cancel_count, 1LL);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Error + retry                                                       */
/* ------------------------------------------------------------------ */

static void test_error_retry_flow(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.mock_mode = COG_CLI_MOCK_ERROR;
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);
    CHECK(cog_cli_app_state(app) == (cog_state_t)0); /* IDLE */

    /* User + the retried assistant response. */
    CHECK_EQ_INT((long long)history_count(app), 2LL);
    CHECK(history_role(app, 0U) == COG_MESSAGE_ROLE_USER);
    CHECK(history_role(app, 1U) == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(history_content(app, 1U) != NULL &&
          strcmp(history_content(app, 1U), "retried ok") == 0);

    /* One error, two sends (original + retry). */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT((long long)st.send_count, 2LL);
    CHECK(st.last_error.code == COG_OK);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                            */
/* ------------------------------------------------------------------ */

static void test_shutdown_flow(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;

    (void)memset(&opts, 0, sizeof(opts));
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_shutdown(app) == COG_OK);
    CHECK(cog_cli_app_run(app) == COG_OK);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static void test_store_flow(void)
{
    char dir[] = "/tmp/cog-cli-store-XXXXXX";
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    const char *sid;

    if (mkdtemp(dir) == NULL) {
        (void)fprintf(stderr, "mkdtemp failed\n");
        ++failures;
        return;
    }

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.chunks = "persisted";
    opts.store_dir = dir;
    app = make_app(&opts);
    CHECK(app != NULL);

    CHECK(cog_cli_app_run(app) == COG_OK);

    /* The store auto-saved the session after the completed turn. */
    sid = cog_session_id(cog_cli_app_session(app));
    CHECK(sid != NULL);
    CHECK(cog_store_exists(cog_cli_app_store(app), sid));

    cog_cli_app_free(app);
    (void)rmdir(dir);
}

/* ------------------------------------------------------------------ */
/* Handler context via cog_core_user (no global app pointer)           */
/* ------------------------------------------------------------------ */

static void test_handler_context(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;
    cog_mock_state_t st;

    (void)memset(&opts, 0, sizeof(opts));
    opts.prompt = "hello";
    opts.chunks = "ctx";
    opts.model_id = "ctx-model-7"; /* distinctive: proves the handler read
                                      the app context, not a default */
    app = make_app(&opts);
    CHECK(app != NULL);

    /* The app context is threaded through the core's borrowed user
     * pointer, not a module global. */
    CHECK(cog_core_user(cog_cli_app_core(app)) == (void *)app);

    CHECK(cog_cli_app_run(app) == COG_OK);

    /* The USER_INPUT handler built the request from app->opts.model_id
     * retrieved via cog_core_user; a stale or missing context would have
     * fallen back to the default model id. */
    CHECK(cog_mock_provider_state(cog_cli_app_provider(app), &st) == COG_OK);
    CHECK(st.last_request != NULL);
    CHECK(st.last_request->model_id != NULL &&
          strcmp(st.last_request->model_id, "ctx-model-7") == 0);

    cog_cli_app_free(app);
}

/* ------------------------------------------------------------------ */
/* Unknown endpoint error                                              */
/* ------------------------------------------------------------------ */

static void test_unknown_endpoint(void)
{
    cog_cli_options_t opts;
    cog_cli_app_t *app = NULL;

    (void)memset(&opts, 0, sizeof(opts));
    opts.model_id = "mock-model";
    opts.endpoint_name = "openai";
    CHECK(cog_cli_app_new(&app, &opts) == COG_ERROR_NOT_FOUND);
    CHECK(app == NULL);
}

int main(void)
{
    test_streaming_flow();
    test_queue_flow();
    test_cancel_flow();
    test_error_retry_flow();
    test_shutdown_flow();
    test_store_flow();
    test_handler_context();
    test_unknown_endpoint();

    if (failures != 0) {
        (void)fprintf(stderr, "test_cli_smoke: %d failure(s)\n", failures);
        return 1;
    }
    (void)printf("test_cli_smoke: all checks passed\n");
    return 0;
}
