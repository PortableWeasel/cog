/** @file toolkit_execution.c @brief Policy-gated, retained asynchronous execution. */
#include <cog/toolkit_exec.h>
#include <cog/core.h>
#include <cog/session.h>
#include <cog/session_store.h>
#include "toolkit_json.h"
#include <stdlib.h>
#include <string.h>

struct cog_toolkit_execution {
    cog_core_t *core;
    const cog_tool_async_vtable_t *ops;
    void *handle;
    cog_store_owner_guard_t *guard;
    cog_toolkit_json_t json;
    char *id;
    char *name;
    uint64_t now;
    bool clock_set;
    bool admitted;
    bool started;
    bool done;
    cog_tool_output_t output;
};

static cog_result_t safe_session(cog_core_t *core)
{
    cog_session_t *session = cog_core_session(core);
    if (session != NULL) {
        cog_session_snapshot_t snapshot;
        cog_result_t r = cog_session_snapshot_view(session, &snapshot);
        if (r != COG_OK) { return r; }
        if (snapshot.inspection_only) { return COG_ERROR_INVALID_STATE; }
        for (size_t i = 0U; i < snapshot.ledger_count; i++) {
            if (snapshot.ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE) {
                return COG_ERROR_INVALID_STATE;
            }
        }
    }
    return COG_OK;
}

static cog_result_t update_clock(cog_toolkit_execution_t *execution, uint64_t now)
{
    if (execution->clock_set && now < execution->now) { return COG_ERROR_INVALID_ARG; }
    execution->now = now;
    execution->clock_set = true;
    return COG_OK;
}

cog_result_t cog_toolkit_execution_new(cog_toolkit_execution_t **out,
    cog_core_t *core, const cog_tool_table_t *table, const char *call_id,
    const char *name, const char *arguments, size_t arguments_len,
    const cog_tool_async_config_t *config)
{
    const cog_tool_t *tool;
    cog_toolkit_execution_t *execution;
    cog_result_t r;
    if (out == NULL) { return COG_ERROR_NULL_PTR; }
    *out = NULL;
    if (core == NULL || table == NULL || call_id == NULL || name == NULL ||
        arguments == NULL || config == NULL) { return COG_ERROR_NULL_PTR; }
    if (*call_id == '\0' || *name == '\0' || config->max_output_bytes == 0U ||
        config->max_output_bytes == SIZE_MAX || config->timeout_ms == 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    r = cog_tool_table_lookup(table, name, &tool);
    if (r != COG_OK) { return r; }
    const cog_tool_async_vtable_t *ops = tool->async;
    if (ops == NULL) { return COG_ERROR_UNSUPPORTED; }
    if (ops->new_ == NULL || ops->start == NULL || ops->poll == NULL ||
        ops->cancel == NULL || ops->result == NULL || ops->destroy == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    r = safe_session(core);
    if (r != COG_OK) { return r; }
    bool require_guard = cog_store_requires_owner(cog_core_store(core));
    if (require_guard && config->owner_guard == NULL) { return COG_ERROR_INVALID_STATE; }
    if (config->owner_guard != NULL) {
        cog_session_t *session = cog_core_session(core);
        if (session == NULL) { return COG_ERROR_INVALID_STATE; }
        r = cog_store_owner_guard_borrow(config->owner_guard, cog_session_id(session));
        if (r != COG_OK) { return r; }
    }
    size_t id_len = strlen(call_id);
    size_t name_len = strlen(name);
    if (id_len > SIZE_MAX - sizeof(*execution) - 2U ||
        name_len > SIZE_MAX - sizeof(*execution) - id_len - 2U) {
        if (config->owner_guard != NULL) { (void)cog_store_owner_guard_return(config->owner_guard); }
        return COG_ERROR_LIMIT;
    }
    execution = calloc(1U, sizeof(*execution) + id_len + name_len + 2U);
    if (execution == NULL) {
        if (config->owner_guard != NULL) { (void)cog_store_owner_guard_return(config->owner_guard); }
        return COG_ERROR_NOMEM;
    }
    execution->core = core;
    execution->ops = ops;
    execution->guard = config->owner_guard;
    execution->id = (char *)(execution + 1);
    execution->name = execution->id + id_len + 1U;
    memcpy(execution->id, call_id, id_len + 1U);
    memcpy(execution->name, name, name_len + 1U);
    execution->output.output = "";
    r = cog_toolkit_json_new(&execution->json, arguments, arguments_len, tool->input_schema, config);
    if (r == COG_OK) { r = ops->new_(core, arguments, arguments_len, config, &execution->handle); }
    if (r == COG_OK && execution->handle == NULL) { r = COG_ERROR_INVALID_STATE; }
    if (r != COG_OK) {
        if (execution->handle != NULL) { (void)ops->destroy(execution->handle); }
        cog_toolkit_json_free(&execution->json);
        if (execution->guard != NULL) { (void)cog_store_owner_guard_return(execution->guard); }
        free(execution);
        return r;
    }
    *out = execution;
    return COG_OK;
}

cog_result_t cog_toolkit_execution_start(cog_toolkit_execution_t *execution,
    cog_toolkit_policy_decision_t decision, uint64_t now_ms)
{
    if (execution == NULL) { return COG_ERROR_NULL_PTR; }
    if (decision != COG_TOOLKIT_POLICY_UNDECIDED && decision != COG_TOOLKIT_POLICY_DENY &&
        decision != COG_TOOLKIT_POLICY_ALLOW) { return COG_ERROR_INVALID_ARG; }
    if (execution->admitted || execution->done) { return COG_ERROR_INVALID_STATE; }
    cog_result_t r = update_clock(execution, now_ms);
    if (r != COG_OK) { return r; }
    if (decision == COG_TOOLKIT_POLICY_UNDECIDED) { return COG_ERROR_AGAIN; }
    if (decision == COG_TOOLKIT_POLICY_DENY) {
        execution->admitted = true;
        execution->done = true;
        execution->output.status = COG_ERROR_DENIED;
        return COG_OK;
    }
    r = safe_session(execution->core);
    if (r != COG_OK) { return r; }
    if (execution->guard != NULL) {
        r = cog_store_owner_guard_borrow(execution->guard,
            cog_session_id(cog_core_session(execution->core)));
        if (r != COG_OK) { return r; }
        r = cog_store_owner_guard_return(execution->guard);
        if (r != COG_OK) { return r; }
    } else if (cog_store_requires_owner(cog_core_store(execution->core))) {
        return COG_ERROR_INVALID_STATE;
    }
    execution->admitted = true;
    r = execution->ops->start(execution->handle, now_ms);
    if (r == COG_OK) { execution->started = true; }
    else { execution->done = true; execution->output.status = r; }
    return r;
}

cog_result_t cog_toolkit_execution_poll(cog_toolkit_execution_t *execution,
    uint64_t now_ms, bool *out_done, uint64_t *out_next_deadline_ms)
{
    if (execution == NULL || out_done == NULL || out_next_deadline_ms == NULL) { return COG_ERROR_NULL_PTR; }
    if (!execution->admitted) { return COG_ERROR_INVALID_STATE; }
    cog_result_t r = update_clock(execution, now_ms);
    if (r != COG_OK) { return r; }
    if (!execution->done) {
        bool done = false;
        r = execution->ops->poll(execution->handle, now_ms, &done, out_next_deadline_ms);
        if (r != COG_OK) { return r; }
        if (done) {
            r = execution->ops->result(execution->handle, &execution->output);
            if (r != COG_OK) { return r; }
            execution->done = true;
        }
    }
    *out_done = execution->done;
    if (execution->done) { *out_next_deadline_ms = UINT64_MAX; }
    return COG_OK;
}

cog_result_t cog_toolkit_execution_cancel(cog_toolkit_execution_t *execution, uint64_t now_ms)
{
    if (execution == NULL) { return COG_ERROR_NULL_PTR; }
    cog_result_t r = update_clock(execution, now_ms);
    if (r != COG_OK || execution->done) { return r; }
    if (!execution->admitted) {
        execution->admitted = true;
        execution->done = true;
        execution->output.status = COG_ERROR_ABORTED;
        return COG_OK;
    }
    return execution->ops->cancel(execution->handle, now_ms);
}

cog_result_t cog_toolkit_execution_result(const cog_toolkit_execution_t *execution,
    cog_toolkit_result_t *out_result, char *output, size_t capacity, size_t *out_required)
{
    if (execution == NULL || out_result == NULL || out_required == NULL) { return COG_ERROR_NULL_PTR; }
    if (output == NULL && capacity != 0U) { return COG_ERROR_NULL_PTR; }
    if (output != NULL && capacity == 0U) { return COG_ERROR_INVALID_ARG; }
    if (!execution->done) { return COG_ERROR_AGAIN; }
    if (execution->output.output_len == SIZE_MAX) { return COG_ERROR_INVALID_STATE; }
    *out_required = execution->output.output_len + 1U;
    if (capacity < *out_required) {
        out_result->output_len = execution->output.output_len;
        return COG_ERROR_OVERFLOW;
    }
    cog_toolkit_result_t result = {0};
    result.status = execution->output.status;
    result.output = output;
    result.output_len = execution->output.output_len;
    result.tool_call_id = execution->id;
    result.truncated = execution->output.truncated;
    result.side_effects_possible = execution->output.side_effects_possible;
    result.exit_status = execution->output.exit_status;
    result.exit_status_known = execution->output.exit_status_known;
    memcpy(output, execution->output.output, *out_required);
    *out_result = result;
    return COG_OK;
}

cog_result_t cog_toolkit_execution_free(cog_toolkit_execution_t *execution)
{
    if (execution == NULL) { return COG_OK; }
    if (execution->started && !execution->done) { return COG_ERROR_INVALID_STATE; }
    cog_result_t r = execution->ops->destroy(execution->handle);
    if (r != COG_OK) { return r; }
    cog_toolkit_json_free(&execution->json);
    if (execution->guard != NULL) { r = cog_store_owner_guard_return(execution->guard); }
    free(execution);
    return r;
}
