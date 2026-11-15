/**
 * @file agent_internal.h
 * @brief Private shared layout for cog-core/src/agent.c and core.c.
 *
 * This header is internal to cog-core.  It is not installed, not included
 * by any public header, and must not be used by consumers.  It exists to
 * give a single definition to the subagent registry layout (and the
 * per-agent record it contains) that is shared between agent.c and
 * core.c.
 *
 * Background: the subagent registry must live inside the core instance
 * (the project bans global mutable state), but @c cog_core_t is opaque
 * and agent.h declares no registry API.  core.c embeds a
 * @c cog_agent_registry_t in @c struct cog_core and exposes it through
 * @c cog_core_agent_registry(); agent.c owns every operation on it.
 *
 * The status-transition functions declared here are the private hooks the
 * orchestrator layer (Stage 12.5) will call to drive subagent execution
 * (start / complete / fail / timeout).  They are not part of the public
 * contract; the white-box unit test exercises them through this header.
 */
#ifndef COG_AGENT_INTERNAL_H
#define COG_AGENT_INTERNAL_H

#include "cog/agent.h"
#include "cog/event.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration — full definition in core.c. */
typedef struct cog_core cog_core_t;

/**
 * @brief Maximum live subagents per core.
 *
 * Legacy test fixture ceiling, retained for source compatibility. Runtime
 * reservations now come from core config.max_agents (default 16); a full
 * reservation returns LIMIT.
 */
#define COG_AGENT_MAX_PER_CORE ((size_t)256)

/**
 * @brief One subagent record (definition of the internal registry entry).
 *
 * Owned by the registry; copied strings point into fixed registry storage. The child
 * session is owned by the record and released when the registry is
 * reset (cog_core_free).
 */
typedef struct cog_agent_entry {
    cog_agent_id_t id;              /**< Unique id; never zero. */
    cog_agent_status_t status;      /**< Current lifecycle status. */
    char *session_id;               /**< Owned copy of the child session id. */
    char *parent_session_id;        /**< Owned copy; NULL for no parent. */
    cog_session_t *session;         /**< Owned child session. */
    char *task;                   /**< Reserved copied task. */
    char *result;                   /**< Reserved final result bytes. */
    size_t status_capacity, result_capacity; /**< Per-agent configured ceilings. */
    size_t result_len;              /**< Byte length of @p result, excluding NUL. */
    char *status_msg;               /**< Reserved last status message. */
    uint64_t group_id;              /**< Fan-out group; zero = none. */
    uint32_t timeout_ms;            /**< Time limit; zero = none. */
    bool force_local;               /**< Placement hint: run locally. */
    bool cancelled;                 /**< Cancellation requested. */
    cog_peer_id_t owner_peer;
    uint64_t execution_epoch, sequence;
    cog_result_t outcome;
    size_t update_len;
    bool has_update, notification_pending;
    cog_event_agent_intent_t spawn_intent, cancel_intent;
    size_t intent_leases;
} cog_agent_entry_t;

/**
 * @brief Subagent registry embedded in @c struct cog_core.
 *
 * Constructor-reserved by cog_core_new; never grows during agent operations.
 * Slots are reused only after terminal release; IDs remain monotonic.
 */
typedef struct cog_agent_registry {
    cog_agent_entry_t *entries;     /**< Owned dynamic array. */
    size_t count;                   /**< Live entries. */
    size_t capacity;                /**< Allocated entries. */
    size_t message_bytes;          /**< Reserved bytes per copied field. */
    char *storage;                 /**< Fixed five-field strings per record. */
    uint64_t next_id;               /**< Next id to assign (starts at 1). */
} cog_agent_registry_t;

/* ------------------------------------------------------------------ */
/* Core integration (implemented by core.c)                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Return the subagent registry embedded in a core.
 *
 * @param[in] core Core to query.
 * @return Pointer to the core's registry, or NULL if @p core is NULL.
 */
cog_agent_registry_t *cog_core_agent_registry(cog_core_t *core);
cog_result_t cog_core_enqueue_agent_intent(cog_core_t *core, const cog_event_t *event);
void cog_agent_intent_dispatched(cog_core_t *core, const cog_event_t *event);

/**
 * @brief Release every agent record and the registry array.
 *
 * Frees each child session, the reserved string storage, and the array
 * itself.  Called by cog_core_free.  NULL-safe.
 *
 * @param[in,out] reg Registry to reset.
 */
cog_result_t cog_agent_registry_init(cog_agent_registry_t *reg, size_t capacity, size_t message_bytes);
void cog_agent_registry_reset(cog_agent_registry_t *reg);

/* ------------------------------------------------------------------ */
/* Status transitions (orchestrator hooks; not public API)             */
/* ------------------------------------------------------------------ */

/**
 * @brief Move an agent from PENDING to RUNNING.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is not PENDING.
 */
cog_result_t cog_agent_internal_start(cog_core_t *core, cog_agent_id_t agent_id);

/**
 * @brief Move a RUNNING agent to COMPLETED and record its result.
 *
 * @p result may be NULL only when @p result_len is zero (empty result).
 * The result is copied; the caller retains ownership.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_INVALID_ARG if @p result is NULL with a non-zero
 *         @p result_len.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is not RUNNING (including
 *         cancelled agents: late results are rejected).
 * @return COG_ERROR_LIMIT if the reserved result cannot fit.
 */
cog_result_t cog_agent_internal_complete(cog_core_t *core, cog_agent_id_t agent_id,
                                         const char *result, size_t result_len);

/**
 * @brief Move a RUNNING agent to FAILED.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is not RUNNING.
 */
cog_result_t cog_agent_internal_fail(cog_core_t *core, cog_agent_id_t agent_id);

/**
 * @brief Move a RUNNING agent to TIMEOUT.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core is NULL.
 * @return COG_ERROR_NOT_FOUND if @p agent_id does not exist.
 * @return COG_ERROR_INVALID_STATE if the agent is not RUNNING.
 */
cog_result_t cog_agent_internal_timeout(cog_core_t *core, cog_agent_id_t agent_id);

/**
 * @brief Return the internal record for an agent (read-only).
 *
 * @param[in] core Core to query.
 * @param[in] agent_id Agent id.
 * @return Pointer to the record, or NULL if @p core is NULL or the id
 *         does not exist.
 *
 * @warning Returned views remain valid until release/core_free. A record's
 *          mutable contents may change at the next owner-thread operation.
 */
const cog_agent_entry_t *cog_agent_internal_get(cog_core_t *core,
                                                 cog_agent_id_t agent_id);

#endif /* COG_AGENT_INTERNAL_H */
