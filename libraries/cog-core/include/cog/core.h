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

#include <cog/agent.h>
#include <cog/event.h>
#include <cog/handler.h>
#include <cog/ipc.h>
#include <cog/obs.h>
#include <cog/result.h>
#include <cog/session.h>
#include <cog/session_store.h>
#include <cog/state.h>
#include <cog/tool.h>

/* Forward declaration — full definition in cog-provider/provider_vtable.h */
typedef struct cog_provider cog_provider_t;

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

    /**
     * @brief Borrowed caller-owned context.
     *
     * The core stores this pointer verbatim and returns it from
     * @c cog_core_user.  It is never freed, dereferenced, or mutated by
     * the core.  The caller retains full ownership and must ensure the
     * pointed-to object remains valid for the lifetime of the core.
     *
     * May be NULL.  Zero-initialising the config yields NULL.
     */
    void *user;

    /**
     * @brief Optional IPC endpoint for multi-process operation.
     *
     * May be NULL for single-process operation (the default).  If
     * non-NULL, the core takes ownership of the endpoint: it drains
     * incoming connections and messages during @c cog_core_run,
     * enqueuing them as COG_EVENT_IPC_* events into the local queue,
     * and destroys the endpoint in @c cog_core_free.  The endpoint must
     * be bound (listening or connected) before it is passed to the
     * core; an unbound endpoint is accepted but never drained.
     *
     * Ownership transfers to the core at @c cog_core_new: the caller
     * must not use or free the endpoint after that call.
     */
    cog_ipc_endpoint_t *ipc_endpoint;

    /**
     * @brief Local peer identity within the IPC fabric.
     *
     * Zero means "unidentified" — the IPC layer assigns an id on
     * connection.  Non-zero values must be unique across the fabric.
     */
    cog_peer_id_t local_peer_id;

    /**
     * @brief Optional active LLM provider instance.
     *
     * May be NULL for tests or no-provider operation; if non-NULL, the
     * provider is used by handlers to make LLM calls.  The caller creates
     * the provider with cog_provider_connect and retains ownership; the
     * core does not free it.
     */
    cog_provider_t *provider;

    /**
     * @brief Optional observability context.
     *
     * If non-NULL, the core logs lifecycle events and errors through
     * this context.  The caller retains ownership.
     */
    cog_obs_t *obs;

    /**
     * @brief Optional session store for persistence.
     *
     * If non-NULL and the store's auto_save flag is true, the core
     * auto-saves the session once after each successfully handled
     * @c COG_EVENT_LLM_COMPLETE exchange, immediately after the valid
     * state transition is applied.  Auto-save errors are propagated by
     * @c cog_core_run.  The caller retains ownership of the store.
     */
    cog_store_t *store;
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
 * @post On success, @p *out is a valid core and the caller owns it.  If
 *       @p config->ipc_endpoint is non-NULL, the core owns it and will
 *       destroy it in @c cog_core_free.
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
 * @post All storage owned by @p core is released, including the IPC
 *       endpoint (if configured), its accepted peers, and any in-flight
 *       IPC message payloads.  @p core must not be used after this call.
 *       The session supplied at construction is not freed; the caller
 *       retains ownership of it.
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
 * @return Another error code if dispatch or auto-save fails.
 *
 * @pre @p core has been created with @c cog_core_new and is not already
 *      running.
 *
 * @post The core is no longer running.
 *
 * Threading: single-threaded driver.  Only one thread may call
 * @c cog_core_run at a time.  @c cog_core_stop may be called from
 * another thread or a signal handler.
 *
 * @note Auto-save, when enabled via the configured store, triggers only
 *       after a @c COG_EVENT_LLM_COMPLETE handler returns @c COG_OK and
 *       its requested transition is allowed and applied.  It does not
 *       trigger on handler failure, denied transition, cancellation,
 *       shutdown, or streaming chunks.
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
 * @brief Enqueue an event in the core's internal event queue.
 *
 * This is the primary way external code feeds events into a running core.
 * The event is copied into the queue; the call returns immediately and
 * does not invoke the state-machine handler.
 *
 * @param[in,out] core  Core to receive the event.
 * @param[in]     event Event to enqueue.
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
cog_result_t cog_core_enqueue(cog_core_t *core, const cog_event_t *event);

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

/**
 * @brief Return the borrowed caller-owned context supplied at construction.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The pointer exactly as supplied in @p config->user, or NULL if
 *         @p core is NULL or if no user context was configured.
 *
 * @note The returned pointer remains owned by the caller; the core does
 *       not free it or modify the object it points to.
 *
 * Threading: not thread-safe.  The same context instance must only be
 * used by one core / one run thread; the caller must provide any external
 * synchronisation if the pointer is shared across threads.
 */
void *cog_core_user(const cog_core_t *core);

/**
 * @brief Return the IPC endpoint owned by the core.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The IPC endpoint, or NULL if single-process or @p core is NULL.
 *
 * @note The returned endpoint is owned by the core and is destroyed by
 *       @c cog_core_free; the caller must not free it.
 *
 * Threading: not thread-safe.
 */
cog_ipc_endpoint_t *cog_core_ipc(cog_core_t *core);

/**
 * @brief Return the active LLM provider.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The provider instance, or NULL if @p core is NULL.
 *
 * Threading: not thread-safe.
 */
cog_provider_t *cog_core_provider(cog_core_t *core);

/**
 * @brief Return the observability context.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The observability context, or NULL if not configured or
 *         @p core is NULL.
 *
 * Threading: not thread-safe.
 */
cog_obs_t *cog_core_obs(cog_core_t *core);

/**
 * @brief Return the session store.
 *
 * @param[in] core Core to query; may be NULL.
 * @return The session store, or NULL if not configured or @p core is NULL.
 *
 * Threading: not thread-safe.
 */
cog_store_t *cog_core_store(cog_core_t *core);

/* ------------------------------------------------------------------ */
/* Pending message queue (interrupt / queue-next support)              */
/* ------------------------------------------------------------------ */

/**
 * @brief Queue a user message for delivery after the current turn.
 *
 * When the user types a message while the LLM is still streaming, the
 * UI can call this to hold the message in a session-scoped FIFO.  The
 * core automatically dequeues and injects the message as a new
 * @c COG_EVENT_USER_INPUT when the current turn completes.
 *
 * @param[in,out] core       Core to queue into.
 * @param[in]     session_id Session to queue for; must not be NULL.
 * @param[in]     text       Message text; copied.  Must not be NULL.
 * @param[in]     text_len   Length of @p text in bytes.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p session_id, or @p text is NULL.
 * @return COG_ERROR_OVERFLOW if the session's pending queue is full
 *         (default capacity: 8 messages).
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, the message is appended to the session's pending
 *       queue and will be delivered after the current turn.
 *
 * Threading: not thread-safe.  Must be called from the same thread
 * that runs @c cog_core_run.
 */
cog_result_t cog_core_queue_message(cog_core_t *core,
                                     const char *session_id,
                                     const char *text,
                                     size_t text_len);

/**
 * @brief Dequeue the next pending message for a session.
 *
 * Removes and returns the oldest pending message for the given session.
 * Called internally by the core when a turn completes; may also be
 * called by the application to inspect or drain the queue.
 *
 * @param[in]  core        Core to query.
 * @param[in]  session_id  Session to dequeue from; must not be NULL.
 * @param[out] out_buf     Buffer that receives the message text,
 *                         NUL-terminated.
 * @param[in]  buf_capacity Size of @p out_buf in bytes, including
 *                          space for the NUL terminator.
 * @param[out] out_len     Receives the message length in bytes,
 *                         excluding the terminator.  May be NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p session_id, or @p out_buf
 *         is NULL.
 * @return COG_ERROR_NOT_FOUND if the session's pending queue is empty.
 * @return COG_ERROR_OVERFLOW if the message does not fit in
 *         @p buf_capacity.  On this error @p *out_len receives the
 *         required capacity, excluding the terminator.
 *
 * @post On success, the message is removed from the queue and copied
 *       into @p out_buf.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_core_dequeue_message(cog_core_t *core,
                                       const char *session_id,
                                       char *out_buf,
                                       size_t buf_capacity,
                                       size_t *out_len);

/**
 * @brief Return the number of pending messages for a session.
 *
 * @param[in]  core        Core to query.
 * @param[in]  session_id  Session to check; must not be NULL.
 * @param[out] out_count   Receives the number of pending messages.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p session_id, or @p out_count
 *         is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_core_pending_count(const cog_core_t *core,
                                     const char *session_id,
                                     size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* COG_CORE_H */
