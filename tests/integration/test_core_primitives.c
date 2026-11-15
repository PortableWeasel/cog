/**
 * @file test_core_primitives.c
 * @brief Cross-module integration test for the cog-core Phase 1 primitives.
 *
 * This test wires the compiled cog-core modules together along a real
 * path that the future @c cog_core_run loop will take:
 *
 *   session.c  — a hierarchical conversation tree (cog_session_append,
 *                cog_session_get_history);
 *   event.c    — a bounded FIFO event queue (cog_event_queue_new/push/pop);
 *   state.c    — the static const dispatch table and its lookup helpers
 *                (cog_state_lookup / cog_state_transition_allowed);
 *   handler.h  — the header-only cog_handler_fn signature, invoked through
 *                the dispatch table.
 *
 * The test defines its own @c struct cog_core (the type is opaque and
 * forward-declared in handler.h) so handlers can carry the session and
 * queue they operate on.  It does NOT implement core.c or any later-phase
 * behaviour: there is no run loop, no event dispatch engine, and no
 * session attachment.  It only proves that the Phase 1 primitives compose
 * correctly and propagate errors across module boundaries.
 *
 * Ticket: 6044b51 (Stage 12.1e — shared build and cross-module integration)
 */

#include "cog/event.h"
#include "cog/handler.h"
#include "cog/message.h"
#include "cog/result.h"
#include "cog/session.h"
#include "cog/state.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test application vocabulary (per state.h/event.h conventions).      */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
    ST_RUNNING = 1,
    ST_DONE = 2,
};

enum {
    EV_START = 0,
    EV_USER = 1,
    EV_SHUTDOWN = 2,
};

/* ------------------------------------------------------------------ */
/* Opaque core type (forward-declared in handler.h).                   */
/* ------------------------------------------------------------------ */

struct cog_core {
    cog_session_t *session;   /**< Session the handlers mutate/read. */
    cog_event_queue_t *queue; /**< Queue the handlers may feed. */
    cog_state_t state;        /**< Current state, tracked by the test. */
};

/* ------------------------------------------------------------------ */
/* Handlers (cog_handler_fn) wired into the static dispatch table.     */
/* ------------------------------------------------------------------ */

/** ST_IDLE + EV_START: record the user's opening message, go RUNNING. */
static cog_handler_result_t on_start(cog_core_t *core,
                                     const cog_event_t *event)
{
    static const cog_message_t msg = {
        COG_MESSAGE_ROLE_USER, "hello", 5U, NULL
    };
    cog_handler_result_t r = { ST_RUNNING, COG_OK };

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    if (event->kind != COG_EVENT_USER_INPUT) {
        r.status = COG_ERROR_INVALID_ARG;
        return r;
    }
    if (cog_session_append(core->session, &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** ST_RUNNING + EV_USER: record an assistant reply, stay RUNNING. */
static cog_handler_result_t on_user(cog_core_t *core,
                                    const cog_event_t *event)
{
    static const cog_message_t msg = {
        COG_MESSAGE_ROLE_ASSISTANT, "hi", 2U, NULL
    };
    cog_handler_result_t r = { ST_RUNNING, COG_OK };

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    if (event->kind != COG_EVENT_USER_INPUT) {
        r.status = COG_ERROR_INVALID_ARG;
        return r;
    }
    if (cog_session_append(core->session, &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** ST_RUNNING + EV_SHUTDOWN: go DONE. */
static cog_handler_result_t on_shutdown(cog_core_t *core,
                                        const cog_event_t *event)
{
    cog_handler_result_t r = { ST_DONE, COG_OK };

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    if (event->kind != COG_EVENT_SHUTDOWN) {
        r.status = COG_ERROR_INVALID_ARG;
        return r;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Static const dispatch table (state.c contract).                     */
/* ------------------------------------------------------------------ */

static const cog_state_t idle_transitions[] = {
    ST_RUNNING,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t running_transitions[] = {
    ST_RUNNING,
    ST_DONE,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_START, on_start, idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_USER, on_user, running_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_SHUTDOWN, on_shutdown, running_transitions),
};

#define DISPATCH_TABLE_SIZE \
    (sizeof(dispatch_table) / sizeof(dispatch_table[0]))

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int expect_history(cog_session_t *s, size_t want_count,
                          const char *want_last)
{
    const cog_session_node_t *active = cog_session_active(s);
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    if (active == NULL) {
        (void)fprintf(stderr, "  no active node\n");
        return 1;
    }
    if (cog_session_get_history(s, active, &msgs, &count) != COG_OK) {
        (void)fprintf(stderr, "  get_history failed\n");
        return 1;
    }
    if (count != want_count) {
        (void)fprintf(stderr, "  history count %zu != %zu\n",
                      count, want_count);
        return 1;
    }
    if (want_last != NULL &&
        (msgs[count - 1U].content == NULL ||
         strcmp(msgs[count - 1U].content, want_last) != 0)) {
        (void)fprintf(stderr, "  last message content mismatch\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test cases                                                          */
/* ------------------------------------------------------------------ */

/** Build the session + queue + core fixture. */
static int fixture_new(cog_core_t *core, cog_session_t **s,
                       cog_event_queue_t **q)
{
    cog_session_config_t cfg = {NULL, 0U}; /* default node cap */
    cog_session_t *session = NULL;
    cog_event_queue_t *queue = NULL;

    if (cog_session_new(&session, &cfg) != COG_OK) {
        return 1;
    }
    if (cog_event_queue_new(&queue, 4U) != COG_OK) {
        cog_session_free(session);
        return 1;
    }
    core->session = session;
    core->queue = queue;
    core->state = ST_IDLE;
    *s = session;
    *q = queue;
    return 0;
}

static void fixture_free(cog_session_t *s, cog_event_queue_t *q)
{
    cog_event_queue_free(q);
    cog_session_free(s);
}

/**
 * Happy path: dispatch (ST_IDLE, EV_START) through the table, invoke the
 * handler, and confirm the session was mutated and the transition is
 * allowed.
 */
static int test_dispatch_start(void)
{
    cog_core_t core;
    cog_session_t *s;
    cog_event_queue_t *q;
    const cog_dispatch_entry_t *entry = NULL;
    cog_event_t ev;
    cog_handler_result_t hr;

    if (fixture_new(&core, &s, &q) != 0) {
        return 1;
    }

    ev.id = EV_START;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 0U;

    if (cog_state_lookup(dispatch_table, DISPATCH_TABLE_SIZE,
                         ST_IDLE, EV_START, &entry) != COG_OK ||
        entry == NULL) {
        (void)fprintf(stderr, "  lookup (IDLE, START) failed\n");
        fixture_free(s, q);
        return 1;
    }
    if (entry->handler == NULL) {
        (void)fprintf(stderr, "  entry handler is NULL\n");
        fixture_free(s, q);
        return 1;
    }

    hr = entry->handler(&core, &ev);
    if (hr.status != COG_OK || hr.transition != ST_RUNNING) {
        (void)fprintf(stderr, "  handler result status=%d transition=%u\n",
                      (int)hr.status, (unsigned)hr.transition);
        fixture_free(s, q);
        return 1;
    }
    if (cog_state_transition_allowed(idle_transitions, hr.transition) !=
        COG_OK) {
        (void)fprintf(stderr, "  transition to RUNNING not allowed\n");
        fixture_free(s, q);
        return 1;
    }
    if (expect_history(s, 1U, "hello") != 0) {
        fixture_free(s, q);
        return 1;
    }

    fixture_free(s, q);
    return 0;
}

/**
 * Cross-module: a second dispatch appends another message, and the
 * session history reflects both handler mutations in order.
 */
static int test_dispatch_sequence(void)
{
    cog_core_t core;
    cog_session_t *s;
    cog_event_queue_t *q;
    const cog_dispatch_entry_t *entry = NULL;
    cog_event_t ev;
    cog_handler_result_t hr;

    if (fixture_new(&core, &s, &q) != 0) {
        return 1;
    }

    ev.id = EV_START;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 0U;

    if (cog_state_lookup(dispatch_table, DISPATCH_TABLE_SIZE,
                         ST_IDLE, EV_START, &entry) != COG_OK) {
        fixture_free(s, q);
        return 1;
    }
    hr = entry->handler(&core, &ev);
    if (hr.status != COG_OK) {
        fixture_free(s, q);
        return 1;
    }

    ev.id = EV_USER;
    ev.kind = COG_EVENT_USER_INPUT;
    if (cog_state_lookup(dispatch_table, DISPATCH_TABLE_SIZE,
                         ST_RUNNING, EV_USER, &entry) != COG_OK) {
        fixture_free(s, q);
        return 1;
    }
    hr = entry->handler(&core, &ev);
    if (hr.status != COG_OK) {
        fixture_free(s, q);
        return 1;
    }
    if (expect_history(s, 2U, "hi") != 0) {
        fixture_free(s, q);
        return 1;
    }

    fixture_free(s, q);
    return 0;
}

/**
 * Event queue interaction: push/pop round-trips an event through the
 * bounded queue, and the queue reports empty/full/count correctly.
 */
static int test_queue_roundtrip(void)
{
    cog_core_t core;
    cog_session_t *s;
    cog_event_queue_t *q;
    cog_event_t ev;
    cog_event_t out;

    if (fixture_new(&core, &s, &q) != 0) {
        return 1;
    }

    ev.id = EV_START;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 42U;

    if (!cog_event_queue_is_empty(q)) {
        (void)fprintf(stderr, "  queue not empty after new\n");
        fixture_free(s, q);
        return 1;
    }
    if (cog_event_queue_push(q, &ev) != COG_OK) {
        (void)fprintf(stderr, "  push failed\n");
        fixture_free(s, q);
        return 1;
    }
    if (cog_event_queue_count(q) != 1U) {
        (void)fprintf(stderr, "  count != 1 after push\n");
        fixture_free(s, q);
        return 1;
    }
    if (cog_event_queue_pop(q, &out) != COG_OK) {
        (void)fprintf(stderr, "  pop failed\n");
        fixture_free(s, q);
        return 1;
    }
    if (out.id != EV_START || out.kind != COG_EVENT_USER_INPUT ||
        out.source_id != 42U) {
        (void)fprintf(stderr, "  popped event fields mismatch\n");
        fixture_free(s, q);
        return 1;
    }
    if (!cog_event_queue_is_empty(q)) {
        (void)fprintf(stderr, "  queue not empty after pop\n");
        fixture_free(s, q);
        return 1;
    }

    fixture_free(s, q);
    return 0;
}

/**
 * Error propagation across modules:
 *   - dispatch lookup for an unhandled (state, event) pair -> NOT_FOUND;
 *   - a transition not in the allowed list -> TRANSITION_DENIED;
 *   - pushing past the bounded capacity -> OVERFLOW;
 *   - NULL arguments -> NULL_PTR.
 */
static int test_error_propagation(void)
{
    cog_core_t core;
    cog_session_t *s;
    cog_event_queue_t *q;
    const cog_dispatch_entry_t *entry = NULL;
    cog_event_t ev;
    size_t i;

    if (fixture_new(&core, &s, &q) != 0) {
        return 1;
    }

    /* Unhandled (state, event) pair. */
    if (cog_state_lookup(dispatch_table, DISPATCH_TABLE_SIZE,
                         ST_DONE, EV_START, &entry) != COG_ERROR_NOT_FOUND) {
        (void)fprintf(stderr, "  unhandled pair did not return NOT_FOUND\n");
        fixture_free(s, q);
        return 1;
    }

    /* Transition not in the allowed list (ST_DONE is not in idle list). */
    if (cog_state_transition_allowed(idle_transitions, ST_DONE) !=
        COG_ERROR_TRANSITION_DENIED) {
        (void)fprintf(stderr, "  denied transition did not return DENIED\n");
        fixture_free(s, q);
        return 1;
    }

    /* NULL allowed list. */
    if (cog_state_transition_allowed(NULL, ST_RUNNING) !=
        COG_ERROR_NULL_PTR) {
        (void)fprintf(stderr, "  NULL allowed list did not return NULL_PTR\n");
        fixture_free(s, q);
        return 1;
    }

    /* Fill the bounded queue (capacity 4) then overflow. */
    ev.id = EV_USER;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = NULL;
    ev.payload_len = 0U;
    ev.source_id = 0U;
    for (i = 0U; i < 4U; ++i) {
        if (cog_event_queue_push(q, &ev) != COG_OK) {
            (void)fprintf(stderr, "  push %zu failed\n", i);
            fixture_free(s, q);
            return 1;
        }
    }
    if (!cog_event_queue_is_full(q)) {
        (void)fprintf(stderr, "  queue not full at capacity\n");
        fixture_free(s, q);
        return 1;
    }
    if (cog_event_queue_push(q, &ev) != COG_ERROR_OVERFLOW) {
        (void)fprintf(stderr, "  overflow did not return OVERFLOW\n");
        fixture_free(s, q);
        return 1;
    }

    /* NULL argument propagation. */
    if (cog_event_queue_push(NULL, &ev) != COG_ERROR_NULL_PTR) {
        (void)fprintf(stderr, "  push NULL queue did not return NULL_PTR\n");
        fixture_free(s, q);
        return 1;
    }
    if (cog_session_append(NULL, NULL, NULL) != COG_ERROR_NULL_PTR) {
        (void)fprintf(stderr, "  append NULL did not return NULL_PTR\n");
        fixture_free(s, q);
        return 1;
    }

    fixture_free(s, q);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("dispatch start", test_dispatch_start());
    CHECK("dispatch sequence", test_dispatch_sequence());
    CHECK("queue roundtrip", test_queue_roundtrip());
    CHECK("error propagation", test_error_propagation());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_core_primitives: ok\n");
    }
    return failures == 0U ? 0 : 1;
}
