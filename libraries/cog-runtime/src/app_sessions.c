/** @file app_sessions.c @brief Guarded durable session selection and offline UX. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include "execution_internal.h"
#include "runtime_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

bool cog_cli_metadata(const cog_cli_app_t *app)
{
    return (app->runtime_resolved && app->runtime_config.mode==COG_RUNTIME_INSPECT) || app->opts.session_action == COG_CLI_SESSION_LIST ||
        app->opts.session_action == COG_CLI_SESSION_RECOVER;
}
static cog_result_t directory_create(const char *path)
{
    char copy[4097];
    size_t length = strlen(path);
    if (length == 0U || length >= sizeof(copy)) return COG_ERROR_LIMIT;
    memcpy(copy, path, length + 1U);
    int parent = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return COG_ERROR_IO;
    char *part = copy + (path[0] == '/' ? 1U : 0U);
    cog_result_t rc = COG_OK;
    while (*part != '\0') {
        char *slash = strchr(part, '/');
        if (slash != NULL) *slash = '\0';
        if (*part != '\0' && strcmp(part, ".") != 0) {
            if (strcmp(part, "..") == 0) { rc = COG_ERROR_DENIED; break; }
            bool created = mkdirat(parent, part, 0700) == 0;
            if (!created && errno != EEXIST) { rc = COG_ERROR_IO; break; }
            int next = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) { rc = COG_ERROR_IO; break; }
            if (created && (fsync(next) != 0 || fsync(parent) != 0)) {
                (void)close(next); rc = COG_ERROR_IO; break;
            }
            (void)close(parent); parent = next;
        }
        if (slash == NULL) break;
        part = slash + 1;
    }
    (void)close(parent);
    return rc;
}
static cog_result_t incarnation(uint64_t *value)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return COG_ERROR_IO;
    ssize_t n = read(fd, value, sizeof(*value));
    (void)close(fd);
    return n == (ssize_t)sizeof(*value) && *value != 0U ? COG_OK : COG_ERROR_IO;
}
static cog_session_config_t session_config(const cog_cli_app_t *app, const char *id)
{
    if(app->runtime_resolved){cog_session_config_t c=app->runtime_config.session;c.id=id;if(c.max_nodes==0U)c.max_nodes=CLI_NODES;if(c.max_tool_calls_per_message==0U)c.max_tool_calls_per_message=CLI_CALLS;return c;}
    return (cog_session_config_t){.id = id, .max_nodes = CLI_NODES,
        .max_payload_bytes = app->opts.max_context_bytes * 4U,
        .max_tool_calls_per_message = CLI_CALLS};
}
static cog_result_t restore_capacity(cog_cli_app_t *app, cog_session_t *loaded)
{
    cog_session_snapshot_t view;
    cog_result_t rc = cog_session_snapshot_view(loaded, &view);
    if (rc != COG_OK) return rc;
    cog_session_config_t config = session_config(app, view.session_id);
    return cog_session_restore(&app->session, &config, &view);
}
static cog_result_t admit(cog_cli_app_t *app, cog_store_owner_t old,
    cog_store_checkpoint_t checkpoint, bool recovery)
{
    if (old.epoch == UINT64_MAX) return COG_ERROR_LIMIT;
    cog_store_owner_t owner = {.epoch = old.epoch + 1U};
    cog_result_t rc = incarnation(&owner.instance_id);
    if (rc != COG_OK) return rc;
    if (owner.instance_id == old.instance_id) return COG_ERROR_IO;
    rc = cog_store_owner_compare_exchange(app->store, cog_session_id(app->session),
        old, owner, checkpoint);
    if (rc != COG_OK) return rc;
    return recovery ? cog_store_owner_acquire_recovery(app->store,
        cog_session_id(app->session), owner, &app->guard) :
        cog_store_owner_acquire(app->store, cog_session_id(app->session), owner, &app->guard);
}
static cog_result_t save_empty(cog_cli_app_t *app)
{
    cog_store_checkpoint_t checkpoint = {0};
    cog_result_t rc = cog_store_checkpoint_save(app->store, app->session, app->guard, &checkpoint);
    if (rc == COG_ERROR_INDETERMINATE) app->recovery = true;
    return rc;
}
cog_result_t cog_cli_sessions_open(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    bool fresh = app->opts.session_action == COG_CLI_SESSION_NEW;
    if (fresh) {
        cog_session_config_t config = session_config(app, app->opts.session_id);
        cog_result_t rc = cog_session_new(&app->session, &config);
        if(rc==COG_OK)rc=cli_runtime_initial_context(app);
        if (rc != COG_OK || app->opts.store_mode == COG_CLI_STORE_DISABLED) return rc;
    }
    cog_result_t rc = COG_OK;
    if(app->runtime_resolved && app->runtime_config.mode==COG_RUNTIME_INSPECT){
        cog_store_config_t inspect=app->runtime_config.store;inspect.require_owner=false;
        rc=cog_store_new(&app->store,&inspect);
        if(rc!=COG_OK)return rc;
        cog_store_load_report_t report;
        return cog_store_load_ex(app->store,app->runtime_config.session_id,COG_STORE_LOAD_INSPECT,&app->session,&report);
    }
    if (fresh && (!app->runtime_resolved || app->runtime_config.store.backend == NULL || app->runtime_config.store.backend == &cog_store_file_backend)) {
        rc = directory_create(app->opts.store_dir);
        if (rc != COG_OK) return rc;
    }
    cog_store_config_t store = {.base_dir = app->opts.store_dir, .require_owner = true,
        .max_record_bytes = app->opts.max_context_bytes * 16U};
    if(app->runtime_resolved)store=app->runtime_config.store;
    rc = cog_store_new(&app->store, &store);
    if (rc != COG_OK || app->opts.session_action == COG_CLI_SESSION_LIST) return rc;
    const char *id = fresh ? cog_session_id(app->session) : app->opts.source_session_id;
    cog_store_owner_t owner = {0}; cog_store_checkpoint_t checkpoint = {0};
    rc = cog_store_owner_read(app->store, id, &owner, &checkpoint);
    if (rc != COG_OK) return rc;
    if (fresh) {
        if (owner.epoch != 0U || checkpoint.generation != 0U) return COG_ERROR_INVALID_STATE;
        rc = admit(app, owner, checkpoint, false);
        return rc == COG_OK ? save_empty(app) : rc;
    }
    cog_session_t *loaded = NULL;
    cog_store_load_report_t report = {0};
    if (checkpoint.generation == 0U) {
        rc = cog_store_load_ex(app->store, id, COG_STORE_LOAD_REPLAY, &loaded, &report);
        if (rc == COG_ERROR_NOT_FOUND && owner.epoch != 0U && owner.instance_id != 0U &&
            app->opts.session_action == COG_CLI_SESSION_RESUME) {
            /* G's checked legacy absence plus exact CAS/guard are conjunctive:
               a read alone never authorizes recreating an empty reservation. */
            cog_session_config_t config = session_config(app, id);
            rc = cog_session_new(&app->session, &config);
            if (rc == COG_OK) rc = admit(app, owner, checkpoint, false);
            return rc == COG_OK ? save_empty(app) : rc;
        }
        if (rc != COG_OK) return rc;
        if (app->opts.session_action == COG_CLI_SESSION_RECOVER) rc = COG_ERROR_INVALID_STATE;
        else if (!app->opts.previous_processes_fenced) rc = COG_ERROR_DENIED;
        else if (report.source_version != 1U || !report.migrated_in_memory || report.indeterminate_tools != 0U)
            rc = COG_ERROR_UNSUPPORTED;
        if (rc == COG_OK) {
            /* v1 cannot acquire a fenced guard. Explicit migration publishes an
               unowned v2 checkpoint under its own matching store constructor. */
            cog_store_t *unowned = NULL; store.require_owner = false;
            rc = cog_store_new(&unowned, &store);
            if (rc == COG_OK) rc = cog_store_save(unowned, loaded);
            cog_store_free(unowned);
        }
        cog_session_free(loaded); loaded = NULL;
        if (rc != COG_OK) return rc;
        rc = cog_store_owner_read(app->store, id, &owner, &checkpoint);
        if (rc != COG_OK) return rc;
    }
    if (checkpoint.generation == 0U) return COG_ERROR_NOT_FOUND;
    bool recovery = app->opts.session_action == COG_CLI_SESSION_RECOVER;
    rc = cog_store_checkpoint_load(app->store, id, checkpoint, recovery?COG_STORE_LOAD_INSPECT:COG_STORE_LOAD_REPLAY,
        &loaded, &report);
    if (rc != COG_OK) return rc;
    bool unresolved=report.indeterminate_tools!=0U || report.indeterminate_jobs!=0U;
    if (unresolved && (!recovery || owner.epoch == 0U)) rc = COG_ERROR_INDETERMINATE;
    else if (recovery && !unresolved) rc = COG_ERROR_INVALID_STATE;
    else if (owner.epoch == 0U && !app->opts.previous_processes_fenced) rc = COG_ERROR_DENIED;
    if (rc == COG_OK) rc = restore_capacity(app, loaded);
    cog_session_free(loaded);
    if (rc != COG_OK) return rc;
    rc = admit(app, owner, checkpoint, recovery);
    if (rc != COG_OK) return rc;
    app->recovery = recovery;
    cog_session_node_t *active = cog_session_active(app->session);
    app->continuation_required = active != NULL && cog_node_data(active)->message->status != COG_MESSAGE_COMPLETE;
    return COG_OK;
}
cog_result_t cli_sessions_admit(cog_cli_app_t *app, const cog_cli_execution_admission_t *admission)
{
    if (app == NULL || admission == NULL || app->headless == NULL) return COG_ERROR_NULL_PTR;
    cli_execution_state_t *h = app->headless;
    if (admission->kind == COG_CLI_EXECUTION_ROOT) {
        cog_result_t rc = cog_cli_sessions_open(app);
        if (rc == COG_OK && app->store != NULL) {
            cog_store_checkpoint_t token;
            rc = cog_store_owner_read(app->store, cog_session_id(app->session), &h->expected_owner, &token);
        }
        return rc;
    }
    if (app->opts.store_mode == COG_CLI_STORE_DISABLED || (!app->runtime_resolved && app->opts.store_dir == NULL))
        return COG_ERROR_INVALID_ARG;
    cog_store_config_t config = {.base_dir = app->opts.store_dir, .require_owner = true,
        .max_record_bytes = app->opts.max_context_bytes * 16U};
    if(app->runtime_resolved)config=app->runtime_config.store;
    cog_result_t rc = cog_store_new(&app->store, &config);
    if (rc != COG_OK) return rc;
    const char *id = admission->kind == COG_CLI_EXECUTION_LOCAL_CHILD ?
        cog_session_id(admission->local_session) : admission->session_id;
    cog_store_owner_t owner = {0}; cog_store_checkpoint_t token = {0};
    rc = cog_store_owner_read(app->store, id, &owner, &token);
    if (rc != COG_OK) return rc;
    if (admission->kind == COG_CLI_EXECUTION_STAGED) {
        if (owner.epoch != admission->expected.epoch || owner.instance_id != admission->expected.instance_id ||
            token.generation != admission->checkpoint.generation) return COG_ERROR_INVALID_STATE;
        cog_session_t *loaded = NULL; cog_store_load_report_t report = {0};
        rc = cog_store_checkpoint_load(app->store, id, admission->checkpoint, COG_STORE_LOAD_INSPECT, &loaded, &report);
        if (rc == COG_OK) rc = restore_capacity(app, loaded);
        cog_session_free(loaded);
        if (rc != COG_OK) return rc;
        cog_session_snapshot_t snapshot;
        rc = cog_session_snapshot_view(app->session, &snapshot);
        if (rc != COG_OK) return rc;
        for (size_t i = 0U; i < snapshot.ledger_count; ++i)
            if (snapshot.ledger[i].state == COG_TOOL_EXECUTION_STARTED ||
                snapshot.ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE ||
                snapshot.ledger[i].state == COG_TOOL_EXECUTION_PROPOSED) return COG_ERROR_INDETERMINATE;
        cog_session_node_t *active = cog_session_active(app->session);
        return active != NULL && cog_node_data(active)->message->status != COG_MESSAGE_COMPLETE ?
            COG_ERROR_INVALID_STATE : COG_OK;
    }
    if (owner.epoch != 0U || owner.instance_id != 0U || token.generation != 0U)
        return COG_ERROR_INVALID_STATE;
    if (admission->kind == COG_CLI_EXECUTION_LOCAL_CHILD) {
        if (cog_session_root(admission->local_session) != NULL ||
            cog_session_parent_id(admission->local_session) == NULL ||
            strcmp(cog_session_parent_id(admission->local_session), admission->parent_session_id) != 0)
            return COG_ERROR_INVALID_STATE;
        app->session = admission->local_session; /* ownership marker already set */
    } else {
        cog_session_config_t session = session_config(app, id);
        rc = cog_session_new(&app->session, &session);
        if (rc == COG_OK) rc = cog_session_set_parent(app->session, admission->parent_session_id);
        if (rc != COG_OK) return rc;
    }
    if(app->runtime_resolved) {
        rc=cli_runtime_initial_context(app);
        if(rc!=COG_OK)return rc;
    }
    rc = admit(app, owner, token, false);
    if (rc != COG_OK) return rc;
    rc = cog_store_owner_read(app->store, id, &h->expected_owner, &token);
    if (rc != COG_OK) return rc;
    cog_message_t context = {.role = COG_MESSAGE_ROLE_SYSTEM,
        .content = h->provenance, .content_len = h->provenance_bytes};
    rc = cog_session_append(app->session, &context, NULL);
    if (rc == COG_OK) rc = save_empty(app);
    return rc;
}
cog_result_t cog_cli_sessions_prepare(cog_cli_app_t *app)
{
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(app->session, &snapshot);
    if (rc != COG_OK) return rc;
    uint64_t assistant_id = 0U;
    for (size_t i = 0U; i < snapshot.ledger_count; i++) {
        const cog_tool_ledger_record_t *entry = &snapshot.ledger[i];
        if (entry->state == COG_TOOL_EXECUTION_STARTED || entry->state == COG_TOOL_EXECUTION_INDETERMINATE)
            return COG_ERROR_INDETERMINATE;
        if (entry->state == COG_TOOL_EXECUTION_PROPOSED) {
            if (assistant_id != 0U && assistant_id != entry->assistant_node_id) return COG_ERROR_PROTOCOL;
            assistant_id = entry->assistant_node_id;
        }
    }
    if (assistant_id == 0U) return COG_OK;
    if (app->opts.session_action != COG_CLI_SESSION_RESUME) return COG_ERROR_INVALID_STATE;
    cog_session_node_t *assistant = cog_cli_session_node(app, assistant_id);
    if (assistant == NULL) return COG_ERROR_PROTOCOL;
    const cog_message_t *message = cog_node_data(assistant)->message;
    if (message->tool_call_count == 0U || message->tool_call_count > CLI_CALLS) return COG_ERROR_LIMIT;
    size_t count = 0U;
    for (cog_session_node_t *node = cog_session_active(app->session); node != NULL; node = cog_node_parent(node))
        if (++count > CLI_NODES) return COG_ERROR_LIMIT;
    size_t at = count;
    for (cog_session_node_t *node = cog_session_active(app->session); node != NULL; node = cog_node_parent(node))
        app->history[--at] = *cog_node_data(node)->message;
    rc = cog_cli_context_check(app, count);
    if (rc != COG_OK) return rc;
    cog_provider_request_t request = {.model_id = app->opts.model_id,
        .messages = app->history, .message_count = count, .stream = true,
        .max_tokens = app->opts.max_output_tokens, .configuration_generation = app->configuration_generation};
    if (app->workspace != NULL) { request.tools_json = app->tools_json; request.tools_json_len = app->tools_json_len; }
    rc=cli_runtime_compile(app,&request);
    if(rc==COG_OK)rc = cog_provider_preflight(app->provider, &request, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS);
    rc = cog_cli_observation_check(app, rc);
    if (rc != COG_OK) return rc;
    for (size_t i = 0U; i < message->tool_call_count; i++) {
        const cog_message_tool_call_t *source = &message->tool_calls[i];
        cli_call_t *call = &app->calls[i];
        if (strlen(source->id) >= sizeof(call->id) || strlen(source->name) >= sizeof(call->name) ||
            source->arguments_len >= CLI_ARGUMENT_BYTES) return COG_ERROR_LIMIT;
        memcpy(call->id, source->id, strlen(source->id) + 1U);
        memcpy(call->name, source->name, strlen(source->name) + 1U);
        memcpy(call->arguments, source->arguments, source->arguments_len);
        call->arguments[source->arguments_len] = '\0';
        call->begun = true; call->ended = true;
        call->view = (cog_message_tool_call_t){call->id, call->name, call->arguments, source->arguments_len};
        app->call_views[i] = call->view;
        cog_tool_execution_state_t state;
        rc = cog_session_tool_state(app->session, assistant, source->id, &state);
        if (rc != COG_OK) return rc;
        call->resumed_closed = state != COG_TOOL_EXECUTION_PROPOSED;
    }
    app->assistant = assistant; app->call_count = message->tool_call_count;
    app->tool_index = 0U; app->active = true; app->resuming_policy = true;
    app->accepted = true; app->effects = true; app->rounds = 1U;
    app->started_at = app->now;
    return COG_OK;
}
cog_session_node_t *cog_cli_session_node(cog_cli_app_t *app, uint64_t id)
{
    cog_session_node_t *node = cog_session_root(app->session);
    for (size_t count = 0U; node != NULL && count < CLI_NODES; count++) {
        if (cog_node_data(node)->node_id == id) return node;
        if (cog_node_first_child(node) != NULL) node = cog_node_first_child(node);
        else {
            while (node != NULL && cog_node_next_sibling(node) == NULL) node = cog_node_parent(node);
            if (node != NULL) node = cog_node_next_sibling(node);
        }
    }
    return NULL;
}
cog_result_t cog_cli_app_fork(cog_cli_app_t *app, uint64_t node_id)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->reload_in_progress) return COG_ERROR_INVALID_STATE;
    if (app->reload_cleanup_error != COG_OK) return COG_ERROR_INDETERMINATE;
    if (app->observation_error != COG_OK) return COG_ERROR_INDETERMINATE;
    if (cog_cli_metadata(app)) return COG_ERROR_INVALID_STATE;
    if (app->assistant_pending) return app->assistant_error;
    if (app->recovery) return COG_ERROR_INDETERMINATE;
    if (app->active) return COG_ERROR_AGAIN;
    for (size_t i = 0U; i < CLI_INPUTS; i++) if (app->inputs[i].used) return COG_ERROR_AGAIN;
    size_t count = 0U;
    cog_result_t rc = cog_core_pending_count(app->core, cog_session_id(app->session), &count);
    if (rc != COG_OK) return rc;
    if (count != 0U) return COG_ERROR_AGAIN;
    cog_session_node_t *node = node_id == 0U ? cog_session_active(app->session) :
        cog_cli_session_node(app, node_id);
    if (node == NULL) return COG_ERROR_NOT_FOUND;
    return cog_session_fork(app->session, node, &app->fork_parent);
}
cog_result_t cog_cli_app_reconcile(cog_cli_app_t *app, uint64_t assistant_id,
    const char *call_id, cog_tool_execution_state_t resolution,
    const char *evidence, size_t length)
{
    if (app == NULL || call_id == NULL || evidence == NULL) return COG_ERROR_NULL_PTR;
    if (app->opts.session_action != COG_CLI_SESSION_RECOVER || !app->opts.previous_processes_fenced ||
        app->guard == NULL || app->recovery_write_failed) return COG_ERROR_INVALID_STATE;
    if (*call_id == '\0' || length == 0U || length >= CLI_EVIDENCE_BYTES ||
        !cog_cli_text_valid(evidence, length) ||
        (resolution != COG_TOOL_EXECUTION_SUCCEEDED && resolution != COG_TOOL_EXECUTION_FAILED &&
         resolution != COG_TOOL_EXECUTION_CANCELLED && resolution != COG_TOOL_EXECUTION_ACKNOWLEDGED))
        return COG_ERROR_INVALID_ARG;
    cog_session_node_t *assistant = cog_cli_session_node(app, assistant_id);
    if (assistant == NULL) return COG_ERROR_NOT_FOUND;
    const char *outcome = resolution == COG_TOOL_EXECUTION_SUCCEEDED ? "succeeded (operator-verified)" :
        resolution == COG_TOOL_EXECUTION_FAILED ? "failed (operator-verified)" :
        resolution == COG_TOOL_EXECUTION_CANCELLED ? "cancelled (operator-verified)" :
        "acknowledged; effects remain unknown";
    int prefix = snprintf(app->result_storage, app->result_capacity,
        "Recovery outcome: %s. Operator evidence: ", outcome);
    if (prefix < 0 || (size_t)prefix >= app->result_capacity ||
        length >= app->result_capacity - (size_t)prefix) return COG_ERROR_LIMIT;
    memcpy(app->result_storage + (size_t)prefix, evidence, length);
    app->result_storage[(size_t)prefix + length] = '\0';
    cog_message_t result = {.role = COG_MESSAGE_ROLE_TOOL, .tool_call_id = call_id,
        .content = app->result_storage, .content_len = (size_t)prefix + length};
    cog_result_t rc = cog_session_reconcile_tool(app->session, assistant, call_id, resolution, &result, NULL);
    if (rc != COG_OK) return rc;
    rc = cog_cli_save(app);
    if (rc != COG_OK) app->recovery_write_failed = true;
    else app->display_sequence++;
    return rc;
}
