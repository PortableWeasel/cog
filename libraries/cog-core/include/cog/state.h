/**
 * @file state.h
 * @brief State identifiers and the table-driven dispatch contract.
 *
 * The core state machine is driven by a static const array of
 * @c cog_dispatch_entry_t rows.  Each row maps a (state, event) pair to
 * a handler and a list of allowed target states.  This keeps the routing
 * table fully visible to code indexers and avoids dynamic registration.
 *
 * Threading model: the dispatch table is read-only after construction and
 * may be shared by multiple core instances.  Individual core instances are
 * not thread-safe.
 */
#ifndef COG_STATE_H
#define COG_STATE_H

#include <cog/event.h>
#include <cog/handler.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reserved transition id: remain in the current state.
 *
 * Handlers may return this value in @c cog_handler_result_t.transition
 * to request a self-transition.
 */
#define COG_TRANSITION_SELF 0xFFFEu

/**
 * @brief Reserved sentinel that terminates an allowed-transition list.
 *
 * Every list pointed to by @c cog_dispatch_entry_t.allowed_transitions
 * must end with this value.
 */
#define COG_TRANSITION_END 0xFFFFu

/**
 * @brief Application state identifiers must be in the range
 *        [0, @c COG_TRANSITION_SELF - 1].
 */
#define COG_STATE_MAX 0xFFFDu

/**
 * @brief One row of the static dispatch table.
 *
 * All fields are resolved at compile or link time; the table itself is
 * stored in read-only memory.
 */
typedef struct cog_dispatch_entry {
    cog_state_t state;              /**< Source state for this row. */
    cog_event_id_t event;           /**< Event id that triggers this row. */
    cog_handler_fn handler;         /**< Handler invoked for this pair. */
    const cog_state_t *allowed_transitions; /**< @c COG_TRANSITION_END-terminated
                                                 list of permitted target states. */
} cog_dispatch_entry_t;

/**
 * @brief Convenience macro for populating a static dispatch table.
 *
 * Usage:
 * @code
 * static const cog_state_t idle_transitions[] = {
 *     COG_STATE_RUNNING,
 *     COG_TRANSITION_SELF,
 *     COG_TRANSITION_END,
 * };
 *
 * static const cog_dispatch_entry_t table[] = {
 *     COG_DISPATCH_ENTRY(COG_STATE_IDLE, COG_EVENT_START, idle_start, idle_transitions),
 *     ...
 * };
 * @endcode
 *
 * @param[in] state_       Source state id.
 * @param[in] event_       Event id.
 * @param[in] handler_     Handler function pointer.
 * @param[in] transitions_ Allowed-transition list.
 */
#define COG_DISPATCH_ENTRY(state_, event_, handler_, transitions_) \
    { (state_), (event_), (handler_), (transitions_) }

/**
 * @brief Search the dispatch table for the handler that matches
 *        (@p state, @p event).
 *
 * This is a low-level lookup helper used by @c cog_core_run.  Most
 * callers should dispatch through the core instead.
 *
 * @param[in] table       Static dispatch table.
 * @param[in] table_size  Number of rows in @p table.
 * @param[in] state       Current state.
 * @param[in] event       Event id.
 * @param[out] out_entry  Receives a pointer to the matching table row on
 *                        success; unchanged on failure.  May be NULL if
 *                        only presence is required.
 * @return COG_OK if a matching row was found.
 * @return COG_ERROR_NULL_PTR if @p table is NULL and @p table_size is
 *         non-zero.
 * @return COG_ERROR_NOT_FOUND if no row matches or if the table is empty
 *         (@p table is NULL and @p table_size is zero).
 *
 * @pre @p table may be NULL only when @p table_size is zero.
 *
 * Threading: the table must not be modified during the call.
 */
cog_result_t cog_state_lookup(const cog_dispatch_entry_t *table, size_t table_size,
                              cog_state_t state, cog_event_id_t event,
                              const cog_dispatch_entry_t **out_entry);

/**
 * @brief Validate that @p transition appears in an allowed-transition list.
 *
 * @param[in] allowed  @c COG_TRANSITION_END-terminated list.
 * @param[in] transition Transition id to validate.
 * @return COG_OK if @p transition is present in @p allowed.
 * @return COG_ERROR_NULL_PTR if @p allowed is NULL.
 * @return COG_ERROR_TRANSITION_DENIED if @p transition is not allowed.
 *
 * Threading: the list must not be modified during the call.
 */
cog_result_t cog_state_transition_allowed(const cog_state_t *allowed,
                                          cog_state_t transition);

#ifdef __cplusplus
}
#endif

#endif /* COG_STATE_H */
