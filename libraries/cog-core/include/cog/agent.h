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
} cog_agent_config_t;

/**
 * @brief Spawn a subagent.
 *
 * Creates a child session, dispatches the task to an appropriate core
 * (local or remote), and returns the agent id for tracking.
 *
 * @param[in,out] core   Core instance that owns the parent session.
 * @param[in]     config Subagent configuration; must be non-NULL.
 * @param[out]    out_id Receives the new agent id on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p config, or @p out_id is NULL.
 * @return COG_ERROR_INVALID_ARG if @p config->task is NULL or empty.
 * @return COG_ERROR_NOMEM if allocation fails.
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
 * @brief Send a status update to the parent session.
 *
 * Called by a subagent to report progress.  The parent receives a
 * COG_EVENT_SUBAGENT_STATUS event with the status message.
 *
 * @param[in,out] core       Core instance (the subagent's core).
 * @param[in]     agent_id   This subagent's id.
 * @param[in]     status_msg Human-readable status message; copied.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core or @p status_msg is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
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
 * @return COG_ERROR_INVALID_STATE if the agent has not completed.
 * @return COG_ERROR_OVERFLOW if the result does not fit.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_agent_result(cog_core_t *core,
                               cog_agent_id_t agent_id,
                               char *out_result,
                               size_t result_capacity,
                               size_t *out_result_len);

#ifdef __cplusplus
}
#endif

#endif /* COG_AGENT_H */