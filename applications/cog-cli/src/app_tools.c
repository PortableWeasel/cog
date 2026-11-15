/** @file app_tools.c @brief Durable sequential admission of complete tool calls. */
#include "app_internal.h"
#include <stdio.h>
#include <string.h>

cog_result_t cog_cli_tools_json(cog_cli_app_t *app)
{
    size_t used = 0U;
    app->tools_json[used++] = '[';
    for (size_t i = 0U; i < cog_workspace_tools.count; i++) {
        const cog_tool_t *tool = &cog_workspace_tools.tools[i];
        /* All interpolated bytes are immutable built-in descriptor literals;
           user/model data never enters this advertised schema construction. */
        int n = snprintf(app->tools_json + used, CLI_SCHEMA_BYTES - used,
            "%s{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"parameters\":%s}}",
            i == 0U ? "" : ",", tool->name, tool->input_schema);
        if (n < 0 || (size_t)n >= CLI_SCHEMA_BYTES - used) return COG_ERROR_LIMIT;
        used += (size_t)n;
    }
    if (used + 2U > CLI_SCHEMA_BYTES) return COG_ERROR_LIMIT;
    app->tools_json[used++] = ']'; app->tools_json[used] = '\0';
    app->tools_json_len = used;
    return COG_OK;
}
static cog_result_t record_result(cog_cli_app_t *app, cog_result_t status,
    const char *output, size_t length, bool truncated, bool effects,
    bool exit_known, int exit_status, cog_tool_execution_state_t expected)
{
    cog_message_tool_call_t *call = &app->call_views[app->tool_index];
    /* Persist execution metadata beside text. A failed/truncated command never
       masquerades as a successful complete result or permission to replay. */
    int n = snprintf(app->result_storage, 384U,
        "status=%s; truncated=%s; side_effects_possible=%s; exit_status=%s%d\n",
        cog_cli_result_name(status), truncated ? "true" : "false", effects ? "true" : "false",
        exit_known ? "" : "unknown:", exit_status);
    if (n < 0 || (size_t)n >= 384U || length > app->result_capacity - (size_t)n - 1U)
        return COG_ERROR_LIMIT;
    memmove(app->result_storage + (size_t)n, output, length);
    app->result_storage[(size_t)n + length] = '\0';
    app->pending_result_len = (size_t)n + length;
    cog_message_t message = {.role = COG_MESSAGE_ROLE_TOOL, .tool_call_id = call->id,
        .name = call->name, .content = app->result_storage, .content_len = (size_t)n + length};
    cog_tool_execution_state_t final = expected == COG_TOOL_EXECUTION_PROPOSED ? COG_TOOL_EXECUTION_DENIED :
        status == COG_OK ? COG_TOOL_EXECUTION_SUCCEEDED :
        status == COG_ERROR_ABORTED ? COG_TOOL_EXECUTION_CANCELLED :
        status == COG_ERROR_INDETERMINATE ? COG_TOOL_EXECUTION_INDETERMINATE : COG_TOOL_EXECUTION_FAILED;
    cog_result_t rc;
    if (expected == COG_TOOL_EXECUTION_PROPOSED) {
        rc = cog_session_tool_transition(app->session, app->assistant, call->id, expected, final);
        if (rc != COG_OK) return rc;
    }
    rc = cog_session_append(app->session, &message, NULL);
    if (rc != COG_OK) return rc;
    app->result_node_added = true;
    if (expected != COG_TOOL_EXECUTION_PROPOSED) {
        rc = cog_session_tool_transition(app->session, app->assistant, call->id, expected, final);
        if (rc != COG_OK) return rc;
    }
    rc = cog_cli_save(app);
    if (rc != COG_OK) { app->recovery = true; return rc; }
    if (app->display_sequence == UINT64_MAX) return COG_ERROR_LIMIT;
    app->display_sequence++;
    if (final == COG_TOOL_EXECUTION_INDETERMINATE) { app->recovery = true; return COG_ERROR_INDETERMINATE; }
    app->tool_index++;
    app->result_node_added = false;
    app->policy_prompt = false;
    app->policy_decision = COG_TOOLKIT_POLICY_UNDECIDED;
    return COG_OK;
}
cog_result_t cog_cli_tools_step(cog_cli_app_t *app, cog_state_t *next)
{
    *next = CLI_POLICY;
    if (app->terminal_result_pending) return app->tool_result_error;
    if (app->execution != NULL && app->tool_done) {
        cog_toolkit_result_t result = {0};
        size_t required = 0U;
        /* Capture beyond the result prefix; record_result uses memmove. */
        char *capture = app->result_storage + 384U;
        cog_result_t rc = cog_toolkit_execution_result(app->execution, &result, capture,
            app->result_capacity - 384U, &required);
        if (rc != COG_OK) return rc;
        rc = record_result(app, result.status, capture, result.output_len, result.truncated,
            result.side_effects_possible, result.exit_status_known, result.exit_status,
            COG_TOOL_EXECUTION_STARTED);
        if (rc != COG_OK) {
            app->terminal_result_pending = true;
            app->tool_result_error = rc;
            app->recovery = true;
            return rc;
        }
        cog_result_t released = cog_toolkit_execution_free(app->execution);
        if (released == COG_OK) app->execution = NULL;
        app->tool_done = false;
        if (rc != COG_OK) return rc;
        if (released != COG_OK) return released;
    }
    while (app->tool_index < app->call_count && app->calls[app->tool_index].resumed_closed)
        app->tool_index++;
    if (app->tool_index == app->call_count) {
        if (app->cancelling || app->shutdown) {
            app->active = false; *next = CLI_IDLE; return COG_OK;
        }
        cog_result_t rc = cog_cli_begin_request(app, true);
        *next = CLI_MODEL;
        return rc;
    }
    cog_message_tool_call_t *call = &app->call_views[app->tool_index];
    bool denied = app->cancelling || app->shutdown || app->workspace == NULL ||
        app->store == NULL || app->opts.tool_policy == COG_CLI_TOOL_POLICY_DENY;
    if (app->policy_decision == COG_TOOLKIT_POLICY_DENY) denied = true;
    if (!denied && app->opts.tool_policy == COG_CLI_TOOL_POLICY_ASK &&
        app->policy_decision != COG_TOOLKIT_POLICY_ALLOW) {
        if (!app->opts.interactive) denied = true;
        else if (!app->policy_prompt) {
            /* Display the complete request to make an interactive approval
               meaningful. Never accept an arbitrary queued prompt as approval. */
            (void)fprintf(stderr, "\nAllow tool %s %.*s? Type allow or deny: ", call->name,
                (int)call->arguments_len, call->arguments);
            app->policy_prompt = true;
            return COG_ERROR_AGAIN;
        } else return COG_ERROR_AGAIN;
    }
    if (denied) {
        static const char text[] = "Tool was not executed: policy denied or durable workspace unavailable.";
        return record_result(app, COG_ERROR_DENIED, text, sizeof(text) - 1U, false, false, false, 0,
            COG_TOOL_EXECUTION_PROPOSED);
    }
    if (app->execution == NULL) {
        cog_tool_async_config_t config = {.max_output_bytes = app->opts.max_tool_output_bytes,
            .max_argument_bytes = CLI_ARGUMENT_BYTES, .max_schema_bytes = CLI_SCHEMA_BYTES,
            .max_json_depth = 64U, .codec_workspace_bytes = CLI_CODEC_BYTES,
            .timeout_ms = app->opts.max_tool_ms, .terminate_grace_ms = 100U,
            .user = app->workspace, .owner_guard = app->guard};
        cog_result_t rc = cog_toolkit_execution_new(&app->execution, app->core, &cog_workspace_tools,
            call->id, call->name, call->arguments, call->arguments_len, &config);
        if (rc != COG_OK) {
            if (rc == COG_ERROR_NOMEM || rc == COG_ERROR_IO || rc == COG_ERROR_INDETERMINATE)
                return rc;
            const char *why = cog_cli_result_name(rc);
            return record_result(app, rc, why, strlen(why), false, false, false, 0,
                COG_TOOL_EXECUTION_PROPOSED);
        }
    }
    cog_result_t rc = cog_session_tool_transition(app->session, app->assistant, call->id,
        COG_TOOL_EXECUTION_PROPOSED, COG_TOOL_EXECUTION_STARTED);
    if (rc == COG_OK) rc = cog_cli_save(app);
    if (rc != COG_OK) { app->recovery = true; return rc; }
    rc = cog_toolkit_execution_start(app->execution, COG_TOOLKIT_POLICY_ALLOW, app->now);
    if (rc != COG_OK) {
        /* The dispatcher guarantees start failure has no effects. Preserve the
           durable intent and an explicit failed result rather than retrying. */
        cog_result_t recorded = record_result(app, rc, "Tool start failed; no automatic retry.",
            38U, false, false, false, 0, COG_TOOL_EXECUTION_STARTED);
        (void)cog_toolkit_execution_free(app->execution); app->execution = NULL;
        return recorded;
    }
    app->effects = true;
    *next = CLI_TOOL;
    return COG_OK;
}
cog_result_t cog_cli_tools_cancel(cog_cli_app_t *app)
{
    if (app->execution == NULL) return COG_OK;
    return cog_toolkit_execution_cancel(app->execution, app->now);
}
