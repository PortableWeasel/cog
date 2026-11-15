/**
 * @file test_state.c
 * @brief Contract test for cog/state.h implemented by cog-core/src/state.c.
 *
 * state.h declares the table-driven dispatch contract: a static const
 * array of @c cog_dispatch_entry_t rows mapping (state, event) pairs to
 * handlers, plus the reserved transition ids COG_TRANSITION_SELF /
 * COG_TRANSITION_END and the two lookup helpers cog_state_lookup and
 * cog_state_transition_allowed.
 *
 * This test pins, at compile time, the documented constant values and the
 * struct field order, and verifies at run time:
 *
 *   - normal (state, event) lookups return the matching row
 *   - self-transition sentinel is a legal allowed-list member
 *   - end-of-list sentinel terminates the list and is never a match
 *   - duplicate (state, event) rows resolve deterministically to the
 *     first row in table order
 *   - missing entries and empty tables return COG_ERROR_NOT_FOUND and
 *     leave *out_entry unchanged
 *   - NULL table with non-zero size and NULL allowed list return
 *     COG_ERROR_NULL_PTR
 *   - boundary state/event ids (0, COG_STATE_MAX, 0xFFFF) and reserved
 *     ids (COG_TRANSITION_SELF, COG_TRANSITION_END) behave as documented
 *   - every declared error path of both functions is exercised
 *
 * Ticket: 6b1c97b (Stage 12.1c — Implement state.c)
 */

#include "cog/state.h"

#include <stddef.h>
#include <stdio.h>

#ifndef COG_STATE_H
#error "COG_STATE_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks: documented constants and struct layout.        */
/* ------------------------------------------------------------------ */

_Static_assert(COG_TRANSITION_SELF == 0xFFFEu,
               "COG_TRANSITION_SELF must be exactly 0xFFFE");
_Static_assert(COG_TRANSITION_END == 0xFFFFu,
               "COG_TRANSITION_END must be exactly 0xFFFF");
_Static_assert(COG_STATE_MAX == 0xFFFDu,
               "COG_STATE_MAX must be exactly 0xFFFD");
_Static_assert(COG_STATE_MAX < COG_TRANSITION_SELF,
               "application states must sit below the reserved ids");
_Static_assert(COG_TRANSITION_SELF < COG_TRANSITION_END,
               "SELF must sort below END");
_Static_assert(sizeof(cog_state_t) == sizeof(uint16_t),
               "cog_state_t must be a 16-bit unsigned id");
_Static_assert(sizeof(cog_event_id_t) == sizeof(uint16_t),
               "cog_event_id_t must be a 16-bit unsigned id");

/* Field order of the dispatch entry is part of the ABI contract. */
_Static_assert(offsetof(cog_dispatch_entry_t, state) <
                   offsetof(cog_dispatch_entry_t, event),
               "state field must precede event field");
_Static_assert(offsetof(cog_dispatch_entry_t, event) <
                   offsetof(cog_dispatch_entry_t, handler),
               "event field must precede handler field");
_Static_assert(offsetof(cog_dispatch_entry_t, handler) <
                   offsetof(cog_dispatch_entry_t, allowed_transitions),
               "handler field must precede allowed_transitions field");

/* ------------------------------------------------------------------ */
/* Test fixtures: a small application state/event vocabulary.          */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
    ST_RUNNING = 1,
    ST_AWAITING_LLM = 2,
    ST_DONE = 3,
};

enum {
    EV_START = 0,
    EV_TOOL_RESULT = 1,
    EV_LLM_COMPLETE = 2,
    EV_SHUTDOWN = 3,
};

/* Dummy handlers: identity is what matters, not behaviour. */
static cog_handler_result_t handler_a(cog_core_t *core, const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}

static cog_handler_result_t handler_b(cog_core_t *core, const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}

static cog_handler_result_t handler_c(cog_core_t *core, const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}

/* Allowed-transition lists, END-terminated as the header requires. */
static const cog_state_t idle_transitions[] = {
    ST_RUNNING,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t running_transitions[] = {
    ST_AWAITING_LLM,
    ST_DONE,
    COG_TRANSITION_END,
};

static const cog_state_t awaiting_transitions[] = {
    ST_RUNNING,
    COG_TRANSITION_END,
};

static const cog_state_t empty_transitions[] = {
    COG_TRANSITION_END,
};

/* The canonical dispatch table used by most lookup tests. */
static const cog_dispatch_entry_t table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_START, handler_a, idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_TOOL_RESULT, handler_b, running_transitions),
    COG_DISPATCH_ENTRY(ST_AWAITING_LLM, EV_LLM_COMPLETE, handler_c,
                       awaiting_transitions),
};

/* ------------------------------------------------------------------ */
/* Lookup: normal transitions.                                         */
/* ------------------------------------------------------------------ */

static int lookup_finds_every_row(void)
{
    const cog_dispatch_entry_t *entry = NULL;
    size_t i;

    for (i = 0U; i < sizeof(table) / sizeof(table[0]); ++i) {
        entry = NULL;
        if (cog_state_lookup(table, 3U, table[i].state, table[i].event,
                             &entry) != COG_OK) {
            (void)fprintf(stderr, "lookup failed for row %zu\n", i);
            return 1;
        }
        if (entry != &table[i]) {
            (void)fprintf(stderr, "lookup returned wrong row for row %zu\n", i);
            return 1;
        }
        /* The returned row must be the exact row: same handler, same list. */
        if (entry->handler != table[i].handler ||
            entry->allowed_transitions != table[i].allowed_transitions) {
            (void)fprintf(stderr, "lookup row %zu fields mismatch\n", i);
            return 1;
        }
    }
    return 0;
}

/* COG_DISPATCH_ENTRY must populate every field of the row. */
static int dispatch_entry_macro_populates_fields(void)
{
    const cog_dispatch_entry_t row =
        COG_DISPATCH_ENTRY(ST_DONE, EV_SHUTDOWN, handler_c, empty_transitions);

    if (row.state != ST_DONE || row.event != EV_SHUTDOWN ||
        row.handler != handler_c ||
        row.allowed_transitions != empty_transitions) {
        return 1;
    }
    return 0;
}

/* A row with a NULL handler still matches: lookup is structural. */
static int lookup_matches_null_handler_row(void)
{
    static const cog_dispatch_entry_t null_table[] = {
        COG_DISPATCH_ENTRY(ST_IDLE, EV_START, NULL, empty_transitions),
    };
    const cog_dispatch_entry_t *entry = NULL;

    if (cog_state_lookup(null_table, 1U, ST_IDLE, EV_START, &entry) != COG_OK) {
        return 1;
    }
    if (entry != &null_table[0] || entry->handler != NULL) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lookup: missing entries, empty tables, NULLs.                       */
/* ------------------------------------------------------------------ */

static int lookup_missing_entry_leaves_out_unchanged(void)
{
    const cog_dispatch_entry_t *entry = (const cog_dispatch_entry_t *)&table[2];

    /* (state, event) pair that exists in no row. */
    if (cog_state_lookup(table, 3U, ST_DONE, EV_START, &entry) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (entry != &table[2]) {
        (void)fprintf(stderr, "*out_entry modified on NOT_FOUND\n");
        return 1;
    }
    return 0;
}

static int lookup_empty_table_returns_not_found(void)
{
    const cog_dispatch_entry_t *entry = (const cog_dispatch_entry_t *)&table[0];
    const cog_dispatch_entry_t *dummy = table;

    /* NULL table with zero size: empty table. */
    if (cog_state_lookup(NULL, 0U, ST_IDLE, EV_START, &entry) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (entry != &table[0]) {
        (void)fprintf(stderr, "*out_entry modified on empty-table NOT_FOUND\n");
        return 1;
    }

    /* Non-NULL table pointer with zero size is also an empty table. */
    if (cog_state_lookup(dummy, 0U, ST_IDLE, EV_START, &entry) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    return 0;
}

static int lookup_null_table_nonzero_size_returns_null_ptr(void)
{
    const cog_dispatch_entry_t *entry = NULL;

    if (cog_state_lookup(NULL, 1U, ST_IDLE, EV_START, &entry) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (entry != NULL) {
        (void)fprintf(stderr, "*out_entry modified on NULL_PTR\n");
        return 1;
    }
    return 0;
}

/* out_entry may be NULL: presence-only lookup. */
static int lookup_presence_only_with_null_out(void)
{
    if (cog_state_lookup(table, 3U, ST_IDLE, EV_START, NULL) != COG_OK) {
        return 1;
    }
    if (cog_state_lookup(table, 3U, ST_DONE, EV_START, NULL) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (cog_state_lookup(NULL, 0U, ST_IDLE, EV_START, NULL) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (cog_state_lookup(NULL, 1U, ST_IDLE, EV_START, NULL) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lookup: boundary and reserved ids.                                  */
/* ------------------------------------------------------------------ */

static int lookup_boundary_ids(void)
{
    static const cog_state_t boundary_transitions[] = {COG_TRANSITION_END};
    static const cog_dispatch_entry_t boundary_table[] = {
        /* Lowest legal state and event ids. */
        COG_DISPATCH_ENTRY(0U, 0U, handler_a, boundary_transitions),
        /* Highest legal state id and highest event id. */
        COG_DISPATCH_ENTRY(COG_STATE_MAX, 0xFFFFu, handler_b,
                           boundary_transitions),
    };
    const cog_dispatch_entry_t *entry = NULL;

    if (cog_state_lookup(boundary_table, 2U, 0U, 0U, &entry) != COG_OK ||
        entry != &boundary_table[0]) {
        return 1;
    }
    if (cog_state_lookup(boundary_table, 2U, COG_STATE_MAX, 0xFFFFu, &entry) !=
            COG_OK ||
        entry != &boundary_table[1]) {
        return 1;
    }
    return 0;
}

/* Reserved ids are not application states: queries using them never
 * match a well-formed table. */
static int lookup_reserved_ids_never_match(void)
{
    const cog_dispatch_entry_t *entry = NULL;

    if (cog_state_lookup(table, 3U, COG_TRANSITION_SELF, EV_START, &entry) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (cog_state_lookup(table, 3U, COG_TRANSITION_END, EV_START, &entry) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    if (cog_state_lookup(table, 3U, ST_IDLE, 0xFFFFu, &entry) !=
        COG_ERROR_NOT_FOUND) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lookup: duplicate entries are deterministic (first row wins).       */
/* ------------------------------------------------------------------ */

static int lookup_duplicate_rows_first_match_wins(void)
{
    static const cog_dispatch_entry_t dup_table[] = {
        COG_DISPATCH_ENTRY(ST_IDLE, EV_START, handler_a, idle_transitions),
        COG_DISPATCH_ENTRY(ST_IDLE, EV_START, handler_b, running_transitions),
        COG_DISPATCH_ENTRY(ST_IDLE, EV_START, handler_c, awaiting_transitions),
    };
    const cog_dispatch_entry_t *entry = NULL;
    unsigned iter;

    /* Repeat to prove the result is stable, not incidental. */
    for (iter = 0U; iter < 100U; ++iter) {
        entry = NULL;
        if (cog_state_lookup(dup_table, 3U, ST_IDLE, EV_START, &entry) !=
            COG_OK) {
            return 1;
        }
        if (entry != &dup_table[0] || entry->handler != handler_a) {
            (void)fprintf(stderr, "duplicate lookup not first-match\n");
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Transition validation.                                              */
/* ------------------------------------------------------------------ */

static int transition_allowed_happy_paths(void)
{
    /* idle_transitions = {RUNNING, SELF, END} */
    if (cog_state_transition_allowed(idle_transitions, ST_RUNNING) != COG_OK) {
        return 1;
    }
    if (cog_state_transition_allowed(idle_transitions, COG_TRANSITION_SELF) !=
        COG_OK) {
        return 1;
    }
    /* running_transitions = {AWAITING_LLM, DONE, END} */
    if (cog_state_transition_allowed(running_transitions, ST_AWAITING_LLM) !=
        COG_OK) {
        return 1;
    }
    if (cog_state_transition_allowed(running_transitions, ST_DONE) != COG_OK) {
        return 1;
    }
    return 0;
}

static int transition_denied_cases(void)
{
    /* idle_transitions = {RUNNING, SELF, END}: DONE is not allowed. */
    if (cog_state_transition_allowed(idle_transitions, ST_DONE) !=
        COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    /* SELF not present in running_transitions. */
    if (cog_state_transition_allowed(running_transitions, COG_TRANSITION_SELF) !=
        COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    /* The END sentinel itself is never a legal transition. */
    if (cog_state_transition_allowed(idle_transitions, COG_TRANSITION_END) !=
        COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    /* An empty allowed list denies every transition. */
    if (cog_state_transition_allowed(empty_transitions, ST_IDLE) !=
        COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    if (cog_state_transition_allowed(empty_transitions, COG_TRANSITION_SELF) !=
        COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    return 0;
}

/* The sentinel terminates the list: entries after it are not members. */
static int transition_sentinel_terminates_list(void)
{
    static const cog_state_t truncated[] = {
        ST_RUNNING,
        COG_TRANSITION_END,
        ST_DONE, /* after the sentinel: not part of the list */
    };

    if (cog_state_transition_allowed(truncated, ST_RUNNING) != COG_OK) {
        return 1;
    }
    if (cog_state_transition_allowed(truncated, ST_DONE) !=
        COG_ERROR_TRANSITION_DENIED) {
        (void)fprintf(stderr, "entry after END sentinel must be ignored\n");
        return 1;
    }
    return 0;
}

static int transition_boundary_ids(void)
{
    static const cog_state_t boundary[] = {
        0U,
        COG_STATE_MAX,
        COG_TRANSITION_END,
    };

    if (cog_state_transition_allowed(boundary, 0U) != COG_OK) {
        return 1;
    }
    if (cog_state_transition_allowed(boundary, COG_STATE_MAX) != COG_OK) {
        return 1;
    }
    if (cog_state_transition_allowed(boundary, COG_TRANSITION_SELF) !=
        COG_ERROR_TRANSITION_DENIED) {
        return 1;
    }
    return 0;
}

static int transition_null_list_returns_null_ptr(void)
{
    if (cog_state_transition_allowed(NULL, ST_IDLE) != COG_ERROR_NULL_PTR) {
        return 1;
    }
    if (cog_state_transition_allowed(NULL, COG_TRANSITION_SELF) !=
        COG_ERROR_NULL_PTR) {
        return 1;
    }
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

    CHECK("lookup finds every row", lookup_finds_every_row());
    CHECK("dispatch entry macro fields", dispatch_entry_macro_populates_fields());
    CHECK("lookup matches NULL-handler row", lookup_matches_null_handler_row());
    CHECK("lookup missing entry leaves out unchanged",
          lookup_missing_entry_leaves_out_unchanged());
    CHECK("lookup empty table returns NOT_FOUND",
          lookup_empty_table_returns_not_found());
    CHECK("lookup NULL table non-zero size returns NULL_PTR",
          lookup_null_table_nonzero_size_returns_null_ptr());
    CHECK("lookup presence-only with NULL out", lookup_presence_only_with_null_out());
    CHECK("lookup boundary ids", lookup_boundary_ids());
    CHECK("lookup reserved ids never match", lookup_reserved_ids_never_match());
    CHECK("lookup duplicate rows first match wins",
          lookup_duplicate_rows_first_match_wins());
    CHECK("transition allowed happy paths", transition_allowed_happy_paths());
    CHECK("transition denied cases", transition_denied_cases());
    CHECK("transition sentinel terminates list",
          transition_sentinel_terminates_list());
    CHECK("transition boundary ids", transition_boundary_ids());
    CHECK("transition NULL list returns NULL_PTR",
          transition_null_list_returns_null_ptr());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_state: ok\n");
    }
    return failures == 0U ? 0 : 1;
}
