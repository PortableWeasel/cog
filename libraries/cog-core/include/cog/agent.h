/**
 * @file agent.h
 * @brief Subagent lifecycle: spawn, cancel, status, and result.
 *
 * A subagent is a child agent loop that runs a specific task.  It may
 * execute locally (in the same core process) or remotely (on an
 * auxiliary core process).  The orchestrator decides placement.
 *
 * Threading model: agent functions are called from the thread running
 * cog_core_run().  They are not thread-safe.
 */
#ifndef COG_AGENT_H
#define COG_AGENT_H

#include <cog/result.h>
#include <cog/ipc.h>
#include <cog/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to break the core/agent include cycle. */
typedef struct cog_core cog_core_t;

/**
 * @brief Unique subagent identifier.
 *
 * Assigned by cog_agent_spawn.  Zero is invalid.
 */
typedef uint64_t cog_agent_id_t;

/**
 * @brief Subagent execution status.
 */
typedef enum cog_agent_status {
    COG_AGENT_STATUS_PENDING = 0,  /**< Created but not yet running. */
    COG_AGENT_STATUS_RUNNING,      /**< Currently executing. */
    COG_AGENT_STATUS_COMPLETED,    /**< Finished successfully. */
    COG_AGENT_STATUS_FAILED,       /**< Finished with an error. */
    COG_AGENT_STATUS_CANCELLED,    /**< Cancelled by parent or timeout. */
    COG_AGENT_STATUS_TIMEOUT,      /**< Exceeded its time limit. */
} cog_agent_status_t;

/**
 * @brief Configuration for spawning a subagent.
 */
typedef struct cog_agent_config {
    /** Task description / prompt for the subagent. */
    const char *task;

    /** Optional parent session ID, copied verbatim. NULL leaves the child
     * unlinked; no hidden parent/session identity is generated for routing. */
    const char *parent_session_id;

    /** Maximum execution time in milliseconds.  Zero = no limit. */
    uint32_t timeout_ms;

    /** If true, the subagent runs locally.  If false, the orchestrator
     *  decides placement (may be remote). */
    bool force_local;

    /** Optional fan-out group identifier.  Agents spawned with the same
     *  group id are part of the same fan-out operation.  Zero = no group. */
    uint64_t group_id;
    cog_peer_id_t owner_peer; /**< Explicit placement; 0 means local. Immutable. */
    uint64_t execution_epoch; /**< Nonzero caller attempt identity for remote
       execution; never reused across restart. For local owner_peer=0, zero
       resolves to the assigned agent ID in this core; discover it through the
       queued intent or copy_config before the first update. Local generated
       identity is scoped to this core lifetime, never a remote/restart token. */
    size_t max_status_bytes; /**< Including NUL; zero uses core reservation. */
    size_t max_result_bytes; /**< Including NUL; zero uses core reservation. */
    /** Optional explicit child-session constructor reservations. Borrowed only
     * through spawn, never retained or returned by copy_config. id must NULL:
     * registry generates a fresh owned child identity. NULL keeps prior defaults.
     * Invalid bounds/allocation failure leave registry and event queue unchanged.
     * Context reservations are necessary for pinned local instruction history. */
    const cog_session_config_t *session_config;
} cog_agent_config_t;

/**
 * @brief Spawn a subagent.
 *
 * Reserves one bounded core-owned record/child session and atomically queues
 * COG_EVENT_SUBAGENT_SPAWN with cog_event_agent_intent_t under the lease in
 * event.h. Nothing executes until the application's static dispatch handles it.
 * The application selects owner_peer using the orchestrator before this call;
 * force_local requires owner_peer=0. No connection wait or nested agent loop.
 * Remote work uses the copied nonzero execution_epoch in every control message.
 * Only an authenticated current-owner update can change the record. Spawn
 * copies task/parent ID into reserved storage; failure dispatches nothing.
 * Match each successful terminal agent with cog_agent_release (or core_free).
 * The handler uses copy_config to retrieve owned task/parent/placement/epoch
 * and checks is_cancelled before starting effects, including cancel admitted
 * while spawn was still queued. For owner_peer!=0, the application forwards its
 * validated bounded remote protocol; core does not know or silently send it.
 * Core gains no provider/orchestrator implementation dependency.
 *
 * @param[in,out] core   Registry-host core. The explicit parent_session_id may
 * name an application session hosted by another executor; core does not borrow,
 * load, mutate or claim ownership of that parent through the string. The child
 * session is owned by THIS registry core until release/core_free. Keep this core
 * alive through all child execution borrows, copied terminal delivery and ack.
 * @param[in]     config Subagent configuration; must be non-NULL.
 * @param[out]    out_id Receives the new agent id on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p config, or @p out_id is NULL.
 * @return COG_ERROR_INVALID_ARG if @p config->task is NULL or empty.
 * @return COG_ERROR_NOMEM if the explicit child-session constructor fails.
 * @return COG_ERROR_LIMIT if record/task/status/result reservation is full;
 * requested per-agent byte limits cannot exceed core constructor reservations.
 * @return COG_ERROR_UNAVAILABLE if no core has capacity.
 * @return COG_ERROR_OVERFLOW if the core event queue has no admission slot.
 * On any failure, no new record/child session/event/lease is retained, *out_id
 * is unchanged and no local or remote task is sent. Retry is a fresh admission.
 *
 * @post On success, the child session retains the optional supplied parent ID
 * and the subagent has exactly one queued spawn intent. No work has executed.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_spawn(cog_core_t *core,
                              const cog_agent_config_t *config,
                              cog_agent_id_t *out_id);

/**
 * @brief Copy the immutable resolved spawn configuration without consuming intent.
 * @param[in] core Owner core, one thread; no service/dispatch side effects.
 * @param[in] agent_id Known retained record, including before first update.
 * @param[out] out_config Full copied scalar config; task/parent point into storage.
 * @param[out] storage Caller UTF-8 bytes for task+NUL and optional parent+NUL;
 * NULL iff capacity=0. No copied string points into an agent/queue/session slot.
 * @param[in] capacity Writable bytes including terminators.
 * @param[out] out_required Exact total storage bytes on OK/OVERFLOW.
 * @return OK; NULL_PTR; NOT_FOUND after release/unknown ID; OVERFLOW changes only
 * required, writes neither config nor storage and preserves the same record.
 * @post On OK, actual owner/assigned nonzero epoch and resolved nonzero status/
 * result capacities are returned. Optional NULL parent remains NULL. All caller
 * input strings were copied at spawn; returned bytes survive dispatch/update/
 * release/core_free. Caller owns and may retain them for deferred local/remote work.
 * @note No allocation or implicit acknowledgment; repeated copies are safe.
 */
cog_result_t cog_agent_copy_config(const cog_core_t *core, cog_agent_id_t agent_id,
                                    cog_agent_config_t *out_config,
                                    char *storage, size_t capacity,
                                    size_t *out_required);

/**
 * @brief Borrow the exact registry-owned local child session for one executor.
 * @param[in,out] core Parent registry owner, one thread; may be called in this
 * record's leased spawn handler, not recursively for the same record.
 * @param[in] agent_id Retained local(owner_peer=0), nonterminal child record.
 * @param[out] session Mutable borrowed session, unchanged on failure.
 * @return OK; NULL_PTR; NOT_FOUND; ABORTED cancellation already admitted;
 * INVALID_STATE remote/terminal/already borrowed. No allocation, execution,
 * update/ack or intent consumption. Exactly one execution borrow at a time.
 * @post Child app/core uses THIS object and its exact copied ID/parent link,
 * never a replacement tree with the same ID. It borrows, never destroys it.
 * Parent release cannot consume this record while borrowed. core_free requires
 * all execution borrows returned after work drains, as it already requires
 * local/remote work fenced/drained. No native pointer is transferred remotely;
 * remote execution uses copied identity and its own guarded store context.
 * Borrow is a storage-lifetime lease, NOT a new execution/admission grant.
 * Returning it cannot authorize restarting the same accepted task; application
 * attempt/receipt state retains one-shot execution and terminal ownership.
 */
cog_result_t cog_agent_session_borrow(cog_core_t *core, cog_agent_id_t agent_id,
                                       cog_session_t **session);
/**
 * @brief Return one local execution borrow after complete child cleanup.
 * @param[in,out] core Live parent registry owner, one thread.
 * @param[in] agent_id Exact borrowed record.
 * @param[in] session Exact borrowed pointer; never freed by the child.
 * @return OK; NULL_PTR; NOT_FOUND; INVALID_STATE mismatch/no borrow. Failure
 * changes no borrow. OK does not release/ack the record or result.
 * @pre No child app/core/provider/tool/store guard or callback still borrows
 * this session: cancel/drain/reap, consuming provider/config cleanup and child
 * core destruction have completed. No allocation or effects.
 */
cog_result_t cog_agent_session_return(cog_core_t *core, cog_agent_id_t agent_id,
                                       cog_session_t *session);

/**
 * @brief Cancel a running subagent.
 *
 * Atomically marks cancellation admitted and queues COG_EVENT_SUBAGENT_CANCEL
 * with the same typed/leased payload and event-ID convention as spawn. It does
 * not set terminal status or execute/send cancellation itself. The application
 * cancels/drains its local runtime or forwards the correlated remote intent,
 * then applies one drained/reaped terminal update through apply_update.
 * Before terminal, repeated admitted cancel is OK without another event/lease
 * or extended deadline. A terminal already accepted wins and returns INVALID_STATE.
 * Queue-full first cancellation is OVERFLOW and changes no cancellation flag,
 * status, notification, event, lease or external work; caller may retry later.
 *
 * @param[in,out] core     Core instance.
 * @param[in]     agent_id Agent to cancel.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is already in a
 *         terminal state.
 * @return COG_ERROR_OVERFLOW if first cancel intent cannot enter the core queue.
 * Threading: owner-thread only, never signal-safe; no allocation or peer wait.
 */
cog_result_t cog_agent_cancel(cog_core_t *core, cog_agent_id_t agent_id);

/**
 * @brief Query the status of a subagent.
 *
 * @param[in]  core       Core instance.
 * @param[in]  agent_id   Agent to query.
 * @param[out] out_status Receives the current status.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p out_status is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_status(cog_core_t *core,
                               cog_agent_id_t agent_id,
                               cog_agent_status_t *out_status);

/**
 * @brief Record a status update for the parent session.
 *
 * Called by a subagent to report progress.  The status message is
 * recorded synchronously on the agent record, replacing any previously
 * recorded status message for this agent.
 *
 * This compatibility helper creates a bounded local RUNNING update. It obeys
 * apply_update/copy_update/ack_update retention: a prior unacknowledged update
 * returns AGAIN unchanged, never frees payload underlying a queued event.
 * The application routes a copied snapshot to the parent; no hidden enqueue
 * or implicit cross-core serialization occurs here.
 *
 * @param[in,out] core       Core instance (the subagent's core).
 * @param[in]     agent_id   This subagent's id.
 * @param[in]     status_msg Human-readable status message; copied.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p status_msg is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_LIMIT if reserved status bytes cannot fit; AGAIN while an
 * earlier notification remains unacknowledged.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_notify_parent(cog_core_t *core,
                                      cog_agent_id_t agent_id,
                                      const char *status_msg);

/**
 * @brief Get the final result of a completed subagent.
 *
 * The result is the subagent's final output string, copied into the
 * caller-provided buffer.
 *
 * @param[in]  core            Core instance.
 * @param[in]  agent_id        Agent to query.
 * @param[out] out_result      Caller-provided buffer for the result.
 * @param[in]  result_capacity Size of @p out_result in bytes.
 * @param[out] out_result_len  Receives the result length (excluding NUL).
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p out_result is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is not terminal.
 * @return COG_ERROR_OVERFLOW if the result does not fit; reports required bytes
 * excluding NUL, writes no output and retains result for retry. Results for
 * FAILED/CANCELLED/TIMEOUT are also readable; outcome comes from copy_update.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_result(cog_core_t *core,
                               cog_agent_id_t agent_id,
                               char *out_result,
                               size_t result_capacity,
                               size_t *out_result_len);

/**
 * @brief Get the session id for an agent.
 *
 * Used for IPC session-routed messaging between subagents.
 *
 * @param[in]  core           Core instance.
 * @param[in]  agent_id       Agent to query.
 * @param[out] out_session_id Receives the session id string.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p out_session_id is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_session_id(cog_core_t *core,
                                   cog_agent_id_t agent_id,
                                   const char **out_session_id);

/**
 * @brief Check if an agent has been cancelled.
 *
 * Used by handlers to discard late results from cancelled agents
 * (e.g. after a fan-out winner is selected).
 *
 * @param[in]  core          Core instance.
 * @param[in]  agent_id      Agent to check.
 * @param[out] out_cancelled Receives true if the agent is cancelled.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p out_cancelled is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_is_cancelled(cog_core_t *core,
                                     cog_agent_id_t agent_id,
                                     bool *out_cancelled);


/** @brief One bounded, ordered local/remote attempt update; borrowed input view. */
typedef struct cog_agent_update {
    cog_peer_id_t owner_peer;
    uint64_t execution_epoch;
    uint64_t sequence; /**< Starts at 1, contiguous per attempt; never wraps. */
    cog_agent_status_t status;
    cog_result_t outcome; /**< COMPLETED requires OK; failures carry failure code. */
    const char *data; /**< Complete UTF-8 status/result, no embedded NUL. */
    size_t data_len;
} cog_agent_update_t;
/**
 * @brief Encode one lifecycle notification in a portable bounded IPC payload.
 * @param[in] agent_id Nonzero spawned ID. @param[in] update Complete update.
 * @param[out] output Wire bytes; NULL iff capacity=0. @param[in] capacity Bytes.
 * @param[out] required Exact wire bytes on OK/OVERFLOW.
 * @return OK; OVERFLOW changes required only; NULL_PTR; INVALID_ARG; LIMIT if
 * data exceeds uint32. No allocation/effects. Version 1 layout: magic CGA1,
 * agent ID/owner peer/epoch/sequence u64, status u8 (enum values 0..5), outcome
 * signed i32 two's-complement, data length u32, exact UTF-8 data without NUL.
 * Integers big-endian; no native padding/pointers. Unknown status/outcome codes
 * are rejected, not guessed. Receiving app verifies transport peer/epoch.
 */
cog_result_t cog_agent_update_encode(cog_agent_id_t agent_id,
                                      const cog_agent_update_t *update,
                                      void *output, size_t capacity,
                                      size_t *required);
/**
 * @brief Decode a complete CGA1 payload into independent caller storage.
 * @param[in] input Wire bytes. @param[in] input_len Exact payload bytes.
 * @param[out] agent_id Decoded nonzero ID. @param[out] update Decoded metadata.
 * @param[out] storage Copied data+NUL; NULL iff capacity=0.
 * @param[in] capacity Bytes. @param[out] required Exact data+NUL bytes.
 * @return OK; OVERFLOW writes required only; NULL_PTR; UNSUPPORTED for version;
 * PROTOCOL for invalid status/outcome/UTF-8/length or trailing bytes. Outputs
 * unchanged on failure except required on OVERFLOW; no allocation/ingestion.
 */
cog_result_t cog_agent_update_decode(const void *input, size_t input_len,
                                      cog_agent_id_t *agent_id,
                                      cog_agent_update_t *update, char *storage,
                                      size_t capacity, size_t *required);
/**
 * @brief Atomically ingest a validated current-attempt status or terminal result.
 * @param[in,out] core Parent/core owner, one thread, no recursive run.
 * @param[in] agent_id Spawned record.
 * @param[in] update Copied before return; source peer/epoch must match spawn.
 * @return OK; NULL_PTR; NOT_FOUND; LIMIT for constructor capture ceiling;
 * AGAIN if a distinct prior notification is unacknowledged; INVALID_STATE for
 * stale epoch/sequence, late RUNNING/COMPLETED after cancel admission, or
 * illegal transition; PROTOCOL for conflicting duplicate/content/status. Failure
 * changes no record or upstream cursor. Identical last update is idempotent OK.
 * @note App validates IPC peer/envelope before ingress. Parent may synthesize
 * FAILED/TIMEOUT only after it fenced/reaped that attempt; never auto-restart
 * effects. PENDING->RUNNING or terminal; RUNNING->terminal; terminal immutable.
 * Admitted cancel rejects later RUNNING/COMPLETED, but accepts one correlated
 * drained/reaped CANCELLED/TIMEOUT or fenced FAILED terminal for that attempt;
 * identical retries are idempotent. A terminal accepted before cancel wins.
 * Successful ingestion retains one notification until explicit ack; the app
 * copies it to independently owned event storage before cog_core_enqueue.
 */
cog_result_t cog_agent_apply_update(cog_core_t *core, cog_agent_id_t agent_id,
                                     const cog_agent_update_t *update);
/**
 * @brief Copy latest retained attempt snapshot, including pending notification.
 * @param[in] core Core owner. @param[in] agent_id Known record.
 * @param[out] update Metadata unchanged unless OK; data points into storage.
 * @param[out] storage Caller-owned UTF-8 bytes; NULL iff capacity=0.
 * @param[in] capacity Bytes including NUL. @param[out] required Exact bytes.
 * @return OK; AGAIN before any update; NULL_PTR; NOT_FOUND; OVERFLOW changes
 * only required and retains metadata/data. No consumption; repeated copies safe.
 * @note Owner thread. Copied pointers survive later ingestion and core_free.
 */
cog_result_t cog_agent_copy_update(const cog_core_t *core, cog_agent_id_t agent_id,
                                    cog_agent_update_t *update, char *storage,
                                    size_t capacity, size_t *required);
/**
 * @brief Acknowledge notification only after copying/queueing it successfully.
 * @param[in,out] core Core. @param[in] agent_id Known record.
 * @param[in] execution_epoch Current attempt. @param[in] sequence Copied sequence.
 * @return OK (duplicate ack of latest is OK); NULL_PTR; NOT_FOUND;
 * INVALID_STATE for stale/mismatched ack. Snapshot/result remains queryable.
 * @note Owner thread, no allocation, no tool/provider effects.
 */
cog_result_t cog_agent_ack_update(cog_core_t *core, cog_agent_id_t agent_id,
                                   uint64_t execution_epoch, uint64_t sequence);
/**
 * @brief Release a terminal agent's reserved record; never abandons execution.
 * @param[in,out] core Owner. @param[in] agent_id Terminal, notification acked,
 * with no queued/in-dispatch spawn/cancel intent lease.
 * @return OK; NULL_PTR; NOT_FOUND; INVALID_STATE if active, pending notification
 * or any intent/execution-session lease remains (including inside its own handler). No change on
 * failure; retry after handler return. Queued payload storage cannot be freed.
 * @note Owner thread. IDs are never reused; invalidates query views, not copied
 * event payloads. core_free requires remote/local owned work fenced/drained.
 */
cog_result_t cog_agent_release(cog_core_t *core, cog_agent_id_t agent_id);

#ifdef __cplusplus
}
#endif

#endif /* COG_AGENT_H */
