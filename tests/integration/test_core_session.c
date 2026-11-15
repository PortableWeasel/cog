/**
 * @file test_core_session.c
 * @brief Cross-module integration test for core.c + session.c.
 *
 * This test wires the compiled cog-core modules together along the real
 * path the agent loop takes:
 *
 *   core.c    — construction, the run loop, event dispatch, and the
 *               session-scoped pending-message FIFO;
 *   session.c — the hierarchical conversation tree (cog_session_append,
 *               cog_session_get_history, cog_session_reset);
 *   event.c   — the bounded FIFO event queue;
 *   state.c   — the static const dispatch table and its lookup helpers;
 *   handler.h — the header-only cog_handler_fn signature.
 *
 * It verifies the cross-module effects that a unit test of core.c alone
 * cannot:
 *
 *   - session attachment: while a core owns a session, cog_session_reset
 *     returns COG_ERROR_INVALID_STATE (the core sets the session's
 *     private `attached` flag); after cog_core_free the same reset
 *     succeeds, proving the flag is cleared on teardown;
 *   - the run loop mutates the session tree through the dispatch table
 *     (history reflects the dispatched events);
 *   - pending messages queued for the session are auto-injected after a
 *     turn completes and appear in session history in FIFO order.
 *
 * Ticket: 681a566 (Stage 12.2a — Implement core.c)
 */

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
    ST_RUNNING = 1,
};

enum {
    EV_USER_INPUT = COG_EVENT_USER_INPUT,
    EV_LLM_COMPLETE = COG_EVENT_LLM_COMPLETE,
};

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

/** ST_IDLE + USER_INPUT: append the payload as a user message, enqueue an
 * LLM_COMPLETE to close the turn, and go RUNNING. */
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

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, on_user_input, idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_COMPLETE, on_llm_complete,
                       running_transitions),
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
/* Session attachment / reset interaction.                             */
/* ------------------------------------------------------------------ */

static void test_session_attachment(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    /* A fresh session resets cleanly. */
    CHECK(cog_session_reset(session) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    /* While the core owns the session, reset is refused. */
    CHECK(cog_session_reset(session) == COG_ERROR_INVALID_STATE);
    CHECK(cog_core_session(core) == session);

    /* After teardown the session is detached and resets again. */
    cog_core_free(core);
    CHECK(cog_session_reset(session) == COG_OK);

    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Run loop mutates session history.                                   */
/* ------------------------------------------------------------------ */

static void test_run_mutates_history(void)
{
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    wait_ctx_t ctx;
    cog_event_t ev;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

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

    /* Enqueue a user turn before running. */
    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = "capital of France";
    ev.payload_len = 18U;

    CHECK(cog_core_enqueue(core, &ev) == COG_OK);
    CHECK(cog_core_run(core) == COG_OK);

    /* The session tree now holds user + assistant. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT(count, 2);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[0].content_len == 18U);
    CHECK(memcmp(msgs[0].content, "capital of France", 18U) == 0);
    CHECK(msgs[1].role == COG_MESSAGE_ROLE_ASSISTANT);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Pending messages auto-inject into session history.                  */
/* ------------------------------------------------------------------ */

static void test_pending_injection_history(void)
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
    CHECK(sid != NULL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    CHECK(cog_core_queue_message(core, sid, "alpha", 5U) == COG_OK);
    CHECK(cog_core_queue_message(core, sid, "beta", 4U) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    CHECK(cog_core_run(core) == COG_OK);
    CHECK_EQ_INT(ctx.calls, 1);

    /* Two turns: alpha/assistant/beta/assistant. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT(count, 4);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[0].content_len == 5U);
    CHECK(memcmp(msgs[0].content, "alpha", 5U) == 0);
    CHECK(msgs[2].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[2].content_len == 4U);
    CHECK(memcmp(msgs[2].content, "beta", 4U) == 0);

    cog_core_free(core);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_session_attachment();
    test_run_mutates_history();
    test_pending_injection_history();

    if (failures != 0) {
        fprintf(stderr, "test_core_session: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_core_session: all checks passed\n");
    return 0;
}
