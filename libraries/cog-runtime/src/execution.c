/** @file execution.c @brief Resolved executor authority and cooperative lifecycle. */
#include "app_internal.h"
#include "execution_internal.h"
#include "runtime_internal.h"
#include "codec_internal.h"
#include <string.h>

static bool same_owner(cog_store_owner_t a, cog_store_owner_t b)
{ return a.epoch == b.epoch && a.instance_id == b.instance_id; }
static bool child(const cli_execution_state_t *h)
{ return h->kind == COG_CLI_EXECUTION_LOCAL_CHILD || h->kind == COG_CLI_EXECUTION_REMOTE_CHILD; }
static cog_result_t remember(cog_cli_app_t *app, cog_result_t result)
{
    if (result != COG_OK && result != COG_ERROR_AGAIN && app->headless->primary == COG_OK)
        app->headless->primary = result;
    return app->headless->primary;
}
static bool id_valid(const char *id)
{
    size_t n;
    return cli_wire_cstr(id, 127U, &n) && cli_wire_session(id, n) &&
        strcmp(id, ".") != 0 && strcmp(id, "..") != 0;
}
static cog_result_t validate(const cog_cli_execution_admission_t *a)
{
    if (a->configuration == NULL) return COG_ERROR_NULL_PTR;
    if (a->kind < COG_CLI_EXECUTION_ROOT || a->kind > COG_CLI_EXECUTION_STAGED ||
        a->configuration_generation == 0U || a->reload_attempts_used > CLI_RELOAD_ATTEMPTS)
        return COG_ERROR_INVALID_ARG;
    const cog_cli_options_t *o = cog_cli_configuration_options(a->configuration);
    bool offline_recovery = a->kind == COG_CLI_EXECUTION_ROOT && a->runtime_config != NULL &&
        a->runtime_config->mode == COG_RUNTIME_RECOVER;
    if (o == NULL || o->session_action == COG_CLI_SESSION_LIST ||
        (o->session_action == COG_CLI_SESSION_RECOVER && !offline_recovery) ||
        o->socket_path != NULL) return COG_ERROR_INVALID_ARG;
    bool is_child = a->kind == COG_CLI_EXECUTION_LOCAL_CHILD || a->kind == COG_CLI_EXECUTION_REMOTE_CHILD;
    if (is_child) {
        size_t n;
        if (!id_valid(a->parent_session_id) || a->absolute_deadline_ms == 0U ||
            !cli_wire_cstr(a->task, o->max_context_bytes, &n) || n == 0U ||
            a->provenance == NULL || a->provenance_len == 0U || a->provenance_len > 4095U ||
            !cli_wire_utf8(a->provenance, a->provenance_len) || a->reload_attempts_used != 0U ||
            a->checkpoint.generation != 0U || a->expected.epoch != 0U || a->expected.instance_id != 0U ||
            a->next.epoch != 0U || a->next.instance_id != 0U) return COG_ERROR_INVALID_ARG;
        if (a->kind == COG_CLI_EXECUTION_LOCAL_CHILD)
            return a->local_session != NULL && a->session_id == NULL ? COG_OK : COG_ERROR_INVALID_ARG;
        return a->local_session == NULL && id_valid(a->session_id) ? COG_OK : COG_ERROR_INVALID_ARG;
    }
    if (a->local_session != NULL || a->parent_session_id != NULL || a->task != NULL ||
        a->provenance != NULL || a->provenance_len != 0U || a->absolute_deadline_ms != 0U)
        return COG_ERROR_INVALID_ARG;
    if (a->kind == COG_CLI_EXECUTION_STAGED) {
        return id_valid(a->session_id) && a->checkpoint.generation != 0U &&
            a->expected.epoch != 0U && a->expected.epoch != UINT64_MAX && a->expected.instance_id != 0U &&
            a->next.epoch == a->expected.epoch + 1U && a->next.instance_id != 0U ? COG_OK : COG_ERROR_INVALID_ARG;
    }
    return a->session_id == NULL && a->reload_attempts_used == 0U && a->checkpoint.generation == 0U &&
        a->expected.epoch == 0U && a->expected.instance_id == 0U && a->next.epoch == 0U &&
        a->next.instance_id == 0U ? COG_OK : COG_ERROR_INVALID_ARG;
}
static cog_result_t staged_preflight(cog_cli_app_t *app)
{
    size_t count = 0U;
    cog_result_t rc = cog_session_copy_history(app->session, NULL, app->history, CLI_NODES, &count);
    if (rc == COG_OK) rc = cog_cli_context_check(app, count);
    if (rc != COG_OK) return rc;
    cog_provider_request_t request = {.model_id = app->opts.model_id, .messages = app->history,
        .message_count = count, .stream = true, .max_tokens = app->opts.max_output_tokens,
        .configuration_generation = app->configuration_generation};
    if (app->tools_json_len > 2U) { request.tools_json = app->tools_json; request.tools_json_len = app->tools_json_len; }
    rc=cli_runtime_compile(app,&request);if(rc!=COG_OK)return rc;
    return cog_cli_observation_check(app,
        cog_provider_preflight(app->provider, &request, COG_PROVIDER_PREFLIGHT_COMPLETE));
}
cog_result_t cog_cli_execution_new(cog_cli_app_t **out, cog_cli_execution_admission_t *admission)
{
    if (out != NULL) *out = NULL;
    if (admission == NULL) return COG_ERROR_NULL_PTR;
    cog_result_t rc = out == NULL ? COG_ERROR_NULL_PTR : validate(admission);
    if (rc != COG_OK) {
        cog_cli_configuration_free(admission->configuration); admission->configuration = NULL;
        return rc;
    }
    rc = cli_app_construct_resolved(out, admission);
    if (rc == COG_OK && admission->kind == COG_CLI_EXECUTION_STAGED) {
        rc = staged_preflight(*out);
        if (rc != COG_OK) (void)cli_app_dispose(out, rc);
    }
    return rc;
}
static cog_result_t capture(cog_cli_app_t *app)
{
    cli_execution_state_t *h = app->headless;
    cog_cli_display_snapshot_t source;
    cog_result_t rc = cog_cli_app_display_snapshot(app, &source);
    if (rc != COG_OK) return rc;
    if (source.item_count > h->frozen_item_capacity) return COG_ERROR_LIMIT;
    size_t size = strlen(source.session_id) + 1U;
    for (size_t i = 0U; i < source.item_count; ++i) {
        if (source.items[i].text_len == SIZE_MAX || !cli_wire_add(&size, source.items[i].text_len + 1U))
            return COG_ERROR_LIMIT;
    }
    if (size > h->frozen_text_capacity) return COG_ERROR_LIMIT;
    size_t at = strlen(source.session_id) + 1U;
    memcpy(h->frozen_text, source.session_id, at);
    for (size_t i = 0U; i < source.item_count; ++i) {
        h->frozen_items[i] = source.items[i];
        if (source.items[i].text_len != 0U) memcpy(h->frozen_text + at, source.items[i].text, source.items[i].text_len);
        h->frozen_items[i].text = h->frozen_text + at;
        at += source.items[i].text_len; h->frozen_text[at++] = '\0';
    }
    h->frozen = source; h->frozen.session_id = h->frozen_text; h->frozen.items = h->frozen_items;
    return COG_OK;
}
cog_result_t cli_execution_barrier(cog_cli_app_t *app, cli_after_publication_t after,
    cog_state_t resume, cog_state_t *next)
{
    if (app == NULL || next == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || child(h)) { *next = resume; return COG_OK; }
    if (h->publication_pending) { h->newer_view_dirty = true; *next = CLI_PUBLICATION; return COG_OK; }
    if (h->mutation_serial == UINT64_MAX) return COG_ERROR_LIMIT;
    cog_result_t rc = capture(app);
    if (rc != COG_OK) return rc;
    h->pending_serial = ++h->mutation_serial; h->publication_pending = true;
    h->after_publication = after; h->resume_state = resume; h->publication_event_queued = false;
    *next = CLI_PUBLICATION;
    return COG_OK;
}
cog_result_t cog_cli_execution_copy_view(cog_cli_app_t *app, uint64_t *serial,
    cog_cli_display_snapshot_t *view)
{
    if (app == NULL || serial == NULL || view == NULL) return COG_ERROR_NULL_PTR;
    if (app->headless == NULL) return COG_ERROR_INVALID_STATE;
    if (!app->headless->publication_pending && app->headless->newer_view_dirty) {
        cog_state_t next;
        cog_result_t rc = cli_execution_barrier(app, app->headless->after_publication,
            app->headless->resume_state, &next);
        if (rc != COG_OK) return rc;
        app->headless->newer_view_dirty = false;
    }
    if (!app->headless->publication_pending) return COG_ERROR_AGAIN;
    *serial = app->headless->pending_serial; *view = app->headless->frozen;
    return COG_OK;
}
cog_result_t cog_cli_execution_commit_view(cog_cli_app_t *app, uint64_t serial,
    uint64_t generation, uint64_t sequence)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || child(h) || serial == 0U || generation == 0U || sequence == 0U)
        return COG_ERROR_INVALID_STATE;
    if (serial == h->committed_serial) return generation == h->committed_generation &&
        sequence == h->committed_sequence ? COG_OK : COG_ERROR_PROTOCOL;
    if (!h->publication_pending || serial != h->pending_serial ||
        (h->committed_generation != 0U && (generation != h->committed_generation || sequence <= h->committed_sequence)))
        return COG_ERROR_INVALID_STATE;
    h->committed_serial = serial; h->committed_generation = generation; h->committed_sequence = sequence;
    h->publication_pending = false;
    if (h->policy_retirement_pending) h->policy_retirement_pending = false;
    /* A newer dirty view is captured by copy_view/service, whose contracts can
       report copy exhaustion. This exact canonical acknowledgement is infallible
       after correlation checks and never hides accepted output behind an error. */
    return COG_OK;
}
cog_result_t cog_cli_execution_begin(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->drive != CLI_DRIVE_UNBEGUN || h->authority != CLI_AUTH_NORMAL || h->in_step)
        return COG_ERROR_INVALID_STATE;
    if (!app->runtime_resolved && app->opts.backend == COG_CLI_BACKEND_REAL && app->store != NULL && h->delegation == NULL)
        return COG_ERROR_INVALID_STATE;
    cog_result_t rc = cog_cli_sessions_prepare(app);
    if (rc != COG_OK) return rc;
    if (!child(h)) {
        cog_state_t next;
        rc = cli_execution_barrier(app, CLI_AFTER_NONE, CLI_IDLE, &next);
        if (rc != COG_OK) return rc;
    }
    rc = cog_core_drive_begin(app->core);
    if (rc != COG_OK) return rc;
    h->drive = CLI_DRIVE_BEGUN;
    if (child(h)) rc = cog_cli_app_submit(app, h->task, h->task_bytes);
    else if (!app->prompt_submitted) {
        if (app->opts.prompt != NULL) rc = cog_cli_app_submit(app, app->opts.prompt, strlen(app->opts.prompt));
        if (rc == COG_OK && app->opts.queue_text != NULL) rc = cog_cli_app_queue(app, app->opts.queue_text, strlen(app->opts.queue_text));
        app->prompt_submitted = true;
    }
    if (rc != COG_OK) { (void)remember(app, rc); cog_core_stop(app->core); }
    return rc;
}
cog_result_t cog_cli_execution_drain_step(cog_cli_app_t *app, cog_core_step_info_t *info)
{
    if (app == NULL || info == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->drive != CLI_DRIVE_DRAINING || h->in_step) return COG_ERROR_INVALID_STATE;
    h->in_step = true;
    *info = (cog_core_step_info_t){0U, false, UINT64_MAX, 1U};
    bool done = false;
    cog_result_t rc = cli_turn_drain_one(app, &done);
    (void)remember(app, rc);
    if (done) { h->drive = CLI_DRIVE_DONE; info->done = true; }
    h->in_step = false;
    return h->primary;
}
cog_result_t cog_cli_execution_step(cog_cli_app_t *app, cog_core_step_info_t *info)
{
    if (app == NULL || info == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->in_step || h->drive == CLI_DRIVE_UNBEGUN || h->drive == CLI_DRIVE_DONE)
        return COG_ERROR_INVALID_STATE;
    if (h->drive == CLI_DRIVE_DRAINING) return cog_cli_execution_drain_step(app, info);
    h->in_step = true;
    cog_result_t rc = cog_core_step(app->core, info);
    (void)remember(app, rc);
    if (info->done) {
        (void)remember(app, cog_core_drive_end(app->core)); h->drive_ended = true;
        h->drive = CLI_DRIVE_DRAINING; info->done = false; info->max_wait_ms = 0U;
    }
    h->in_step = false;
    return h->primary;
}
cog_result_t cog_cli_execution_end(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->headless == NULL || app->headless->in_step || app->headless->drive != CLI_DRIVE_DONE)
        return COG_ERROR_INVALID_STATE;
    return app->headless->primary;
}
cog_result_t cog_cli_execution_close(cog_cli_app_t **owner, cog_result_t primary)
{
    if (owner == NULL) return COG_ERROR_NULL_PTR;
    cog_cli_app_t *app = *owner;
    if (app == NULL) return primary;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->in_step || (h->drive != CLI_DRIVE_UNBEGUN && h->drive != CLI_DRIVE_DONE) ||
        app->provider_active || (app->execution != NULL && !app->tool_done)) return COG_ERROR_INVALID_STATE;
    return cli_app_dispose(owner, primary);
}
cog_result_t cog_cli_execution_copy_policy(const cog_cli_app_t *app, uint64_t *serial,
    cog_cli_policy_view_t *view)
{
    if (app == NULL || serial == NULL || view == NULL) return COG_ERROR_NULL_PTR;
    if (app->headless == NULL || !app->headless->policy_pending) return COG_ERROR_AGAIN;
    *serial = app->headless->prompt_serial; *view = app->headless->policy;
    return COG_OK;
}
cog_result_t cog_cli_execution_copy_configuration(const cog_cli_app_t *app,
    uint64_t *active, uint64_t *published, uint32_t *attempts)
{
    if (app == NULL || active == NULL || published == NULL || attempts == NULL) return COG_ERROR_NULL_PTR;
    *active = app->configuration_generation;
    *published = app->future_configuration != NULL ? app->future_configuration_generation : *active;
    *attempts = app->reload_attempts;
    return COG_OK;
}
static bool quiet(const cog_cli_app_t *app)
{
    if (app->active || app->provider_active || app->execution != NULL || app->delta_ready || app->event_queued ||
        app->assistant_pending || app->terminal_result_pending || app->resuming_policy || app->recovery ||
        app->future_configuration != NULL || app->retiring_configuration != NULL ||
        app->reload_notice_pending || app->reload_notice_queued || app->reload_in_progress ||
        app->headless->publication_pending || app->headless->policy_pending || app->continuation_required)
        return false;
    for (size_t i = 0U; i < CLI_INPUTS; ++i) if (app->inputs[i].used) return false;
    size_t count = 0U;
    return cog_core_pending_count(app->core, cog_session_id(app->session), &count) == COG_OK && count == 0U;
}
cog_result_t cog_cli_execution_quiesce(cog_cli_app_t *app, cog_store_owner_t expected,
    cog_store_checkpoint_t *checkpoint)
{
    if (app == NULL || checkpoint == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || child(h) || h->in_step || app->guard == NULL || !same_owner(expected, h->expected_owner))
        return COG_ERROR_INVALID_STATE;
    if (h->authority == CLI_AUTH_SOURCE_FROZEN) {
        if (!h->checkpoint_saved) return COG_ERROR_INVALID_STATE;
        *checkpoint = h->checkpoint; return COG_OK;
    }
    if (h->authority != CLI_AUTH_NORMAL) return COG_ERROR_INVALID_STATE;
    if (!quiet(app)) return COG_ERROR_AGAIN;
    h->authority = CLI_AUTH_SOURCE_FROZEN;
    cog_store_checkpoint_t token;
    cog_result_t rc = cog_store_checkpoint_save(app->store, app->session, app->guard, &token);
    if (rc != COG_OK) return rc;
    h->checkpoint = token; h->checkpoint_saved = true; *checkpoint = token;
    return COG_OK;
}
cog_result_t cog_cli_execution_release_source(cog_cli_app_t *app, cog_store_checkpoint_t token)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->in_step || !h->checkpoint_saved || token.generation != h->checkpoint.generation)
        return COG_ERROR_INVALID_STATE;
    if (h->authority == CLI_AUTH_SOURCE_RELEASED) return COG_OK;
    if (h->authority != CLI_AUTH_SOURCE_FROZEN) return COG_ERROR_INVALID_STATE;
    if (app->guard == NULL) return h->primary != COG_OK ? h->primary : COG_ERROR_INVALID_STATE;
    cog_result_t rc = cog_store_owner_release(app->guard);
    if (rc != COG_ERROR_INVALID_STATE) app->guard = NULL;
    if (rc == COG_OK) { h->source_released = true; h->authority = CLI_AUTH_SOURCE_RELEASED; }
    else if (rc != COG_ERROR_INVALID_STATE && h->primary == COG_OK)
        h->primary = rc; /* Synchronous consuming cleanup includes AGAIN. */
    return rc;
}
cog_result_t cog_cli_execution_stage_guard(cog_cli_app_t *app, cog_store_owner_t next,
    cog_store_checkpoint_t token)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->in_step || h->kind != COG_CLI_EXECUTION_STAGED ||
        !same_owner(next, h->next_owner) || token.generation != h->checkpoint.generation)
        return COG_ERROR_INVALID_STATE;
    if (h->authority == CLI_AUTH_TARGET_GUARDED && app->guard != NULL) return COG_OK;
    if (h->authority != CLI_AUTH_STAGED || app->guard != NULL) return COG_ERROR_INVALID_STATE;
    cog_store_owner_t actual; cog_store_checkpoint_t current;
    cog_result_t rc = cog_store_owner_read(app->store, cog_session_id(app->session), &actual, &current);
    if (rc != COG_OK) return rc;
    if (!same_owner(next, actual) || current.generation != token.generation) return COG_ERROR_INVALID_STATE;
    rc = cog_store_owner_acquire(app->store, cog_session_id(app->session), next, &app->guard);
    if (rc == COG_OK) { h->target_guard_acquired = true; h->authority = CLI_AUTH_TARGET_GUARDED; }
    return rc;
}
cog_result_t cog_cli_execution_stage_activate(cog_cli_app_t *app, cog_store_owner_t next,
    cog_store_checkpoint_t token)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->in_step || h->kind != COG_CLI_EXECUTION_STAGED ||
        !same_owner(next, h->next_owner) || token.generation != h->checkpoint.generation)
        return COG_ERROR_INVALID_STATE;
    if (h->activation_accepted) return COG_OK;
    if (h->authority != CLI_AUTH_TARGET_GUARDED || app->guard == NULL) return COG_ERROR_INVALID_STATE;
    h->activation_accepted = true; h->authority = CLI_AUTH_NORMAL; h->expected_owner = next;
    return COG_OK;
}
cog_result_t cog_cli_execution_apply_command(cog_cli_app_t *app, const cog_cli_command_t *cmd,
    cog_store_owner_t owner, uint64_t serial)
{
    if (app == NULL || cmd == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (h == NULL || h->in_step || h->authority != CLI_AUTH_NORMAL ||
        !same_owner(owner, h->expected_owner) || h->drive != CLI_DRIVE_BEGUN)
        return COG_ERROR_INVALID_STATE;
    size_t required;
    cog_result_t rc = cog_cli_command_encode(cmd, NULL, 0U, &required);
    if (rc != COG_ERROR_OVERFLOW && rc != COG_OK) return rc;
    if (child(h) && cmd->kind != COG_CLI_COMMAND_CANCEL) return COG_ERROR_DENIED;
    if (cmd->kind == COG_CLI_COMMAND_CANCEL) return cog_cli_app_cancel(app);
    if (cmd->kind == COG_CLI_COMMAND_SUBMIT) return cog_cli_app_queue(app, cmd->text, cmd->text_len);
    if (cmd->kind == COG_CLI_COMMAND_CONTINUE || cmd->kind == COG_CLI_COMMAND_FORK) {
        if (serial == 0U || serial != h->mutation_serial || h->publication_pending || app->active)
            return COG_ERROR_INVALID_STATE;
        cog_session_node_t *node = cog_session_active(app->session);
        if (cmd->kind == COG_CLI_COMMAND_CONTINUE) {
            if (!app->continuation_required || node == NULL || cog_node_data(node)->node_id != cmd->node_id)
                return COG_ERROR_INVALID_STATE;
            return cog_cli_app_submit(app, cmd->text, cmd->text_len);
        }
        return cog_cli_app_fork(app, cmd->node_id);
    }
    if (cmd->kind != COG_CLI_COMMAND_POLICY) return COG_ERROR_DENIED;
    if (!h->policy_pending || serial != h->prompt_serial || app->cancelling || app->shutdown ||
        app->assistant == NULL || cmd->node_id != h->policy.assistant_node ||
        cmd->call_ordinal != h->policy.call_ordinal || strcmp(cmd->target_session_id, h->policy.session_id) != 0 ||
        app->now >= h->policy.deadline_ms || app->guard == NULL) return COG_ERROR_INVALID_STATE;
    cog_tool_execution_state_t state;
    rc = cog_session_tool_state(app->session, app->assistant, app->call_views[app->tool_index].id, &state);
    if (rc != COG_OK || state != COG_TOOL_EXECUTION_PROPOSED) return COG_ERROR_INVALID_STATE;
    if (h->publication_pending) return COG_ERROR_AGAIN;
    cog_state_t next;
    rc = cli_execution_barrier(app, CLI_AFTER_CONTROL, CLI_POLICY, &next);
    if (rc != COG_OK) return rc;
    app->policy_decision = cmd->decision == 2U ? COG_TOOLKIT_POLICY_ALLOW : COG_TOOLKIT_POLICY_DENY;
    h->policy_pending = false; h->policy_retirement_pending = true; h->retired_prompt_serial = h->prompt_serial;
    return COG_OK;
}
