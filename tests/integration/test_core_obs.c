/**
 * @file test_core_obs.c
 * @brief Cross-module integration test for core.c + obs.c.
 *
 * Configures a real cog-obs context (with a capture sink) on a cog-core
 * agent and proves the core emits useful lifecycle and error records
 * through it:
 *
 *   - cog_core_new logs "core.created";
 *   - cog_core_run logs "core.run_start" and, on a clean stop,
 *     "core.run_stop";
 *   - a dispatch failure logs "core.run_error" (and the loop returns the
 *     error);
 *   - cog_core_free logs "core.destroyed".
 *
 * The core logs with no fields, so the obs context needs no field table.
 * The capture sink records every admitted record; the test asserts the
 * expected lifecycle messages appear.  Because cog-obs logging is
 * allocation-free and the core owns no hidden allocations on this path,
 * the test also runs clean under ASan/UBSan/LSan (leak detection).
 *
 * Ticket: dd2f55e (Stage 12.3e — Integrate infrastructure modules and core)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test application vocabulary.                                        */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
};

enum {
    EV_USER_INPUT = COG_EVENT_USER_INPUT,
    EV_UNKNOWN = 0x1234,
};

/* ------------------------------------------------------------------ */
/* Capture sink                                                        */
/* ------------------------------------------------------------------ */

typedef struct sink_capture {
    char buf[8192];
    size_t len;
    size_t calls;
} sink_capture_t;

static cog_result_t capture_sink(void *user, const char *record,
                                 size_t record_len)
{
    sink_capture_t *c = (sink_capture_t *)user;

    ++c->calls;
    if (c->len + record_len + 1U < sizeof(c->buf)) {
        (void)memcpy(c->buf + c->len, record, record_len);
        c->len += record_len;
        c->buf[c->len] = '\n';
        ++c->len;
    }
    return COG_OK;
}

/* The sink descriptor is mutable so each test can point its @c user at a
 * local capture buffer before constructing the obs context. */
static cog_obs_sink_t capture_sink_desc = {
    "capture", capture_sink, NULL, COG_OBS_TRACE
};

static const cog_obs_sink_table_t sink_table = {
    &capture_sink_desc, 1U
};

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

static cog_handler_result_t on_user_input(cog_core_t *core,
                                          const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static const cog_state_t stay_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, on_user_input,
                       stay_transitions),
};

/* ------------------------------------------------------------------ */
/* Wait hook: stop the loop on the first wait.                         */
/* ------------------------------------------------------------------ */

typedef struct wait_ctx {
    cog_core_t *core;
    int calls;
} wait_ctx_t;

static void stop_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;
    ctx->calls++;
    cog_core_stop(ctx->core);
}

/* ------------------------------------------------------------------ */
/* Assertion helpers.                                                  */
/* ------------------------------------------------------------------ */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",    \
                    __FILE__, __LINE__, #a, #b, _a, _b);                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* Lifecycle logging                                                   */
/* ------------------------------------------------------------------ */

static void test_lifecycle_logging(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_obs_config_t obs_cfg;
    cog_obs_t *obs = NULL;
    sink_capture_t capture;
    wait_ctx_t ctx;

    memset(&capture, 0, sizeof(capture));
    capture_sink_desc.user = &capture;
    memset(&obs_cfg, 0, sizeof(obs_cfg));
    obs_cfg.min_severity = COG_OBS_TRACE;
    obs_cfg.sinks = &sink_table;
    CHECK(cog_obs_new(&obs, &obs_cfg) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.obs = obs;
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    CHECK(cog_core_obs(core) == obs);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    /* Clean run: the wait hook stops the loop immediately. */
    CHECK(cog_core_run(core) == COG_OK);
    CHECK_EQ_INT(ctx.calls, 1);

    cog_core_free(core);
    cog_obs_free(obs);
    cog_session_free(session);

    /* The capture sink saw the full lifecycle. */
    CHECK(capture.calls >= 4U);
    CHECK(strstr(capture.buf, "core.created") != NULL);
    CHECK(strstr(capture.buf, "core.run_start") != NULL);
    CHECK(strstr(capture.buf, "core.run_stop") != NULL);
    CHECK(strstr(capture.buf, "core.destroyed") != NULL);
    CHECK(strstr(capture.buf, "core.run_error") == NULL);
}

/* ------------------------------------------------------------------ */
/* Error logging                                                       */
/* ------------------------------------------------------------------ */

static void test_error_logging(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_obs_config_t obs_cfg;
    cog_obs_t *obs = NULL;
    sink_capture_t capture;
    wait_ctx_t ctx;
    cog_event_t ev;

    memset(&capture, 0, sizeof(capture));
    capture_sink_desc.user = &capture;
    memset(&obs_cfg, 0, sizeof(obs_cfg));
    obs_cfg.min_severity = COG_OBS_TRACE;
    obs_cfg.sinks = &sink_table;
    CHECK(cog_obs_new(&obs, &obs_cfg) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.obs = obs;
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    /* An event with no dispatch row fails the loop. */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_UNKNOWN;
    ev.kind = COG_EVENT_SYSTEM_SIGNAL;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_ERROR_NOT_FOUND);

    cog_core_free(core);
    cog_obs_free(obs);
    cog_session_free(session);

    /* The error was logged. */
    CHECK(strstr(capture.buf, "core.run_start") != NULL);
    CHECK(strstr(capture.buf, "core.run_error") != NULL);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_lifecycle_logging();
    test_error_logging();

    if (failures != 0) {
        fprintf(stderr, "test_core_obs: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_core_obs: all checks passed\n");
    return 0;
}
