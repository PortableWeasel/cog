/**
 * @file state.c
 * @brief Implementation of the table-driven dispatch contract declared in
 *        cog/state.h.
 *
 * The dispatch table is a static const array of @c cog_dispatch_entry_t
 * rows.  This module provides the two pure lookup helpers that
 * @c cog_core_run uses to route (state, event) pairs to handlers and to
 * validate handler transition requests against an entry's allowed list.
 *
 * The module holds no global state: every input is caller-provided and
 * the tables are read-only.  Lookup is a deterministic linear scan in
 * table order, so the first matching row always wins.
 *
 * Ticket: 6b1c97b (Stage 12.1c — Implement state.c)
 */

#include "cog/state.h"

cog_result_t cog_state_lookup(const cog_dispatch_entry_t *table, size_t table_size,
                              cog_state_t state, cog_event_id_t event,
                              const cog_dispatch_entry_t **out_entry)
{
    size_t i;

    /* A NULL table is only legal when the table is empty (size zero). */
    if (table == NULL && table_size != 0U) {
        return COG_ERROR_NULL_PTR;
    }

    /* An empty table can never contain a match.  This covers both
     * (table == NULL, size == 0) and (table != NULL, size == 0). */
    if (table_size == 0U) {
        return COG_ERROR_NOT_FOUND;
    }

    /* Deterministic linear scan: the first row in table order whose
     * (state, event) pair matches is selected.  Rows are compared
     * structurally; a row that (incorrectly) uses a reserved state id is
     * simply never matched by a well-formed query. */
    for (i = 0U; i < table_size; ++i) {
        if (table[i].state == state && table[i].event == event) {
            if (out_entry != NULL) {
                *out_entry = &table[i];
            }
            return COG_OK;
        }
    }

    return COG_ERROR_NOT_FOUND;
}

cog_result_t cog_state_transition_allowed(const cog_state_t *allowed,
                                          cog_state_t transition)
{
    size_t i;

    if (allowed == NULL) {
        return COG_ERROR_NULL_PTR;
    }

    /* Scan until the COG_TRANSITION_END sentinel.  The sentinel itself is
     * never a match, so requesting COG_TRANSITION_END is always denied.
     * COG_TRANSITION_SELF is an ordinary list member and matches when it
     * appears in the list.  Entries after the sentinel are not part of
     * the list.  A list that is not END-terminated is a caller error and
     * cannot be detected here without reading past the end of the array. */
    for (i = 0U; ; ++i) {
        const cog_state_t candidate = allowed[i];
        if (candidate == COG_TRANSITION_END) {
            break;
        }
        if (candidate == transition) {
            return COG_OK;
        }
    }

    return COG_ERROR_TRANSITION_DENIED;
}
