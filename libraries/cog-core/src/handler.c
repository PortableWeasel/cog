/**
 * @file handler.c
 * @brief Implementation of the handler dispatch contract.
 *
 * handler.h declares the handler result type (@c cog_handler_result_t)
 * and the handler function signature (@c cog_handler_fn); it declares
 * no functions.  The dispatch primitives that route a (state, event)
 * pair to a handler are declared in state.h and implemented here: they
 * are the "handler dispatch" of the core state machine.
 *
 * MODULE OWNERSHIP NOTE: @c cog_state_lookup and
 * @c cog_state_transition_allowed are declared in state.h.  They are
 * implemented in this translation unit because ticket 9298184 owns
 * handler dispatch (handler function pointer invocation and error
 * propagation).  The parallel state.c ticket (6b1c97b) also targets
 * these functions; the integration ticket 6044b51 resolves the overlap
 * by keeping a single implementation.  This file does not add any
 * dynamic registration and holds no global mutable state.
 */
#include <cog/state.h>

cog_result_t cog_state_lookup(const cog_dispatch_entry_t *table, size_t table_size,
                              cog_state_t state, cog_event_id_t event,
                              const cog_dispatch_entry_t **out_entry)
{
    size_t i;

    if (table == NULL) {
        if (table_size != 0U) {
            return COG_ERROR_NULL_PTR;
        }
        return COG_ERROR_NOT_FOUND;
    }

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

    for (i = 0U; allowed[i] != COG_TRANSITION_END; ++i) {
        if (allowed[i] == transition) {
            return COG_OK;
        }
    }

    return COG_ERROR_TRANSITION_DENIED;
}