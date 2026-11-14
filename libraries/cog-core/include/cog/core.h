/**
 * @file core.h
 * @brief Public umbrella header for cog-core.
 *
 * cog-core is the heart of every Cog process: it owns the agent loop,
 * the session, the event queue, and the table-driven state machine.
 * A consumer of this library needs only:
 *
 * @code
 * #include "cog/core.h"
 * @endcode
 *
 * All state is caller-allocated and passed by pointer.  The library never
 * allocates implicitly: constructors return heap objects with matching
 * destructors, and configuration tables are supplied by the caller.
 *
 * Threading model: a single @c cog_core_t instance is intended to be
 * driven by a single thread calling @c cog_core_run.  @c cog_core_stop
 * may be called from another thread or a signal handler to request a
 * graceful exit.  Multi-threaded operation may be layered on top later
 * via external locking or the IPC fabric.
 */
#ifndef COG_CORE_H
#define COG_CORE_H

#include <cog/event.h>
#include <cog/handler.h>
#include <cog/result.h>
#include <cog/session.h>
#include <cog/state.h>
#include <cog/tool.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque agent core.
 *
 * Created with @c cog_core_new and destroyed with @c cog_core_free.
 */
typedef struct cog_core cog_core_t;

/**
 * @brief Hook called by @c cog_core_run when the event queue is empty.
 *
 * The hook may block until a new event arrives, yield the CPU, or poll an
 * external source.  It must return promptly after @c cog_core_stop has
 * been called.
 *
 * @param[in,out] arg Caller-provided argument from @c cog_core_config_t.
 */
typedef void (*cog_wait_fn)(void *arg);

/**
 * @brief Configuration passed to @c cog_core_new.
 *
 * Callers should zero-initialise this struct to obtain defaults; adding
 * fields later will not break callers that zero-init.
 */
typedef struct cog_core_config {
    /**
     * @brief Session that owns the conversation context.
     *
     * Must be non-NULL and must outlive the core.  The caller creates it
     * with @c cog_session_new and retains ownership.
     */
    cog_session_t *session;

    /**
     * @brief Static const state-machine dispatch table.
     *
     * Must be non-NULL.  The table must remain valid for the lifetime of
     * the core.
     */
    const cog_dispatch_entry_t *dispatch_table;

    /**
     * @brief Number of rows in @p dispatch_table.
     *
     * Must be greater than zero.
     */
    size_t dispatch_table_size;

    /**
     * @brief Optional static const tool table.
     *
     * May be NULL if no tools are available.  If non-NULL, the table must
     * remain valid for the lifetime of the core.
     */
    const cog_tool_table_t *tool_table;

    /**
     * @brief Capacity of the core's internal event queue.
     *
     * Zero selects a sensible default.
     */
    size_t event_queue_capacity;

    /**
     * @brief Optional wait hook called when the event queue is empty.
     *
     * If NULL, the run loop busy-waits with a short yield.  See
     * @c cog_wait_fn for the contract.
     */
    cog_wait_fn wait_fn;

    /**
     * @brief Caller argument passed to @p wait_fn.
     */
    void *wait_arg;
} cog_core_config_t;

/**
 * @brief Create a new agent core.
 *
 * @param[out] out   Uninitialised pointer; set to the new core on success.
 * @param[in]  config Configuration; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out or @p config is NULL.
 * @return COG_ERROR_INVALID_ARG if @p config->session is NULL, if
 *         @p config->dispatch_table is NULL, or if
 *         @p config->dispatch_table_size is zero.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid core and the caller owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_core_new(cog_core_t **out, const cog_core_config_t *config);

/**
 * @brief Destroy a core.
 *
 * @param[in,out] core Core to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p core is released.  @p core must not be
 *       used after this call.  The session supplied at construction is
 *       not freed; the caller retains ownership of it.
 *
 * Threading: not thread-safe.  @p core must not be running when this
 * function is called.
 */
void cog_core_free(cog_core_t *core);

/**
 * @brief Run the agent event loop.
 *
 * Drains the internal event queue, dispatches events through the static
 * table, and waits via @p config->wait_fn when no events are available.
 * Returns when @c cog_core_stop is called or a fatal error occurs.
 *
 * @param[in,out] core Core to run.
 * @return COG_OK if the loop exited because @c cog_core_stop was called.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_INVALID_STATE if the core is already running.
 * @return Another error code if dispatch fails.
 *
 * @pre @p core has been created with @c cog_core_new and is not already
 *      running.
 *
 * @post The core is no longer running.
 *
 * Threading: single-threaded driver.  Only one thread may call
 * @c cog_core_run at a time.  @c cog_core_stop may be called from
 * another thread or a signal handler.
 */
cog_result_t cog_core_run(cog_core_t *core);

/**
 * @brief Request the run loop to exit.
 *
 * @param[in,out] core Core to stop; may be NULL (no-op).
 *
 * @post The running flag is cleared.  The loop will exit at the next
 *       safe opportunity (after the current wait/dispatch cycle).
 *
 * Threading: may be called from any thread or a signal handler.  Must not
 * be called concurrently with @c cog_core_free on the same instance.
 */
void cog_core_stop(cog_core_t *core);

/**
 * @brief Push an event into the core's internal event queue.
 *
 * This is the primary way external code feeds events into a running core.
 *
 * @param[in,out] core  Core to receive the event.
 * @param[in]     event Event to dispatch.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p event is NULL.
 * @return COG_ERROR_OVERFLOW if the internal queue is full.
 *
 * @post The event is copied into the core's queue.
 *
 * Threading: single-producer with respect to the core's queue.  Safe to
 * call from the thread running @c cog_core_run, from the wait hook, or
 * from a thread designated as the producer.  Multi-threaded producers
 * require external synchronisation.
 */
cog_result_t cog_core_dispatch(cog_core_t *core, const cog_event_t *event);

/**
 * @brief Return the session associated with the core.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The session supplied at construction, or NULL if @p core is
 *         NULL.
 *
 * @note The returned session remains owned by the caller; do not free it
 *       while the core is alive.
 *
 * Threading: not thread-safe.
 */
cog_session_t *cog_core_session(cog_core_t *core);

/**
 * @brief Return the current state of the core's state machine.
 *
 * @param[in] core Core to query; may be NULL.
 * @return Current state id, or zero if @p core is NULL.
 *
 * Threading: not thread-safe for reliable results.
 */
cog_state_t cog_core_state(const cog_core_t *core);

/**
 * @brief Return true if the core is currently running its event loop.
 *
 * @param[in] core Core to query; may be NULL.
 * @return true if the core is running, false otherwise.
 *
 * Threading: may be called from any thread, but the result is a snapshot
 * and may change immediately after the call returns.
 */
bool cog_core_is_running(const cog_core_t *core);

#ifdef __cplusplus
}
#endif

#endif /* COG_CORE_H */
