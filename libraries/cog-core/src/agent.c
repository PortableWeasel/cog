/**
 * @file agent.c
 * @brief Subagent lifecycle implementation for cog-core.
 *
 * Implements the public contract declared in @c cog/agent.h: spawn,
 * cancel, status, notify_parent, result, session_id, and is_cancelled,
 * plus the private orchestrator hooks declared in agent_internal.h
 * (start / complete / fail / timeout) and the registry lifecycle used
 * by core.c.
 *
 * Design notes (the full executable contract is recorded in
 * libraries/cog-core/contracts/agent/CONTRACT.md):
 *
 * - The subagent registry is embedded in @c struct cog_core (see
 *   agent_internal.h) and reached through @c cog_core_agent_registry().
 *   Q reserves records and copied strings at construction. N/O owns the
 *   declared release/reuse and remote-notification protocol migration.
 * - Spawn creates a child session (cog_session_new), links it to the
 *   parent via cog_session_set_parent, and records the agent as PENDING.
 *   The child session is owned by the agent record.
 * - Status transitions follow a strict state machine:
 *     PENDING --start--> RUNNING
 *     PENDING --cancel--> CANCELLED
 *     RUNNING --complete--> COMPLETED
 *     RUNNING --fail--> FAILED
 *     RUNNING --cancel--> CANCELLED
 *     RUNNING --timeout--> TIMEOUT
 *   Terminal states (COMPLETED, FAILED, CANCELLED, TIMEOUT) accept no
 *   further transitions; cancel on a terminal agent returns
 *   COG_ERROR_INVALID_STATE.
 * - cog_agent_cancel marks the agent cancelled (is_cancelled becomes
 *   true) and moves it to CANCELLED immediately: local semantics.  A
 *   late result delivered after cancellation is rejected with
 *   COG_ERROR_INVALID_STATE, so handlers can discard late fan-out
 *   results by checking cog_agent_is_cancelled first.
 * - cog_agent_notify_parent records the status message on the agent
 *   record.  Delivery of COG_EVENT_SUBAGENT_STATUS to the parent session
 *   is the orchestrator layer's job (Stage 12.5); in this phase the
 *   message is stored so the orchestrator can read it via
 *   cog_agent_internal_get.  This deliberately avoids enqueueing an
 *   event whose payload lifetime the core cannot track.
 * - The registry is bounded by core config.max_agents; full reservations
 *   return LIMIT without growth. Child sessions use an explicit constructor.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/agent.h"

#include "agent_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Return true if @p status is a terminal (locked) state. */
static bool agent_status_terminal(cog_agent_status_t status)
{
    return status == COG_AGENT_STATUS_COMPLETED ||
           status == COG_AGENT_STATUS_FAILED ||
           status == COG_AGENT_STATUS_CANCELLED ||
           status == COG_AGENT_STATUS_TIMEOUT;
}

/**
 * @brief Look up an agent record by id.
 *
 * @return Pointer to the record, or NULL if @p core is NULL or the id
 *         does not exist.
 */
static cog_agent_entry_t *agent_lookup(cog_core_t *core, cog_agent_id_t id)
{
    cog_agent_registry_t *reg;
    size_t i;

    if (core == NULL) {
        return NULL;
    }
    reg = cog_core_agent_registry(core);
    for (i = 0U; i < reg->count; ++i) {
        if (reg->entries[i].id == id) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Registry lifecycle (called by core.c)                               */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_registry_init(cog_agent_registry_t *reg, size_t capacity, size_t message_bytes)
{
    if (capacity == 0U || message_bytes == 0U || capacity > SIZE_MAX / sizeof(*reg->entries) ||
        capacity > SIZE_MAX / 5U || capacity * 5U > SIZE_MAX / message_bytes) { return COG_ERROR_INVALID_ARG; }
    reg->entries = calloc(capacity, sizeof(*reg->entries));
    reg->storage = malloc(capacity * 5U * message_bytes);
    if (reg->entries == NULL || reg->storage == NULL) {
        free(reg->entries); free(reg->storage); memset(reg, 0, sizeof(*reg)); return COG_ERROR_NOMEM;
    }
    reg->capacity = capacity; reg->message_bytes = message_bytes; return COG_OK;
}
void cog_agent_registry_reset(cog_agent_registry_t *reg)
{
    size_t i;
    if (reg == NULL) { return; }
    for (i = 0U; i < reg->count; ++i) { cog_session_free(reg->entries[i].session); }
    free(reg->entries); free(reg->storage); memset(reg, 0, sizeof(*reg));
}

/* ------------------------------------------------------------------ */
/* Spawn                                                               */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_spawn(cog_core_t *core,
                             const cog_agent_config_t *config,
                             cog_agent_id_t *out_id)
{
    cog_agent_registry_t *reg;
    cog_agent_entry_t *entry;
    cog_session_config_t scfg;
    cog_session_t *child;
    const char *sid;
    cog_result_t rc;

    if (core == NULL || config == NULL || out_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (config->task == NULL || config->task[0] == '\0') {
        return COG_ERROR_INVALID_ARG;
    }

    reg = cog_core_agent_registry(core);
    if (reg->count >= reg->capacity || reg->next_id == UINT64_MAX ||
        strnlen(config->task, reg->message_bytes) == reg->message_bytes ||
        (config->parent_session_id != NULL && strnlen(config->parent_session_id, reg->message_bytes) == reg->message_bytes)) {
        return COG_ERROR_LIMIT;
    }
    if (config->force_local && config->owner_peer != COG_PEER_ID_INVALID) { return COG_ERROR_INVALID_ARG; }
    if (config->max_status_bytes > reg->message_bytes || config->max_result_bytes > reg->message_bytes) { return COG_ERROR_LIMIT; }
    if (config->owner_peer != COG_PEER_ID_INVALID) { return COG_ERROR_UNSUPPORTED; }

    /* Create the child session and link it to the parent.  cog_session_new
     * generates a unique id when config->id is NULL, so sid is never NULL
     * after a successful creation. */
    memset(&scfg, 0, sizeof(scfg));
    scfg.id = NULL;      /* generate a unique id */
    scfg.max_nodes = 0U; /* default */
    child = NULL;
    rc = cog_session_new(&child, &scfg);
    if (rc != COG_OK) {
        return rc;
    }
    rc = cog_session_set_parent(child, config->parent_session_id);
    if (rc != COG_OK) {
        cog_session_free(child);
        return rc;
    }
    sid = cog_session_id(child);

    entry = &reg->entries[reg->count];
    memset(entry, 0, sizeof(*entry));
    entry->id = reg->next_id + 1U;
    reg->next_id = entry->id;
    entry->status = COG_AGENT_STATUS_PENDING;
    entry->session = child;
    entry->group_id = config->group_id;
    entry->timeout_ms = config->timeout_ms;
    entry->force_local = config->force_local;
    entry->status_capacity = config->max_status_bytes != 0U ? config->max_status_bytes : reg->message_bytes;
    entry->result_capacity = config->max_result_bytes != 0U ? config->max_result_bytes : reg->message_bytes;
    if (strlen(sid) >= reg->message_bytes) { cog_session_free(child); return COG_ERROR_LIMIT; }
    entry->session_id = reg->storage + reg->count * 5U * reg->message_bytes;
    entry->parent_session_id = entry->session_id + reg->message_bytes;
    entry->task = entry->parent_session_id + reg->message_bytes;
    entry->result = entry->task + reg->message_bytes;
    entry->status_msg = entry->result + reg->message_bytes;
    memcpy(entry->session_id, sid, strlen(sid) + 1U);
    if (config->parent_session_id != NULL) { memcpy(entry->parent_session_id, config->parent_session_id, strlen(config->parent_session_id) + 1U); }
    else { entry->parent_session_id = NULL; }
    memcpy(entry->task, config->task, strlen(config->task) + 1U);
    entry->result[0] = '\0'; entry->status_msg[0] = '\0';

    ++reg->count;
    *out_id = entry->id;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Cancel                                                              */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_cancel(cog_core_t *core, cog_agent_id_t agent_id)
{
    cog_agent_entry_t *entry;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (agent_status_terminal(entry->status)) {
        return COG_ERROR_INVALID_STATE;
    }
    entry->cancelled = true;
    entry->status = COG_AGENT_STATUS_CANCELLED;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_status(cog_core_t *core,
                              cog_agent_id_t agent_id,
                              cog_agent_status_t *out_status)
{
    cog_agent_entry_t *entry;

    if (core == NULL || out_status == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    *out_status = entry->status;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Notify parent                                                       */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_notify_parent(cog_core_t *core,
                                     cog_agent_id_t agent_id,
                                     const char *status_msg)
{
    cog_agent_entry_t *entry;

    if (core == NULL || status_msg == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    {
        cog_agent_registry_t *reg = cog_core_agent_registry(core);
        size_t length = strnlen(status_msg, entry->status_capacity);
        (void)reg;
        if (length == entry->status_capacity) { return COG_ERROR_LIMIT; }
        memcpy(entry->status_msg, status_msg, length + 1U);
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Result                                                              */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_result(cog_core_t *core,
                              cog_agent_id_t agent_id,
                              char *out_result,
                              size_t result_capacity,
                              size_t *out_result_len)
{
    cog_agent_entry_t *entry;

    if (core == NULL || out_result == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (entry->status != COG_AGENT_STATUS_COMPLETED) {
        return COG_ERROR_INVALID_STATE;
    }
    if (entry->result_len >= result_capacity) {
        if (out_result_len != NULL) {
            *out_result_len = entry->result_len; /* required, excluding NUL */
        }
        return COG_ERROR_OVERFLOW;
    }
    if (entry->result_len > 0U) {
        (void)memcpy(out_result, entry->result, entry->result_len);
    }
    out_result[entry->result_len] = '\0';
    if (out_result_len != NULL) {
        *out_result_len = entry->result_len;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Session id                                                          */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_session_id(cog_core_t *core,
                                  cog_agent_id_t agent_id,
                                  const char **out_session_id)
{
    cog_agent_entry_t *entry;

    if (core == NULL || out_session_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    *out_session_id = entry->session_id;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Is cancelled                                                        */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_is_cancelled(cog_core_t *core,
                                    cog_agent_id_t agent_id,
                                    bool *out_cancelled)
{
    cog_agent_entry_t *entry;

    if (core == NULL || out_cancelled == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    *out_cancelled = entry->cancelled;
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Internal status transitions (orchestrator hooks)                    */
/* ------------------------------------------------------------------ */

cog_result_t cog_agent_internal_start(cog_core_t *core, cog_agent_id_t agent_id)
{
    cog_agent_entry_t *entry;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (entry->status != COG_AGENT_STATUS_PENDING) {
        return COG_ERROR_INVALID_STATE;
    }
    entry->status = COG_AGENT_STATUS_RUNNING;
    return COG_OK;
}

cog_result_t cog_agent_internal_complete(cog_core_t *core,
                                         cog_agent_id_t agent_id,
                                         const char *result,
                                         size_t result_len)
{
    cog_agent_entry_t *entry;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (result == NULL && result_len > 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    /* result_len + 1 must not wrap: SIZE_MAX would make the allocation
     * size zero and then write past it.  Reject it as an unsatisfiable
     * allocation. */
    if (result_len == SIZE_MAX) {
        return COG_ERROR_LIMIT;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (entry->status != COG_AGENT_STATUS_RUNNING) {
        return COG_ERROR_INVALID_STATE;
    }
    if (result_len >= entry->result_capacity) { return COG_ERROR_LIMIT; }
    if (result_len > 0U) { memcpy(entry->result, result, result_len); }
    entry->result[result_len] = '\0';
    entry->result_len = result_len;
    entry->status = COG_AGENT_STATUS_COMPLETED;
    return COG_OK;
}

cog_result_t cog_agent_internal_fail(cog_core_t *core, cog_agent_id_t agent_id)
{
    cog_agent_entry_t *entry;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (entry->status != COG_AGENT_STATUS_RUNNING) {
        return COG_ERROR_INVALID_STATE;
    }
    entry->status = COG_AGENT_STATUS_FAILED;
    return COG_OK;
}

cog_result_t cog_agent_internal_timeout(cog_core_t *core, cog_agent_id_t agent_id)
{
    cog_agent_entry_t *entry;

    if (core == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) {
        return COG_ERROR_NOT_FOUND;
    }
    if (entry->status != COG_AGENT_STATUS_RUNNING) {
        return COG_ERROR_INVALID_STATE;
    }
    entry->status = COG_AGENT_STATUS_TIMEOUT;
    return COG_OK;
}

const cog_agent_entry_t *cog_agent_internal_get(cog_core_t *core,
                                                 cog_agent_id_t agent_id)
{
    return agent_lookup(core, agent_id);
}
