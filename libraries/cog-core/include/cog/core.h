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
 * @brief Fallible nonblocking external service hook, including timers/net/tools.
 * @param[in,out] arg Caller-owned context retained until core_free.
 * @param[in] now_ms Nondecreasing monotonic milliseconds from the core clock.
 * @param[out] out_next_deadline_ms Absolute next desired service time;
 * UINT64_MAX means none. Past/due means no idle wait.
 * @return COG_OK or AGAIN for normal operation; any other result stops run()
 * and propagates unchanged. Hook may enqueue; OVERFLOW must be handled by
 * retaining the source item, never losing an accepted provider/tool result.
 * @note Owner thread, no waits, recursion into run(), or destruction of core.
 */
typedef cog_result_t (*cog_service_fn)(void *arg, uint64_t now_ms,
                                      uint64_t *out_next_deadline_ms);
/**
 * @brief Fallible bounded idle hook for stdin/IPC readiness.
 * @param[in,out] arg Caller context.
 * @param[in] max_wait_ms Maximum relative wait, possibly zero.
 * @return COG_OK or AGAIN; other errors propagate from run().
 * @note Must return within supplied budget subject to OS scheduling and on
 * stop wakeup. Must not drain work indefinitely. Owner thread only.
 */
typedef cog_result_t (*cog_idle_fn)(void *arg, uint32_t max_wait_ms);

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
     * Core reserves event descriptors and ipc_payload_bytes before running.
     * Before consuming a complete endpoint frame it reserves both an event
     * slot and enough owned payload storage. Full reservations backpressure
     * that peer without consuming the frame; other service/dispatch continues.
     * Copied payload lives until handler return, then its arena slice is reused.
     * No per-message allocation occurs. Accepted endpoints are explicit accept
     * constructors paired with endpoint_free and bounded by ipc_peer_capacity;
     * core tracking slots are constructor-reserved and listener linkage may be
     * intrusive, without a separate growing peer array.
     * Ownership transfers to the core only on successful @c cog_core_new: the caller
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

    /** Nonblocking service, called before first dispatch, before every idle
     * wait, at due deadlines, and at least every service_interval dispatches
     * even while the queue remains nonempty. IPC and stop checks receive the
     * same fairness. Core links no net/provider/toolkit implementation. */
    cog_service_fn service_fn;
    void *service_arg; /**< Borrowed context, not freed by core. */
    size_t service_interval; /**< Max dispatched events between service; default 16. */
    cog_idle_fn idle_fn; /**< Bounded wait; NULL uses bounded platform wait/yield. */
    void *idle_arg; /**< Borrowed idle context. */
    uint32_t max_idle_wait_ms; /**< Default 10; reduced to earliest deadline. */
    size_t ipc_peer_capacity; /**< Maximum accepted peers; default 16. */
    size_t ipc_max_frame_bytes; /**< Core admission ceiling; default 1 MiB. A
       larger endpoint-retained frame closes only that offending peer with a
       reported LIMIT; never loops forever on receive OVERFLOW or truncates it. */
    size_t ipc_payload_bytes; /**< Total constructor-reserved queued IPC payload
       arena; default 8 MiB, must be >= ipc_max_frame_bytes. No runtime growth. */
    size_t ipc_service_quantum; /**< Max IPC units per fairness step; default 16. */
    size_t max_agents; /**< Constructor-reserved agent records; default 16. */
    size_t max_agent_message_bytes; /**< Per-agent copied task/status/result bytes;
       default 65536. Replacement/ingestion never grows it; LIMIT is atomic. */
    size_t pending_session_capacity; /**< Reserved pending FIFO owners; default 17. */
    size_t pending_messages_per_session; /**< Reserved entries/owner; default 8. */
    size_t pending_message_bytes; /**< Bytes/entry including NUL; default 65536. */
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
 *         @p config->dispatch_table_size is zero, or legacy wait_fn is combined
 *         with service_fn/idle_fn (unbounded legacy waiting is incompatible).
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
 * function is called. Queued agent intents are discarded and their leases
 * released before registry storage is destroyed; caller first fences/drains
 * all already-started owned work. No queued intent is executed by destruction.
 */
void cog_core_free(cog_core_t *core);

/**
 * @brief Run the agent event loop.
 *
 * Dispatches bounded batches through the static table and services IPC,
 * deadlines, stop requests and config.service_fn even while work is queued.
 * Idle waits are capped by max_idle_wait_ms and the next service deadline;
 * service/idle errors are returned unchanged, AGAIN is normal no-progress.
 * Legacy wait_fn is supported only without the new service/idle hooks.
 * Returns when @c cog_core_stop is called or a fatal error occurs.
 *
 * @param[in,out] core Core to run.
 * @return COG_OK if the loop exited because @c cog_core_stop was called.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_INVALID_STATE if the core is already running.
 * @return Another error code if dispatch, service, idle, IPC or auto-save fails.
 *
 * @pre @p core has been created with @c cog_core_new and is not already
 *      running.
 *
 * @post A popped agent intent lease is released after its dispatch/handler
 * returns on every success/error path; no pointer escapes to deferred work.
 * Unprocessed intents remain queued/leased for a later run or explicit teardown.
 * The core is no longer running. Application-owned asynchronous work
 * remains explicitly owned; the application cancels/drains/releases it.
 * @note Hooks/handlers must perform bounded CPU/I/O work. Fairness bounds
 * dispatch counts and requested waits, not arbitrary handler or OS latency.
 * IPC uses endpoint_service plus nonblocking retained recv/send; readiness of
 * a frame prefix never authorizes a blocking full-frame read. Accepted peer
 * count and per-step work are constructor-bounded; excess peers are refused.
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
 * @return COG_ERROR_INVALID_ARG for SUBAGENT_SPAWN/CANCEL kinds, which only
 * agent_spawn/cancel may originate through atomic leased admission. This does
 * not reserve their numeric dispatch IDs for unrelated application events.
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
 * @return The borrowed provider instance, or NULL if @p core is NULL.
 * @note Replaced only by set_provider; callers must not retain an old reference
 * across replacement or its owner destruction. Core never frees the provider.
 *
 * Threading: not thread-safe.
 */
cog_provider_t *cog_core_provider(cog_core_t *core);

/**
 * @brief Replace the borrowed provider reference without driving either provider.
 * @param[in,out] core Core whose observable provider reference is updated.
 * @param[in] provider Borrowed replacement; NULL detaches the reference.
 * @return COG_OK; NULL_PTR if core is NULL. No other failure is possible.
 * @post provider() returns exactly the replacement. No allocation, callbacks,
 * provider calls, dispatch, clock/queue/session mutation or ownership transfer.
 * @pre Owner thread with no provider call or other outstanding handler borrow
 * of the old pointer. Service/idle hooks and a handler before acquiring its
 * provider are permitted; the caller proves no old borrow will be reused. It keeps
 * each provider alive while any caller/handler still borrows it and disconnects
 * the old instance only after its work is drained and references are replaced.
 * Core cannot infer provider activity or validate its implementation. This does
 * not authorize cancelling/replacing live work; composition owns that decision.
 * @note Not thread-safe or signal-safe. Setting the same pointer is a no-op.
 */
cog_result_t cog_core_set_provider(cog_core_t *core, cog_provider_t *provider);


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
 * @return COG_ERROR_LIMIT if text/session capacity exceeds the core constructor
 * reservation. No runtime allocation; failure leaves the FIFO unchanged.
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
 *         required capacity, excluding the terminator. No output bytes are
 *         written; the same oldest entry remains queued across service calls.
 *
 * @post Auto-injection logically removes an entry only after successful core
 *       enqueue; full event queues retain it without duplicate injection. Its
 *       backing text slot stays leased until the consuming handler returns, or
 *       bytes are first copied into another constructor-reserved event owner.
 *       Reusing a FIFO slot must never overwrite an already queued prompt.
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
