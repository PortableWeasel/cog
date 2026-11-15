/**
 * @file test_handler.c
 * @brief Contract test for the handler.h contract (header-only module).
 *
 * handler.h declares the handler result type (@c cog_handler_result_t)
 * and the handler function signature (@c cog_handler_fn); it declares
 * no functions and owns no implementation unit.  This test pins:
 *
 *   - the shape of cog_handler_result_t (transition + status fields);
 *   - the cog_handler_fn signature (assignable and invocable through a
 *     test-defined handler);
 *   - context and event propagation: a handler receives the core and
 *     the event that triggered it and may read the event payload;
 *   - returned next_state and status passthrough: the handler's
 *     transition and status are returned verbatim;
 *   - NULL core/event handling as documented (a handler may detect a
 *     NULL core or NULL event and report COG_ERROR_NULL_PTR);
 *   - the zero-initialized default result.
 *
 * This module does not test state.h dispatch (cog_state_lookup /
 * cog_state_transition_allowed); state.c owns those.  Nothing is tested
 * here that is not either stated in handler.h or guaranteed by C11
 * semantics.
 *
 * Ticket: 9298184 (Stage 12.1d — Implement handler.c)
 */

#include "cog/handler.h"
#include "cog/state.h"   /* COG_TRANSITION_SELF (referenced by handler.h) */

#include <stddef.h>
#include <stdio.h>

#ifndef COG_HANDLER_H
#error "COG_HANDLER_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Test state/event ids (application-defined, per state.h/event.h).    */
/* ------------------------------------------------------------------ */

#define TEST_STATE_IDLE    0u
#define TEST_STATE_RUNNING 1u

#define TEST_EVENT_START  100u

/* ------------------------------------------------------------------ */
/* Test handlers.                                                      */
/* ------------------------------------------------------------------ */

/**
 * Context/event propagation: receives the core and event, verifies the
 * event context, and returns a requested next_state with COG_OK.
 */
static cog_handler_result_t context_handler(cog_core_t *core,
                                            const cog_event_t *event)
{
    cog_handler_result_t r = { TEST_STATE_RUNNING, COG_OK };

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    if (event->id != TEST_EVENT_START || event->kind != COG_EVENT_USER_INPUT) {
        r.status = COG_ERROR_INVALID_ARG;
        return r;
    }
    return r;
}

/** Self-transition request, succeed. */
static cog_handler_result_t self_handler(cog_core_t *core,
                                         const cog_event_t *event)
{
    (void)core;
    (void)event;
    cog_handler_result_t r = { COG_TRANSITION_SELF, COG_OK };
    return r;
}

/** Status passthrough: returns a non-COG_OK status verbatim. */
static cog_handler_result_t error_handler(cog_core_t *core,
                                          const cog_event_t *event)
{
    (void)core;
    (void)event;
    cog_handler_result_t r = { TEST_STATE_IDLE, COG_ERROR_IO };
    return r;
}

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

/* COG_TRANSITION_SELF is referenced by handler.h's transition doc. */
_Static_assert(COG_TRANSITION_SELF == 0xFFFEu,
               "COG_TRANSITION_SELF must be 0xFFFE");

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** A zero-initialized result is a valid default: status COG_OK, state 0. */
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

/** cog_handler_fn is assignable and invocable through the pointer. */
static int verify_fn_pointer(void)
{
    static long dummy_core_storage;
    cog_handler_fn fn = context_handler;
    const cog_event_t ev = {
        .id = TEST_EVENT_START,
        .kind = COG_EVENT_USER_INPUT,
        .payload = NULL,
        .payload_len = 0U,
        .source_id = 7U,
    };
    cog_handler_result_t r = fn((cog_core_t *)&dummy_core_storage, &ev);

    if (r.status != COG_OK) {
        return 1;
    }
    if (r.transition != TEST_STATE_RUNNING) {
        return 1;
    }
    return 0;
}

/** Context/event propagation: handler reads the event it was given. */
static int verify_context_propagation(void)
{
    static long dummy_core_storage;
    const cog_event_t ev = {
        .id = TEST_EVENT_START,
        .kind = COG_EVENT_USER_INPUT,
        .payload = NULL,
        .payload_len = 0U,
        .source_id = 7U,
    };
    cog_handler_result_t r = context_handler((cog_core_t *)&dummy_core_storage, &ev);

    if (r.status != COG_OK) {
        return 1;
    }
    return 0;
}

/** Next-state passthrough: the handler's transition is returned verbatim. */
static int verify_next_state_passthrough(void)
{
    static long dummy_core_storage;
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_USER_INPUT };
    cog_handler_result_t r = context_handler((cog_core_t *)&dummy_core_storage, &ev);

    if (r.transition != TEST_STATE_RUNNING) {
        return 1;
    }
    return 0;
}

/** A handler may request a self-transition (COG_TRANSITION_SELF). */
static int verify_self_transition(void)
{
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_USER_INPUT };
    cog_handler_result_t r = self_handler(NULL, &ev);

    if (r.status != COG_OK) {
        return 1;
    }
    if (r.transition != COG_TRANSITION_SELF) {
        return 1;
    }
    return 0;
}

/** Status passthrough: a non-COG_OK status is returned verbatim. */
static int verify_status_passthrough(void)
{
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_ERROR };
    cog_handler_result_t r = error_handler(NULL, &ev);

    if (r.status != COG_ERROR_IO) {
        return 1;
    }
    if (r.transition != TEST_STATE_IDLE) {
        return 1;
    }
    return 0;
}

/** NULL core/event: a handler may report COG_ERROR_NULL_PTR. */
static int verify_null_args(void)
{
    static long dummy_core_storage;
    const cog_event_t ev = { .id = TEST_EVENT_START, .kind = COG_EVENT_USER_INPUT };

    if (context_handler(NULL, &ev).status != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (context_handler(NULL, NULL).status != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (context_handler((cog_core_t *)&dummy_core_storage, NULL).status !=
        COG_ERROR_NULL_PTR) {
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

    CHECK("default result", verify_default_result());
    CHECK("fn pointer", verify_fn_pointer());
    CHECK("context propagation", verify_context_propagation());
    CHECK("next state passthrough", verify_next_state_passthrough());
    CHECK("self transition", verify_self_transition());
    CHECK("status passthrough", verify_status_passthrough());
    CHECK("null args", verify_null_args());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_handler: ok\n");
    }
    return failures == 0U ? 0 : 1;
}