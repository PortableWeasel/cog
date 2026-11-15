/**
 * @file test_core.c
 * @brief Contract test for cog/core.h implemented by cog-core/src/core.c.
 *
 * core.h declares the agent core: construction/destruction, the run loop,
 * stop, enqueue, the state/running inspectors, the provider/obs/store/ipc
 * accessors, and the session-scoped pending-message FIFO.
 *
 * This test verifies at run time:
 *
 *   - construction validation (NULL out/config, NULL session, NULL
 *     dispatch table, zero table size, NOMEM on oversized queue)
 *   - destruction (NULL no-op; session detached so it can be reset)
 *   - the run loop: dispatch through the static table, handler
 *     invocation, transition application, handler-status propagation,
 *     transition-denied propagation, unhandled (no row) propagation,
 *     empty-queue wait-hook behaviour, and clean stop
 *   - the already-running guard (re-entrant cog_core_run)
 *   - enqueue (NULL checks, overflow on a full queue)
 *   - the inspectors and accessors (NULL core and non-NULL core)
 *   - the pending-message FIFO: queue/dequeue/pending_count, NULL checks,
 *     bounded capacity overflow, not-found, buffer sizing, FIFO order,
 *     and per-session isolation
 *   - automatic dequeue + re-injection of pending messages after a turn
 *     completes, verified through session history
 *
 * Ticket: 681a566 (Stage 12.2a — Implement core.c)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef COG_CORE_H
#error "COG_CORE_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Test application vocabulary (per state.h/event.h conventions).      */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
    ST_RUNNING = 1,
    ST_DONE = 2,
};

/* Event ids used by the dispatch table.  USER_INPUT and LLM_COMPLETE map
 * one-to-one to their kinds; the rest are application-defined ids. */
enum {
    EV_USER_INPUT = COG_EVENT_USER_INPUT,
    EV_LLM_COMPLETE = COG_EVENT_LLM_COMPLETE,
    EV_FAIL = 0x2000,
    EV_BADTRANS = 0x2001,
    EV_UNKNOWN = 0x1234,
    EV_BLOCK = 0x2002,
};

/* ------------------------------------------------------------------ */
/* Handlers wired into the static dispatch table.                      */
/* ------------------------------------------------------------------ */

/**
 * ST_IDLE + USER_INPUT: append the event payload as a user message, then
 * enqueue an LLM_COMPLETE to simulate a turn, and go RUNNING.
 */
static cog_handler_result_t on_user_input(cog_core_t *core,
                                          const cog_event_t *event)
{
    cog_message_t msg;
    cog_event_t complete;
    cog_handler_result_t r = {ST_RUNNING, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    msg.role = COG_MESSAGE_ROLE_USER;
    msg.content = (const char *)event->payload;
    msg.content_len = event->payload_len;
    msg.name = NULL;
    if (cog_session_append(cog_core_session(core), &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    complete.id = (cog_event_id_t)EV_LLM_COMPLETE;
    complete.kind = COG_EVENT_LLM_COMPLETE;
    complete.payload = NULL;
    complete.payload_len = 0U;
    complete.source_id = 0U;
    if (cog_core_enqueue(core, &complete) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** ST_RUNNING + LLM_COMPLETE: append an assistant reply and go IDLE. */
static cog_handler_result_t on_llm_complete(cog_core_t *core,
                                            const cog_event_t *event)
{
    static const cog_message_t asst = {
        COG_MESSAGE_ROLE_ASSISTANT, "ok", 2U, NULL
    };
    cog_handler_result_t r = {ST_IDLE, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    if (cog_session_append(cog_core_session(core), &asst, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** ST_IDLE + EV_FAIL: report a handler failure. */
static cog_handler_result_t on_fail(cog_core_t *core, const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){ST_IDLE, COG_ERROR_IO};
}

/** ST_IDLE + EV_BADTRANS: request a transition that is not allowed. */
static cog_handler_result_t on_badtrans(cog_core_t *core,
                                        const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){ST_DONE, COG_OK};
}

/* Allowed-transition lists, END-terminated as the header requires. */
static const cog_state_t idle_transitions[] = {
    ST_RUNNING,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t running_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t stay_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, on_user_input, idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_COMPLETE, on_llm_complete,
                       running_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_FAIL, on_fail, stay_transitions),
    COG_DISPATCH_ENTRY(ST_IDLE, EV_BADTRANS, on_badtrans, stay_transitions),
};

/* ------------------------------------------------------------------ */
/* Wait-hook context and helpers.                                      */
/* ------------------------------------------------------------------ */

typedef struct wait_ctx {
    cog_core_t *core;
    int calls;
    cog_result_t reentrant_result;
} wait_ctx_t;

/** First call enqueues a USER_INPUT; later calls stop the loop. */
static void drive_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;
    cog_event_t ev;

    ctx->calls++;
    if (ctx->calls == 1) {
        ev.id = (cog_event_id_t)EV_USER_INPUT;
        ev.kind = COG_EVENT_USER_INPUT;
        ev.payload = "hello";
        ev.payload_len = 5U;
        ev.source_id = 0U;
        (void)cog_core_enqueue(ctx->core, &ev);
    } else {
        cog_core_stop(ctx->core);
    }
}

/** Stop the loop on the first wait. */
static void stop_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;
    ctx->calls++;
    cog_core_stop(ctx->core);
}

/** Re-entrantly call cog_core_run (must fail: already running). */
static void reentrant_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;
    ctx->calls++;
    ctx->reentrant_result = cog_core_run(ctx->core);
    cog_core_stop(ctx->core);
}

/* ------------------------------------------------------------------ */
/* Small assertion helpers.                                            */
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
/* Construction / destruction                                          */
/* ------------------------------------------------------------------ */

static void test_new_validation(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    /* NULL out / NULL config. */
    CHECK(cog_core_new(NULL, &cfg) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_new(&core, NULL) == COG_ERROR_NULL_PTR);
    CHECK(core == NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);

    /* NULL session. */
    cfg.session = NULL;
    CHECK(cog_core_new(&core, &cfg) == COG_ERROR_INVALID_ARG);
    CHECK(core == NULL);
    cfg.session = session;

    /* NULL dispatch table. */
    cfg.dispatch_table = NULL;
    CHECK(cog_core_new(&core, &cfg) == COG_ERROR_INVALID_ARG);
    CHECK(core == NULL);
    cfg.dispatch_table = dispatch_table;

    /* Zero table size. */
    cfg.dispatch_table_size = 0U;
    CHECK(cog_core_new(&core, &cfg) == COG_ERROR_INVALID_ARG);
    CHECK(core == NULL);
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);

    /* NOMEM via an oversized event-queue capacity. */
    cfg.event_queue_capacity = SIZE_MAX;
    CHECK(cog_core_new(&core, &cfg) == COG_ERROR_NOMEM);
    CHECK(core == NULL);

    /* Valid construction (default queue capacity). */
    cfg.event_queue_capacity = 0U;
    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    CHECK(core != NULL);
    cog_core_free(core);

    cog_session_free(session);
}

static void test_free_null(void)
{
    cog_core_free(NULL); /* must be a no-op */
    CHECK(1);
}

/* ------------------------------------------------------------------ */
/* Inspectors and accessors                                            */
/* ------------------------------------------------------------------ */

static void test_accessors(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_obs_config_t obs_cfg;
    cog_obs_t *obs = NULL;
    static int dummy_provider;
    static int dummy_store;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    /* The core now dereferences obs (lifecycle logging), so use a real
     * context rather than a fake pointer.  Zero-init config selects
     * defaults (no sinks, no fields). */
    memset(&obs_cfg, 0, sizeof(obs_cfg));
    CHECK(cog_obs_new(&obs, &obs_cfg) == COG_OK);

    /* NULL core returns NULL / zero / false. */
    CHECK(cog_core_session(NULL) == NULL);
    CHECK(cog_core_state(NULL) == 0U);
    CHECK(!cog_core_is_running(NULL));
    CHECK(cog_core_ipc(NULL) == NULL);
    CHECK(cog_core_provider(NULL) == NULL);
    CHECK(cog_core_obs(NULL) == NULL);
    CHECK(cog_core_store(NULL) == NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.provider = (cog_provider_t *)&dummy_provider;
    cfg.obs = obs;
    cfg.store = (cog_store_t *)&dummy_store;
    cfg.local_peer_id = 7U;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    CHECK(cog_core_session(core) == session);
    CHECK(cog_core_state(core) == 0U);
    CHECK(!cog_core_is_running(core));
    /* No IPC endpoint is configured, so the accessor returns NULL. */
    CHECK(cog_core_ipc(core) == NULL);
    CHECK(cog_core_provider(core) == (cog_provider_t *)&dummy_provider);
    CHECK(cog_core_obs(core) == obs);
    CHECK(cog_core_store(core) == (cog_store_t *)&dummy_store);

    /* Provider/obs/store may be NULL (not configured). */
    cfg.provider = NULL;
    cfg.obs = NULL;
    cfg.store = NULL;
    {
        cog_core_t *core2 = NULL;
        CHECK(cog_core_new(&core2, &cfg) == COG_OK);
        CHECK(cog_core_provider(core2) == NULL);
        CHECK(cog_core_obs(core2) == NULL);
        CHECK(cog_core_store(core2) == NULL);
        cog_core_free(core2);
    }

    cog_core_free(core);
    cog_obs_free(obs);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* IPC endpoint acceptance                                             */
/* ------------------------------------------------------------------ */

static void test_ipc_acceptance(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_ipc_endpoint_t *ep = NULL;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    CHECK(cog_ipc_endpoint_new(&ep) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.ipc_endpoint = ep;

    /* A non-NULL IPC endpoint is accepted and owned by the core. */
    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    CHECK(cog_core_ipc(core) == ep);

    /* cog_core_free destroys the endpoint (an unbound endpoint is never
     * drained). */
    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Enqueue                                                             */
/* ------------------------------------------------------------------ */

static void test_enqueue(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_event_t ev;
    cog_result_t rc;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.event_queue_capacity = 2U;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;

    CHECK(cog_core_enqueue(NULL, &ev) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_enqueue(core, NULL) == COG_ERROR_NULL_PTR);

    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    rc = cog_core_enqueue(core, &ev);
    CHECK(rc == COG_ERROR_OVERFLOW);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Run loop                                                            */
/* ------------------------------------------------------------------ */

static void test_run_basic(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = drive_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    CHECK(cog_core_run(core) == COG_OK);
    CHECK_EQ_INT(ctx.calls, 2);
    CHECK(cog_core_state(core) == ST_IDLE);
    CHECK(!cog_core_is_running(core));

    /* The turn appended a user message and an assistant message. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT(count, 2);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[0].content_len == 5U);
    CHECK(memcmp(msgs[0].content, "hello", 5U) == 0);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_run_null_and_reentrant(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;

    CHECK(cog_core_run(NULL) == COG_ERROR_NULL_PTR);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = reentrant_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    CHECK(cog_core_run(core) == COG_OK);
    CHECK(ctx.reentrant_result == COG_ERROR_INVALID_STATE);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_run_empty_queue_stop(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    /* Empty queue: the wait hook stops the loop immediately. */
    CHECK(cog_core_run(core) == COG_OK);
    CHECK_EQ_INT(ctx.calls, 1);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_run_dispatch_errors(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_event_t ev;
    wait_ctx_t ctx;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    /* No dispatch row for (ST_IDLE, EV_UNKNOWN). */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_UNKNOWN;
    ev.kind = COG_EVENT_SYSTEM_SIGNAL;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_ERROR_NOT_FOUND);

    /* Handler returns a non-COG_OK status. */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_FAIL;
    ev.kind = COG_EVENT_SYSTEM_SIGNAL;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_ERROR_IO);

    /* Handler requests a transition that is not allowed. */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_BADTRANS;
    ev.kind = COG_EVENT_SYSTEM_SIGNAL;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_ERROR_TRANSITION_DENIED);

    /* State is unchanged after the denied transition. */
    CHECK(cog_core_state(core) == ST_IDLE);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Pending message FIFO                                                */
/* ------------------------------------------------------------------ */

static void test_pending_api(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    const char *sid;
    char buf[64];
    size_t len = 0U;
    size_t count = 0U;
    int i;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    sid = cog_session_id(session);
    CHECK(sid != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    /* NULL checks. */
    CHECK(cog_core_queue_message(NULL, sid, "x", 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_queue_message(core, NULL, "x", 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_queue_message(core, sid, NULL, 1U) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_dequeue_message(NULL, sid, buf, sizeof(buf), &len) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_core_dequeue_message(core, NULL, buf, sizeof(buf), &len) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_core_dequeue_message(core, sid, NULL, sizeof(buf), &len) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_core_pending_count(NULL, sid, &count) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_pending_count(core, NULL, &count) == COG_ERROR_NULL_PTR);
    CHECK(cog_core_pending_count(core, sid, NULL) == COG_ERROR_NULL_PTR);

    /* Empty queue: dequeue is NOT_FOUND, count is zero. */
    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT(count, 0);
    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) ==
          COG_ERROR_NOT_FOUND);

    /* FIFO order and round-trip. */
    CHECK(cog_core_queue_message(core, sid, "one", 3U) == COG_OK);
    CHECK(cog_core_queue_message(core, sid, "two", 3U) == COG_OK);
    CHECK(cog_core_queue_message(core, sid, "three", 5U) == COG_OK);
    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT(count, 3);

    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) == COG_OK);
    CHECK_EQ_INT(len, 3);
    CHECK(memcmp(buf, "one", 3U) == 0);
    CHECK(buf[3] == '\0');

    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) == COG_OK);
    CHECK_EQ_INT(len, 3);
    CHECK(memcmp(buf, "two", 3U) == 0);

    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) == COG_OK);
    CHECK_EQ_INT(len, 5);
    CHECK(memcmp(buf, "three", 5U) == 0);

    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT(count, 0);

    /* Buffer too small: OVERFLOW and required length reported. */
    CHECK(cog_core_queue_message(core, sid, "hello world", 11U) == COG_OK);
    len = 0U;
    CHECK(cog_core_dequeue_message(core, sid, buf, 5U, &len) ==
          COG_ERROR_OVERFLOW);
    CHECK_EQ_INT(len, 11);
    /* The message is still queued. */
    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT(count, 1);
    CHECK(cog_core_dequeue_message(core, sid, buf, sizeof(buf), &len) == COG_OK);
    CHECK_EQ_INT(len, 11);

    /* Bounded capacity: the 9th message overflows (capacity 8). */
    for (i = 0; i < 8; ++i) {
        CHECK(cog_core_queue_message(core, sid, "m", 1U) == COG_OK);
    }
    CHECK(cog_core_queue_message(core, sid, "m", 1U) == COG_ERROR_OVERFLOW);
    CHECK(cog_core_pending_count(core, sid, &count) == COG_OK);
    CHECK_EQ_INT(count, 8);

    /* Per-session isolation: a different session id is not found. */
    CHECK(cog_core_pending_count(core, "other-session", &count) == COG_OK);
    CHECK_EQ_INT(count, 0);
    CHECK(cog_core_dequeue_message(core, "other-session", buf, sizeof(buf),
                                   &len) == COG_ERROR_NOT_FOUND);

    cog_core_free(core);
    cog_session_free(session);
}

static void test_pending_injection(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    const char *sid;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    sid = cog_session_id(session);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    /* Queue two messages for the core's session before running. */
    CHECK(cog_core_queue_message(core, sid, "first", 5U) == COG_OK);
    CHECK(cog_core_queue_message(core, sid, "second", 6U) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    /* The loop injects each pending message as a USER_INPUT after the
     * previous turn completes; the wait hook stops once the FIFO drains. */
    CHECK(cog_core_run(core) == COG_OK);
    CHECK_EQ_INT(ctx.calls, 1);

    /* Two turns: user/assistant/user/assistant. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT(count, 4);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[0].content_len == 5U);
    CHECK(memcmp(msgs[0].content, "first", 5U) == 0);
    CHECK(msgs[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[2].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[2].content_len == 6U);
    CHECK(memcmp(msgs[2].content, "second", 6U) == 0);
    CHECK(msgs[3].role == COG_MESSAGE_ROLE_ASSISTANT);

    /* The FIFO is drained. */
    {
        size_t pc = 0U;
        CHECK(cog_core_pending_count(core, sid, &pc) == COG_OK);
        CHECK_EQ_INT(pc, 0);
    }

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Regression tests (Critic review 4d7badf)                            */
/* ------------------------------------------------------------------ */

/** text_len == SIZE_MAX must not overflow the text_len + 1 allocation. */
static void test_queue_message_size_max(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    const char *sid;
    size_t pc = 0U;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    sid = cog_session_id(session);
    CHECK(sid != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    /* A length of SIZE_MAX would wrap text_len + 1 to zero and then write
     * past the allocation; it must be rejected, not corrupt memory. */
    CHECK(cog_core_queue_message(core, sid, "x", SIZE_MAX) == COG_ERROR_NOMEM);

    /* The FIFO is unchanged. */
    CHECK(cog_core_pending_count(core, sid, &pc) == COG_OK);
    CHECK_EQ_INT(pc, 0);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Shutdown must not dequeue-and-drop a pending message.               */
/* ------------------------------------------------------------------ */

/* Handler that blocks long enough for a concurrent stop to land while a
 * pending message is still queued and the event queue is empty. */
static cog_handler_result_t on_block(cog_core_t *core, const cog_event_t *event)
{
    struct timespec ts;

    (void)core;
    (void)event;
    ts.tv_sec = 0;
    ts.tv_nsec = 50000000L; /* 50 ms */
    (void)nanosleep(&ts, NULL);
    return (cog_handler_result_t){ST_IDLE, COG_OK};
}

static const cog_state_t block_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t block_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_BLOCK, on_block, block_transitions),
};

static cog_core_t *g_stop_core;

static void *stop_after_delay(void *arg)
{
    struct timespec ts;

    (void)arg;
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000L; /* 10 ms: fires while on_block is sleeping */
    (void)nanosleep(&ts, NULL);
    cog_core_stop(g_stop_core);
    return NULL;
}

static void test_shutdown_preserves_pending(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    const char *sid;
    size_t pc = 0U;
    pthread_t th;
    cog_event_t ev;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    sid = cog_session_id(session);
    CHECK(sid != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = block_table;
    cfg.dispatch_table_size = sizeof(block_table) / sizeof(block_table[0]);

    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    g_stop_core = core;

    /* A pending message is queued but not yet injected. */
    CHECK(cog_core_queue_message(core, sid, "hello", 5U) == COG_OK);

    /* A blocking event occupies the loop while a concurrent stop lands. */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_BLOCK;
    ev.kind = COG_EVENT_SYSTEM_SIGNAL;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);

    CHECK(pthread_create(&th, NULL, stop_after_delay, NULL) == 0);
    CHECK(cog_core_run(core) == COG_OK);
    CHECK(pthread_join(th, NULL) == 0);

    /* The pending message must survive shutdown, not be dequeued and
     * dropped. */
    CHECK(cog_core_pending_count(core, sid, &pc) == COG_OK);
    CHECK_EQ_INT(pc, 1);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Deterministic shutdown-preserves-pending (ticket ed77259).          */
/* ------------------------------------------------------------------ */

/* Context shared between the run thread (parked in the wait hook) and the
 * test thread (which issues the concurrent stop).  The mutex/condvar make
 * the stop land deterministically while the queue is empty and a pending
 * message exists, with no timing sleeps. */
typedef struct stop_ctx {
    cog_core_t *core;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int parked;      /**< Wait hook has queued the pending msg and blocked. */
    int stop_called; /**< Test thread has called cog_core_stop. */
} stop_ctx_t;

/* Runs on the run thread.  The queue is empty and no pending message
 * exists yet, so the loop calls this hook.  We queue a pending message for
 * the core's own session (single-threaded use: we are on the run thread),
 * then block until the test thread has issued a concurrent stop. */
static void park_wait_fn(void *arg)
{
    stop_ctx_t *ctx = (stop_ctx_t *)arg;
    const char *sid = cog_session_id(cog_core_session(ctx->core));

    (void)cog_core_queue_message(ctx->core, sid, "hello", 5U);

    pthread_mutex_lock(&ctx->lock);
    ctx->parked = 1;
    pthread_cond_signal(&ctx->cond);
    while (!ctx->stop_called) {
        pthread_cond_wait(&ctx->cond, &ctx->lock);
    }
    pthread_mutex_unlock(&ctx->lock);
}

static void *run_loop_thread(void *arg)
{
    cog_core_t *core = (cog_core_t *)arg;
    return (void *)(intptr_t)cog_core_run(core);
}

static void test_shutdown_preserves_pending_deterministic(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    stop_ctx_t ctx;
    const char *sid;
    size_t pc = 0U;
    pthread_t th;
    void *ret = NULL;

    CHECK(cog_session_new(&session, NULL) == COG_OK);
    sid = cog_session_id(session);
    CHECK(sid != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = park_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;
    CHECK(pthread_mutex_init(&ctx.lock, NULL) == 0);
    CHECK(pthread_cond_init(&ctx.cond, NULL) == 0);

    /* Run the loop on a separate thread.  It drains the empty queue and
     * parks in the wait hook, which queues a pending message and blocks. */
    CHECK(pthread_create(&th, NULL, run_loop_thread, core) == 0);

    /* Wait until the wait hook has queued the pending message and is
     * blocked: the queue is empty and a pending message exists. */
    pthread_mutex_lock(&ctx.lock);
    while (!ctx.parked) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    /* Concurrent stop from this thread while the queue is empty and a
     * pending message exists.  The loop must exit without dequeuing and
     * dropping the pending message. */
    cog_core_stop(core);
    pthread_mutex_lock(&ctx.lock);
    ctx.stop_called = 1;
    pthread_cond_signal(&ctx.cond);
    pthread_mutex_unlock(&ctx.lock);

    CHECK(pthread_join(th, &ret) == 0);
    CHECK((cog_result_t)(intptr_t)ret == COG_OK);

    /* The pending message must survive shutdown, not be dequeued and
     * dropped. */
    CHECK(cog_core_pending_count(core, sid, &pc) == COG_OK);
    CHECK_EQ_INT(pc, 1);

    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_new_validation();
    test_free_null();
    test_accessors();
    test_enqueue();
    test_run_basic();
    test_run_null_and_reentrant();
    test_run_empty_queue_stop();
    test_run_dispatch_errors();
    test_pending_api();
    test_pending_injection();
    test_queue_message_size_max();
    test_shutdown_preserves_pending();
    test_shutdown_preserves_pending_deterministic();
    test_ipc_acceptance();

    if (failures != 0) {
        fprintf(stderr, "test_core: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_core: all checks passed\n");
    return 0;
}
