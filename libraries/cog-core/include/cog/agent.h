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

    /** Parent session id.  The subagent's session is a child of this. */
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
       execution; never reused across restart. Local zero selects a core ID. */
    size_t max_status_bytes; /**< Including NUL; zero uses core reservation. */
    size_t max_result_bytes; /**< Including NUL; zero uses core reservation. */
} cog_agent_config_t;

/**
 * @brief Spawn a subagent.
 *
 * Reserves one bounded core-owned record/child session and queues spawn intent.
 * The application selects owner_peer using the orchestrator before this call;
 * force_local requires owner_peer=0. No connection wait or nested agent loop.
 * Remote work uses the copied nonzero execution_epoch in every control message.
 * Only an authenticated current-owner update can change the record. Spawn
 * copies task/parent ID into reserved storage; failure dispatches nothing.
 * match each successful terminal agent with cog_agent_release (or core_free).
 *
 * @param[in,out] core   Core instance that owns the parent session.
 * @param[in]     config Subagent configuration; must be non-NULL.
 * @param[out]    out_id Receives the new agent id on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p config, or @p out_id is NULL.
 * @return COG_ERROR_INVALID_ARG if @p config->task is NULL or empty.
 * @return COG_ERROR_NOMEM if the explicit child-session constructor fails.
 * @return COG_ERROR_LIMIT if record/task/status/result reservation is full;
 * requested per-agent byte limits cannot exceed core constructor reservations.
 * @return COG_ERROR_UNAVAILABLE if no core has capacity.
 *
 * @post On success, a child session exists with parent_session_id set
 *       and the subagent is queued for execution.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_spawn(cog_core_t *core,
                              const cog_agent_config_t *config,
                              cog_agent_id_t *out_id);

/**
 * @brief Cancel a running subagent.
 *
 * Sends a cancellation request to the subagent.  If the subagent is
 * remote, the cancellation is forwarded via IPC.  The subagent may
 * not stop immediately; poll cog_agent_status for terminal state.
 *
 * @param[in,out] core     Core instance.
 * @param[in]     agent_id Agent to cancel.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is already in a
 *         terminal state.
 *
 * Threading: not thread-safe.
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
 * @param[in,out] core Owner. @param[in] agent_id Terminal, notification acked.
 * @return OK; NULL_PTR; NOT_FOUND; INVALID_STATE if active/pending notification.
 * @note Owner thread. IDs are never reused; invalidates query views, not copied
 * event payloads. core_free requires remote/local owned work fenced/drained.
 */
cog_result_t cog_agent_release(cog_core_t *core, cog_agent_id_t agent_id);

#ifdef __cplusplus
}
#endif

#endif /* COG_AGENT_H */
