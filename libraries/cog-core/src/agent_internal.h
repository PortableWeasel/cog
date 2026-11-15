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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration — full definition in core.c. */
typedef struct cog_core cog_core_t;

/**
 * @brief Maximum live subagents per core.
 *
 * A full registry makes @c cog_agent_spawn return
 * @c COG_ERROR_UNAVAILABLE ("no core has capacity").  The registry is
 * append-only for the lifetime of the core (agent.h declares no destroy
 * API), so this bounds total agents per core.
 */
#define COG_AGENT_MAX_PER_CORE ((size_t)256)

/**
 * @brief One subagent record (definition of the internal registry entry).
 *
 * Owned by the registry; every string is an owned copy.  The child
 * session is owned by the record and released when the registry is
 * reset (cog_core_free).
 */
typedef struct cog_agent_entry {
    cog_agent_id_t id;              /**< Unique id; never zero. */
    cog_agent_status_t status;      /**< Current lifecycle status. */
    char *session_id;               /**< Owned copy of the child session id. */
    char *parent_session_id;        /**< Owned copy; NULL for no parent. */
    cog_session_t *session;         /**< Owned child session. */
    char *result;                   /**< Owned final result; NULL until completed. */
    size_t result_len;              /**< Byte length of @p result, excluding NUL. */
    char *status_msg;               /**< Owned last status message; may be NULL. */
    uint64_t group_id;              /**< Fan-out group; zero = none. */
    uint32_t timeout_ms;            /**< Time limit; zero = none. */
    bool force_local;               /**< Placement hint: run locally. */
    bool cancelled;                 /**< Cancellation requested. */
} cog_agent_entry_t;

/**
 * @brief Subagent registry embedded in @c struct cog_core.
 *
 * Zero-initialised by cog_core_new (calloc); grows lazily in agent.c.
 * Append-only for the lifetime of the core: agents are never removed
 * individually (agent.h declares no destroy API), so the registry is
 * released wholesale by cog_core_free.
 */
typedef struct cog_agent_registry {
    cog_agent_entry_t *entries;     /**< Owned dynamic array. */
    size_t count;                   /**< Live entries. */
    size_t capacity;                /**< Allocated entries. */
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

/**
 * @brief Release every agent record and the registry array.
 *
 * Frees each record's owned strings and child session, then the array
 * itself.  Called by cog_core_free.  NULL-safe.
 *
 * @param[in,out] reg Registry to reset.
 */
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
 * @return COG_ERROR_NOMEM if allocation fails (or @p result_len is
 *         SIZE_MAX, which would wrap the allocation size).
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
 * @warning The returned pointer points into the registry's dynamic
 *          array (@c cog_agent_registry_t::entries), which is
 *          reallocated when the registry grows.  It is therefore
 *          invalidated by any subsequent @c cog_agent_spawn that grows
 *          the registry (capacity boundaries 8 -> 16 -> ... -> 256), and
 *          by @c cog_core_free (which resets the registry and frees the
 *          array).  It is NOT valid until the core is freed.
 *
 * Safe usage window: the pointer is valid from the moment this function
 * returns until the next spawn that grows the registry or until the core
 * is freed, whichever comes first.  Calls that mutate an existing record
 * in place (cog_agent_cancel, cog_agent_status, cog_agent_notify_parent,
 * cog_agent_result, cog_agent_session_id, cog_agent_is_cancelled, and
 * the internal start/complete/fail/timeout hooks) do not reallocate the
 * array and do not invalidate the pointer.  Orchestrator consumers must
 * not hold the pointer across a spawn: re-fetch it after any spawn, and
 * never use it after cog_core_free.
 */
const cog_agent_entry_t *cog_agent_internal_get(cog_core_t *core,
                                                 cog_agent_id_t agent_id);

#endif /* COG_AGENT_INTERNAL_H */
