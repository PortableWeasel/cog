/**
 * @file agent.c
 * @brief Bounded subagent records, attempt updates and lifecycle snapshots.
 *
 * Records and copied bytes are reserved by the core constructor. Explicit
 * child-session construction is the spawn allocation boundary. Transport
 * authentication and orchestration remain application responsibilities.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog/agent.h"

#include "agent_internal.h"
#include "session_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Return true if @p status is a terminal (locked) state. */
static bool agent_utf8(const char *text, size_t length);

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

    if (core == NULL || id == 0U) {
        return NULL;
    }
    reg = cog_core_agent_registry(core);
    for (i = 0U; i < reg->capacity; ++i) {
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
    for (i = 0U; i < reg->capacity; ++i) {
        assert(!reg->entries[i].execution_borrowed);
        cog_session_free(reg->entries[i].session);
    }
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
    cog_event_t intent_event;
    size_t slot;

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
    if (!agent_utf8(config->task, strlen(config->task)) ||
        (config->parent_session_id != NULL && !agent_utf8(config->parent_session_id, strlen(config->parent_session_id)))) { return COG_ERROR_INVALID_ARG; }
    if (config->force_local && config->owner_peer != COG_PEER_ID_INVALID) { return COG_ERROR_INVALID_ARG; }
    if (config->max_status_bytes > reg->message_bytes || config->max_result_bytes > reg->message_bytes) { return COG_ERROR_LIMIT; }
    if (config->owner_peer != COG_PEER_ID_INVALID && config->execution_epoch == 0U) { return COG_ERROR_INVALID_ARG; }
    for (slot = 0U; slot < reg->capacity && reg->entries[slot].id != 0U; ++slot) {}

    /* Create the child session and link it to the parent.  cog_session_new
     * generates a unique id when config->id is NULL, so sid is never NULL
     * after a successful creation. */
    memset(&scfg, 0, sizeof(scfg));
    if (config->session_config != NULL) {
        if (config->session_config->id != NULL) return COG_ERROR_INVALID_ARG;
        scfg = *config->session_config;
    }
    scfg.id = NULL; /* registry always owns a fresh identity */
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

    if (strlen(sid) >= reg->message_bytes) { cog_session_free(child); return COG_ERROR_LIMIT; }
    entry = &reg->entries[slot];
    memset(entry, 0, sizeof(*entry));
    entry->id = reg->next_id + 1U;
    entry->owner_peer = config->owner_peer;
    entry->execution_epoch = config->execution_epoch != 0U ? config->execution_epoch : entry->id;
    entry->status = COG_AGENT_STATUS_PENDING;
    entry->session = child;
    entry->group_id = config->group_id;
    entry->timeout_ms = config->timeout_ms;
    entry->force_local = config->force_local;
    entry->status_capacity = config->max_status_bytes != 0U ? config->max_status_bytes : reg->message_bytes;
    entry->result_capacity = config->max_result_bytes != 0U ? config->max_result_bytes : reg->message_bytes;
    entry->session_id = reg->storage + slot * 5U * reg->message_bytes;
    entry->parent_session_id = entry->session_id + reg->message_bytes;
    entry->task = entry->parent_session_id + reg->message_bytes;
    entry->result = entry->task + reg->message_bytes;
    entry->status_msg = entry->result + reg->message_bytes;
    memcpy(entry->session_id, sid, strlen(sid) + 1U);
    if (config->parent_session_id != NULL) { memcpy(entry->parent_session_id, config->parent_session_id, strlen(config->parent_session_id) + 1U); }
    else { entry->parent_session_id = NULL; }
    memcpy(entry->task, config->task, strlen(config->task) + 1U);
    entry->result[0] = '\0'; entry->status_msg[0] = '\0';
    entry->spawn_intent = (cog_event_agent_intent_t){entry->id, entry->execution_epoch,
                                                    entry->owner_peer, COG_EVENT_SUBAGENT_SPAWN};
    intent_event = (cog_event_t){(cog_event_id_t)COG_EVENT_SUBAGENT_SPAWN,
                                 COG_EVENT_SUBAGENT_SPAWN, &entry->spawn_intent,
                                 sizeof(entry->spawn_intent), entry->id};
    rc = cog_core_enqueue_agent_intent(core, &intent_event);
    if (rc != COG_OK) {
        cog_session_free(child);
        memset(entry, 0, sizeof(*entry));
        return rc;
    }
    entry->intent_leases = 1U;
    ++reg->count;
    reg->next_id = entry->id;
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
    if (entry->cancelled) return COG_OK;
    {
        cog_event_t event;
        cog_result_t rc;
        entry->cancel_intent = (cog_event_agent_intent_t){entry->id, entry->execution_epoch,
                                                         entry->owner_peer, COG_EVENT_SUBAGENT_CANCEL};
        event = (cog_event_t){(cog_event_id_t)COG_EVENT_SUBAGENT_CANCEL,
                              COG_EVENT_SUBAGENT_CANCEL, &entry->cancel_intent,
                              sizeof(entry->cancel_intent), entry->id};
        rc = cog_core_enqueue_agent_intent(core, &event);
        if (rc != COG_OK) return rc;
        ++entry->intent_leases;
    }
    entry->cancelled = true;
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
        cog_agent_update_t update;
        size_t length;
        if (entry->owner_peer != COG_PEER_ID_INVALID) return COG_ERROR_INVALID_STATE;
        length = strnlen(status_msg, entry->status_capacity);
        if (length == entry->status_capacity) return COG_ERROR_LIMIT;
        if (entry->sequence == UINT64_MAX) return COG_ERROR_LIMIT;
        update = (cog_agent_update_t){entry->owner_peer, entry->execution_epoch,
                                     entry->sequence + 1U, COG_AGENT_STATUS_RUNNING,
                                     COG_OK, status_msg, length};
        return cog_agent_apply_update(core, agent_id, &update);
    }
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
    if (!agent_status_terminal(entry->status)) {
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

static cog_result_t agent_internal_update(cog_core_t *core, cog_agent_id_t agent_id,
                                          cog_agent_status_t status, cog_result_t outcome,
                                          const char *data, size_t len)
{
    cog_agent_entry_t *entry;
    cog_agent_update_t update;
    if (core == NULL) return COG_ERROR_NULL_PTR;
    if (data == NULL && len != 0U) return COG_ERROR_INVALID_ARG;
    if (len == SIZE_MAX) return COG_ERROR_LIMIT;
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if ((status == COG_AGENT_STATUS_RUNNING && entry->status != COG_AGENT_STATUS_PENDING) ||
        (status != COG_AGENT_STATUS_RUNNING && entry->status != COG_AGENT_STATUS_RUNNING))
        return COG_ERROR_INVALID_STATE;
    if (entry->sequence == UINT64_MAX) return COG_ERROR_LIMIT;
    update = (cog_agent_update_t){entry->owner_peer, entry->execution_epoch,
                                  entry->sequence + 1U, status, outcome, data, len};
    return cog_agent_apply_update(core, agent_id, &update);
}
cog_result_t cog_agent_internal_start(cog_core_t *core, cog_agent_id_t agent_id)
{
    return agent_internal_update(core, agent_id, COG_AGENT_STATUS_RUNNING, COG_OK, NULL, 0U);
}
cog_result_t cog_agent_internal_complete(cog_core_t *core, cog_agent_id_t agent_id,
                                          const char *result, size_t result_len)
{
    return agent_internal_update(core, agent_id, COG_AGENT_STATUS_COMPLETED, COG_OK, result, result_len);
}
cog_result_t cog_agent_internal_fail(cog_core_t *core, cog_agent_id_t agent_id)
{
    return agent_internal_update(core, agent_id, COG_AGENT_STATUS_FAILED, COG_ERROR_IO, NULL, 0U);
}
cog_result_t cog_agent_internal_timeout(cog_core_t *core, cog_agent_id_t agent_id)
{
    return agent_internal_update(core, agent_id, COG_AGENT_STATUS_TIMEOUT, COG_ERROR_TIMEOUT, NULL, 0U);
}

const cog_agent_entry_t *cog_agent_internal_get(cog_core_t *core,
                                                 cog_agent_id_t agent_id)
{
    return agent_lookup(core, agent_id);
}

/* Snapshot data is kept in one of the two constructor-reserved fields. */
static const char *agent_snapshot_data(const cog_agent_entry_t *entry)
{
    return agent_status_terminal(entry->status) ? entry->result : entry->status_msg;
}

static bool agent_utf8(const char *text, size_t length)
{
    size_t i = 0U;
    if (text == NULL) return length == 0U;
    while (i < length) {
        unsigned char first = (unsigned char)text[i++];
        uint32_t code, minimum;
        size_t rest;
        if (first == 0U) return false;
        if (first < 0x80U) continue;
        if (first >= 0xc2U && first <= 0xdfU) { rest = 1U; code = first & 0x1fU; minimum = 0x80U; }
        else if (first >= 0xe0U && first <= 0xefU) { rest = 2U; code = first & 0x0fU; minimum = 0x800U; }
        else if (first >= 0xf0U && first <= 0xf4U) { rest = 3U; code = first & 0x07U; minimum = 0x10000U; }
        else return false;
        if (rest > length - i) return false;
        for (size_t k = 0U; k < rest; ++k) {
            unsigned char c = (unsigned char)text[i++];
            if ((c & 0xc0U) != 0x80U) return false;
            code = (code << 6U) | (c & 0x3fU);
        }
        if (code < minimum || code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU)) return false;
    }
    return true;
}

cog_result_t cog_agent_apply_update(cog_core_t *core, cog_agent_id_t agent_id,
                                    const cog_agent_update_t *update)
{
    cog_agent_entry_t *entry;
    size_t capacity;
    char *destination;
    if (core == NULL || update == NULL) return COG_ERROR_NULL_PTR;
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (update->owner_peer != entry->owner_peer || update->execution_epoch != entry->execution_epoch)
        return COG_ERROR_INVALID_STATE;
    if (update->status < COG_AGENT_STATUS_RUNNING || update->status > COG_AGENT_STATUS_TIMEOUT ||
        ((update->status == COG_AGENT_STATUS_RUNNING || update->status == COG_AGENT_STATUS_COMPLETED)
         ? update->outcome != COG_OK : (update->outcome >= COG_OK || update->outcome < COG_ERROR_INDETERMINATE)))
        return COG_ERROR_PROTOCOL;
    capacity = agent_status_terminal(update->status) ? entry->result_capacity : entry->status_capacity;
    if (update->data_len >= capacity) return COG_ERROR_LIMIT;
    if (!agent_utf8(update->data, update->data_len)) return COG_ERROR_PROTOCOL;
    if (entry->has_update && update->sequence == entry->sequence) {
        if (entry->status == update->status && entry->outcome == update->outcome &&
            entry->update_len == update->data_len &&
            (update->data_len == 0U || memcmp(agent_snapshot_data(entry), update->data, update->data_len) == 0)) return COG_OK;
        return COG_ERROR_PROTOCOL;
    }
    if (entry->sequence == UINT64_MAX || update->sequence != entry->sequence + 1U ||
        agent_status_terminal(entry->status) ||
        (entry->cancelled && (update->status == COG_AGENT_STATUS_RUNNING || update->status == COG_AGENT_STATUS_COMPLETED)))
        return COG_ERROR_INVALID_STATE;
    if (entry->notification_pending) return COG_ERROR_AGAIN;
    destination = agent_status_terminal(update->status) ? entry->result : entry->status_msg;
    if (update->data_len != 0U) memmove(destination, update->data, update->data_len);
    destination[update->data_len] = '\0';
    entry->status = update->status;
    if (update->status == COG_AGENT_STATUS_CANCELLED) entry->cancelled = true;
    entry->outcome = update->outcome;
    entry->sequence = update->sequence;
    entry->update_len = update->data_len;
    if (agent_status_terminal(update->status)) entry->result_len = update->data_len;
    entry->has_update = true;
    entry->notification_pending = true;
    return COG_OK;
}

cog_result_t cog_agent_copy_update(const cog_core_t *core, cog_agent_id_t agent_id,
                                   cog_agent_update_t *update, char *storage,
                                   size_t capacity, size_t *required)
{
    cog_agent_entry_t *entry;
    if (core == NULL || update == NULL || required == NULL || (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    entry = agent_lookup((cog_core_t *)core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (!entry->has_update) return COG_ERROR_AGAIN;
    if (capacity <= entry->update_len) { *required = entry->update_len + 1U; return COG_ERROR_OVERFLOW; }
    memcpy(storage, agent_snapshot_data(entry), entry->update_len + 1U);
    *update = (cog_agent_update_t){entry->owner_peer, entry->execution_epoch, entry->sequence,
                                  entry->status, entry->outcome, storage, entry->update_len};
    *required = entry->update_len + 1U;
    return COG_OK;
}

cog_result_t cog_agent_ack_update(cog_core_t *core, cog_agent_id_t agent_id,
                                  uint64_t execution_epoch, uint64_t sequence)
{
    cog_agent_entry_t *entry;
    if (core == NULL) return COG_ERROR_NULL_PTR;
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (!entry->has_update || entry->execution_epoch != execution_epoch || entry->sequence != sequence)
        return COG_ERROR_INVALID_STATE;
    entry->notification_pending = false;
    return COG_OK;
}

cog_result_t cog_agent_session_borrow(cog_core_t *core, cog_agent_id_t agent_id,
                                       cog_session_t **session)
{
    cog_agent_entry_t *entry;
    if (core == NULL || session == NULL) return COG_ERROR_NULL_PTR;
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (entry->owner_peer != COG_PEER_ID_INVALID || agent_status_terminal(entry->status) ||
        entry->execution_borrowed) return COG_ERROR_INVALID_STATE;
    if (entry->cancelled) return COG_ERROR_ABORTED;
    entry->execution_borrowed = true;
    *session = entry->session;
    return COG_OK;
}
cog_result_t cog_agent_session_return(cog_core_t *core, cog_agent_id_t agent_id,
                                       cog_session_t *session)
{
    cog_agent_entry_t *entry;
    if (core == NULL || session == NULL) return COG_ERROR_NULL_PTR;
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (!entry->execution_borrowed || entry->session != session ||
        ((struct cog_session *)session)->attached) return COG_ERROR_INVALID_STATE;
    entry->execution_borrowed = false;
    return COG_OK;
}

cog_result_t cog_agent_release(cog_core_t *core, cog_agent_id_t agent_id)
{
    cog_agent_entry_t *entry;
    cog_agent_registry_t *reg;
    if (core == NULL) return COG_ERROR_NULL_PTR;
    entry = agent_lookup(core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    if (!agent_status_terminal(entry->status) || entry->notification_pending || entry->intent_leases != 0U || entry->execution_borrowed) return COG_ERROR_INVALID_STATE;
    cog_session_free(entry->session);
    memset(entry, 0, sizeof(*entry));
    reg = cog_core_agent_registry(core);
    --reg->count;
    return COG_OK;
}

static void agent_put_u64(unsigned char *p, uint64_t v)
{
    for (size_t i = 0U; i < 8U; ++i) p[i] = (unsigned char)(v >> (56U - 8U * i));
}
static uint64_t agent_get_u64(const unsigned char *p)
{
    uint64_t v = 0U;
    for (size_t i = 0U; i < 8U; ++i) v = (v << 8U) | p[i];
    return v;
}
static void agent_put_u32(unsigned char *p, uint32_t v)
{
    for (size_t i = 0U; i < 4U; ++i) p[i] = (unsigned char)(v >> (24U - 8U * i));
}
static uint32_t agent_get_u32(const unsigned char *p)
{
    uint32_t v = 0U;
    for (size_t i = 0U; i < 4U; ++i) v = (v << 8U) | p[i];
    return v;
}
#define AGENT_WIRE_HEADER 45U
cog_result_t cog_agent_update_encode(cog_agent_id_t agent_id, const cog_agent_update_t *update,
                                      void *output, size_t capacity, size_t *required)
{
    unsigned char *p = output;
    cog_agent_update_t snapshot;
    size_t need;
    if (update == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    snapshot = *update;
    update = &snapshot;
    if (update->data_len > UINT32_MAX || update->data_len > SIZE_MAX - AGENT_WIRE_HEADER) return COG_ERROR_LIMIT;
    if (agent_id == 0U || update->status < COG_AGENT_STATUS_PENDING || update->status > COG_AGENT_STATUS_TIMEOUT ||
        update->outcome > COG_OK || update->outcome < COG_ERROR_INDETERMINATE ||
        !agent_utf8(update->data, update->data_len)) return COG_ERROR_INVALID_ARG;
    need = AGENT_WIRE_HEADER + update->data_len;
    if (capacity < need) { *required = need; return COG_ERROR_OVERFLOW; }
    if (update->data_len != 0U) memmove(p + AGENT_WIRE_HEADER, update->data, update->data_len);
    memcpy(p, "CGA1", 4U);
    agent_put_u64(p + 4U, agent_id);
    agent_put_u64(p + 12U, update->owner_peer);
    agent_put_u64(p + 20U, update->execution_epoch);
    agent_put_u64(p + 28U, update->sequence);
    p[36] = (unsigned char)update->status;
    agent_put_u32(p + 37U, (uint32_t)(int32_t)update->outcome);
    agent_put_u32(p + 41U, (uint32_t)update->data_len);
    *required = need;
    return COG_OK;
}
cog_result_t cog_agent_update_decode(const void *input, size_t input_len, cog_agent_id_t *agent_id,
                                      cog_agent_update_t *update, char *storage, size_t capacity,
                                      size_t *required)
{
    const unsigned char *p = input;
    uint32_t raw, length;
    int64_t outcome;
    cog_agent_update_t decoded;
    uint64_t id;
    if (input == NULL || agent_id == NULL || update == NULL || required == NULL ||
        (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (input_len < 4U) return COG_ERROR_PROTOCOL;
    if (memcmp(p, "CGA", 3U) != 0) return COG_ERROR_PROTOCOL;
    if (p[3] != (unsigned char)'1') return COG_ERROR_UNSUPPORTED;
    if (input_len < AGENT_WIRE_HEADER) return COG_ERROR_PROTOCOL;
    id = agent_get_u64(p + 4U);
    raw = agent_get_u32(p + 37U);
    outcome = raw <= INT32_MAX ? (int64_t)raw : (int64_t)raw - INT64_C(4294967296);
    length = agent_get_u32(p + 41U);
    if (id == 0U || p[36] > (unsigned char)COG_AGENT_STATUS_TIMEOUT || outcome > COG_OK ||
        outcome < COG_ERROR_INDETERMINATE || (size_t)length != input_len - AGENT_WIRE_HEADER ||
        !agent_utf8((const char *)p + AGENT_WIRE_HEADER, (size_t)length)) return COG_ERROR_PROTOCOL;
    if (capacity <= (size_t)length) { *required = (size_t)length + 1U; return COG_ERROR_OVERFLOW; }
    decoded = (cog_agent_update_t){agent_get_u64(p + 12U), agent_get_u64(p + 20U),
                                  agent_get_u64(p + 28U), (cog_agent_status_t)p[36],
                                  (cog_result_t)outcome, storage, (size_t)length};
    memmove(storage, p + AGENT_WIRE_HEADER, (size_t)length);
    storage[length] = '\0';
    *agent_id = id;
    *update = decoded;
    *required = (size_t)length + 1U;
    return COG_OK;
}

void cog_agent_intent_dispatched(cog_core_t *core, const cog_event_t *event)
{
    cog_agent_registry_t *reg = cog_core_agent_registry(core);
    if (event->kind != COG_EVENT_SUBAGENT_SPAWN && event->kind != COG_EVENT_SUBAGENT_CANCEL) return;
    for (size_t i = 0U; i < reg->capacity; ++i) {
        cog_agent_entry_t *entry = &reg->entries[i];
        if (entry->id != 0U && (event->payload == &entry->spawn_intent || event->payload == &entry->cancel_intent)) {
            assert(entry->intent_leases != 0U);
            --entry->intent_leases;
            return;
        }
    }
    assert(false && "unknown agent intent lease");
}

cog_result_t cog_agent_copy_config(const cog_core_t *core, cog_agent_id_t agent_id,
                                   cog_agent_config_t *out_config, char *storage,
                                   size_t capacity, size_t *out_required)
{
    cog_agent_entry_t *entry;
    cog_agent_config_t copy;
    size_t task_bytes, parent_bytes, needed;
    if (core == NULL || out_config == NULL || out_required == NULL ||
        (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    entry = agent_lookup((cog_core_t *)core, agent_id);
    if (entry == NULL) return COG_ERROR_NOT_FOUND;
    task_bytes = strlen(entry->task) + 1U;
    parent_bytes = entry->parent_session_id == NULL ? 0U : strlen(entry->parent_session_id) + 1U;
    needed = task_bytes + parent_bytes;
    if (capacity < needed) { *out_required = needed; return COG_ERROR_OVERFLOW; }
    memcpy(storage, entry->task, task_bytes);
    if (parent_bytes != 0U) memcpy(storage + task_bytes, entry->parent_session_id, parent_bytes);
    copy = (cog_agent_config_t){storage, parent_bytes != 0U ? storage + task_bytes : NULL,
                                entry->timeout_ms, entry->force_local, entry->group_id,
                                entry->owner_peer, entry->execution_epoch,
                                entry->status_capacity, entry->result_capacity, NULL};
    *out_config = copy;
    *out_required = needed;
    return COG_OK;
}
