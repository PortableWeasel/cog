/**
 * @file test_handler.c
 * @brief Contract test for the handler dispatch contract (handler.h +
 *        the dispatch primitives implemented in handler.c).
 *
 * handler.h declares the handler result type and the handler function
 * signature; it declares no functions.  The dispatch primitives that
 * route a (state, event) pair to a handler are declared in state.h and
 * implemented in handler.c (ticket 9298184).  This test pins:
 *
 *   - the shape of cog_handler_result_t (transition + status);
 *   - the cog_handler_fn signature (assignable and invocable);
 *   - successful dispatch: cog_state_lookup finds a matching row;
 *   - state results: a handler returns the transition/status it wants;
 *   - handler errors: a handler returns a non-COG_OK status;
 *   - NULL handlers/context/event as documented (a handler may detect a
 *     NULL core or NULL event and report COG_ERROR_NULL_PTR);
 *   - empty/default inputs and every boundary/error path of the two
 *     dispatch functions (NULL table, empty table, not found, denied
 *     transition, COG_TRANSITION_SELF, COG_TRANSITION_END sentinel).
 *
 * Nothing is tested here that is not either stated in the headers or
 * guaranteed by C11 semantics.
 *
 * Ticket: 9298184 (Stage 12.1d — Implement handler.c)
 */

#include "cog/handler.h"
#include "cog/state.h"

#include <stddef.h>
#include <stdio.h>

#ifndef COG_HANDLER_H
#error "COG_HANDLER_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Test states and events (application-defined ids, per state.h).      */
/* ------------------------------------------------------------------ */

#define TEST_STATE_IDLE    0u
#define TEST_STATE_RUNNING 1u
#define TEST_STATE_ERROR   2u

#define TEST_EVENT_START  100u
#define TEST_EVENT_INPUT  101u
#define TEST_EVENT_FAIL   102u

/* ------------------------------------------------------------------ */
/* Test handlers.                                                      */
/* ------------------------------------------------------------------ */

/** Happy path: request a forward transition, succeed. */
static cog_handler_result_t idle_start_handler(cog_core_t *core,
                                               const cog_event_t *event)
{
    (void)core;
    (void)event;
    cog_handler_result_t r = { TEST_STATE_RUNNING, COG_OK };
    return r;
}

/** Self-transition request, succeed. */
static cog_handler_result_t running_input_handler(cog_core_t *core,
                                                  const cog_event_t *event)
{
    (void)core;
    (void)event;
    cog_handler_result_t r = { COG_TRANSITION_SELF, COG_OK };
    return r;
}

/** Handler failure: non-COG_OK status, transition must not be applied. */
static cog_handler_result_t fail_handler(cog_core_t *core,
                                         const cog_event_t *event)
{
    (void)core;
    (void)event;
    cog_handler_result_t r = { TEST_STATE_ERROR, COG_ERROR_IO };
    return r;
}

/**
 * NULL context/event detection: per handler.h, core must be fully
 * initialised and event must be non-NULL.  A handler that receives a
 * NULL core or NULL event reports COG_ERROR_NULL_PTR.
 */
static cog_handler_result_t null_check_handler(cog_core_t *core,
                                               const cog_event_t *event)
{
    cog_handler_result_t r = { COG_TRANSITION_SELF, COG_OK };
    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Static const dispatch table (the documented usage pattern).         */
/* ------------------------------------------------------------------ */

static const cog_state_t idle_transitions[] = {
    TEST_STATE_RUNNING,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t running_transitions[] = {
    TEST_STATE_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t error_transitions[] = {
    TEST_STATE_IDLE,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t table[] = {
    COG_DISPATCH_ENTRY(TEST_STATE_IDLE, TEST_EVENT_START,
                       idle_start_handler, idle_transitions),
    COG_DISPATCH_ENTRY(TEST_STATE_RUNNING, TEST_EVENT_INPUT,
                       running_input_handler, running_transitions),
    COG_DISPATCH_ENTRY(TEST_STATE_ERROR, TEST_EVENT_FAIL,
                       fail_handler, error_transitions),
};

static const size_t table_size = sizeof(table) / sizeof(table[0]);

/* ------------------------------------------------------------------ */
/* Compile-time checks.                                                */
/* ------------------------------------------------------------------ */

/* cog_handler_result_t must expose transition and status fields. */
_Static_assert(_Generic(((cog_handler_result_t){0}).transition,
                        cog_state_t : 1, default : 0),
               "cog_handler_result_t.transition must be cog_state_t");
_Static_assert(_Generic(((cog_handler_result_t){0}).status,
                        cog_result_t : 1, default : 0),
               "cog_handler_result_t.status must be cog_result_t");

/* Reserved transition ids must have the documented values. */
_Static_assert(COG_TRANSITION_SELF == 0xFFFEu,
               "COG_TRANSITION_SELF must be 0xFFFE");
_Static_assert(COG_TRANSITION_END == 0xFFFFu,
               "COG_TRANSITION_END must be 0xFFFF");

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** Successful dispatch: a matching row is found and returned. */
static int verify_lookup_found(void)
{
    const cog_dispatch_entry_t *entry = NULL;

    if (cog_state_lookup(table, table_size, TEST_STATE_IDLE,
                         TEST_EVENT_START, &entry) != COG_OK) {
        return 1;
    }
    if (entry == NULL) {
        return 1;
    }
    if (entry->state != TEST_STATE_IDLE || entry->event != TEST_EVENT_START) {
        return 1;
    }
    if (entry->handler != idle_start_handler) {
        return 1;
    }
    if (entry->allowed_transitions != idle_transitions) {
        return 1;
    }
    return 0;
}

/** Lookup with out_entry == NULL still reports presence. */
static int verify_lookup_out_entry_null(void)
{
    if (cog_state_lookup(table, table_size, TEST_STATE_RUNNING,
                         TEST_EVENT_INPUT, NULL) != COG_OK) {
        return 1;
    }
    return 0;
}

/** No matching (state, event) row -> COG_ERROR_NOT_FOUND. */
static int verify_lookup_not_found(void)
{
    const cog_dispatch_entry_t *entry = (const cog_dispatch_entry_t *)&table[0];

    if (cog_state_lookup(table, table_size, TEST_STATE_IDLE,
                         TEST_EVENT_INPUT, &entry) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    /* out_entry must be unchanged on failure. */
    if (entry != &table[0]) {
        return 1;
    }
    return 0;
}

/** NULL table with non-zero size -> COG_ERROR_NULL_PTR. */
static int verify_lookup_null_table_nonzero(void)
{
    if (cog_state_lookup(NULL, 1U, TEST_STATE_IDLE,
                         TEST_EVENT_START, NULL) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

/** NULL table with zero size (empty table) -> COG_ERROR_NOT_FOUND. */
static int verify_lookup_null_table_zero(void)
{
    if (cog_state_lookup(NULL, 0U, TEST_STATE_IDLE,
                         TEST_EVENT_START, NULL) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    return 0;
}

/** Non-NULL empty table (size zero) -> COG_ERROR_NOT_FOUND. */
static int verify_lookup_empty_table(void)
{
    static const cog_dispatch_entry_t empty[1] = { {0} };

    if (cog_state_lookup(empty, 0U, TEST_STATE_IDLE,
                         TEST_EVENT_START, NULL) != COG_ERROR_NOT_FOUND) {
        return 1;
    }
    return 0;
}

/** Allowed transition -> COG_OK. */
static int verify_transition_allowed(void)
{
    if (cog_state_transition_allowed(idle_transitions,
                                     TEST_STATE_RUNNING) != COG_OK) {
        return 1;
    }
    return 0;
}

/** COG_TRANSITION_SELF is a valid allowed transition. */
static int verify_transition_self(void)
{
    if (cog_state_transition_allowed(running_transitions,
                                     COG_TRANSITION_SELF) != COG_OK) {
        return 1;
    }
    return 0;
}

/** Disallowed transition -> COG_ERROR_TRANSITION_DENIED. */
static int verify_transition_denied(void)
{
    if (cog_state_transition_allowed(idle_transitions,
                                     TEST_STATE_ERROR) != COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    return 0;
}

/** NULL allowed list -> COG_ERROR_NULL_PTR. */
static int verify_transition_null(void)
{
    if (cog_state_transition_allowed(NULL, TEST_STATE_IDLE) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

/** COG_TRANSITION_END is a sentinel, never a valid transition. */
static int verify_transition_end_sentinel(void)
{
    if (cog_state_transition_allowed(idle_transitions,
                                     COG_TRANSITION_END) != COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    return 0;
}

/** A handler invoked with valid args returns its requested result. */
static int verify_handler_success(void)
{
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_USER_INPUT };
    cog_handler_result_t r = idle_start_handler(NULL, &ev);

    if (r.status != COG_OK) {
        return 1;
    }
    if (r.transition != TEST_STATE_RUNNING) {
        return 1;
    }
    return 0;
}

/** A handler may request a self-transition. */
static int verify_handler_self_transition(void)
{
    const cog_event_t ev = { .id = TEST_EVENT_INPUT, .kind = COG_EVENT_USER_INPUT };
    cog_handler_result_t r = running_input_handler(NULL, &ev);

    if (r.status != COG_OK) {
        return 1;
    }
    if (r.transition != COG_TRANSITION_SELF) {
        return 1;
    }
    return 0;
}

/** Handler error propagation: non-COG_OK status is preserved. */
static int verify_handler_error(void)
{
    const cog_event_t ev = { .id = TEST_EVENT_FAIL, .kind = COG_EVENT_ERROR };
    cog_handler_result_t r = fail_handler(NULL, &ev);

    if (r.status != COG_ERROR_IO) {
        return 1;
    }
    if (r.transition != TEST_STATE_ERROR) {
        return 1;
    }
    return 0;
}

/** NULL context/event: a handler may report COG_ERROR_NULL_PTR. */
static int verify_handler_null_args(void)
{
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_USER_INPUT };

    if (null_check_handler(NULL, &ev).status != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (null_check_handler(NULL, NULL).status != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (null_check_handler((cog_core_t *)0x1, NULL).status != COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

/** cog_handler_fn is assignable and invocable through the pointer. */
static int verify_handler_fn_pointer(void)
{
    cog_handler_fn fn = idle_start_handler;
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_USER_INPUT };
    cog_handler_result_t r = fn(NULL, &ev);

    if (r.status != COG_OK || r.transition != TEST_STATE_RUNNING) {
        return 1;
    }
    return 0;
}

/** A zero-initialized handler result is a valid default: status is
 *  COG_OK (success) and transition is state 0 (not COG_TRANSITION_SELF,
 *  which is 0xFFFE). */
static int verify_default_result(void)
{
    const cog_handler_result_t r = {0};

    if (r.transition != 0u) {
        return 1;
    }
    if (r.status != COG_OK) {
        return 1;
    }
    return 0;
}

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

    CHECK("lookup found", verify_lookup_found());
    CHECK("lookup out_entry null", verify_lookup_out_entry_null());
    CHECK("lookup not found", verify_lookup_not_found());
    CHECK("lookup null table nonzero", verify_lookup_null_table_nonzero());
    CHECK("lookup null table zero", verify_lookup_null_table_zero());
    CHECK("lookup empty table", verify_lookup_empty_table());
    CHECK("transition allowed", verify_transition_allowed());
    CHECK("transition self", verify_transition_self());
    CHECK("transition denied", verify_transition_denied());
    CHECK("transition null", verify_transition_null());
    CHECK("transition end sentinel", verify_transition_end_sentinel());
    CHECK("handler success", verify_handler_success());
    CHECK("handler self transition", verify_handler_self_transition());
    CHECK("handler error", verify_handler_error());
    CHECK("handler null args", verify_handler_null_args());
    CHECK("handler fn pointer", verify_handler_fn_pointer());
    CHECK("default result", verify_default_result());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_handler: ok\n");
    }
    return failures == 0U ? 0 : 1;
}