/** @file app_sessions.c @brief Guarded durable session selection and offline UX. */
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
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
    return app->opts.session_action == COG_CLI_SESSION_LIST ||
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
        if (rc != COG_OK || app->opts.store_mode == COG_CLI_STORE_DISABLED) return rc;
    }
    cog_result_t rc = COG_OK;
    if (fresh) {
        rc = directory_create(app->opts.store_dir);
        if (rc != COG_OK) return rc;
    }
    cog_store_config_t store = {.base_dir = app->opts.store_dir, .require_owner = true,
        .max_record_bytes = app->opts.max_context_bytes * 16U};
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
    rc = cog_store_checkpoint_load(app->store, id, checkpoint, COG_STORE_LOAD_REPLAY,
        &loaded, &report);
    if (rc != COG_OK) return rc;
    bool recovery = app->opts.session_action == COG_CLI_SESSION_RECOVER;
    if (report.indeterminate_tools != 0U && (!recovery || owner.epoch == 0U)) rc = COG_ERROR_INDETERMINATE;
    else if (recovery && report.indeterminate_tools == 0U) rc = COG_ERROR_INVALID_STATE;
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
    rc = cog_provider_preflight(app->provider, &request, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS);
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
static cog_result_t list_sessions(cog_cli_app_t *app)
{
    if (app->store == NULL) return COG_ERROR_INVALID_STATE;
    char **ids = NULL; size_t count = 0U, bytes = 0U;
    cog_result_t rc = cog_store_list(app->store, &ids, &count);
    if (rc != COG_OK) return rc;
    if (count > CLI_LIST_IDS) rc = COG_ERROR_LIMIT;
    for (size_t i = 0U; i < count && rc == COG_OK; i++) {
        size_t n = strlen(ids[i]);
        if (n == 0U || n > 127U || !cog_cli_text_valid(ids[i], n) ||
            strchr(ids[i], '/') != NULL || strchr(ids[i], '\\') != NULL ||
            strcmp(ids[i], ".") == 0 || strcmp(ids[i], "..") == 0) { rc = COG_ERROR_PROTOCOL; break; }
        for (size_t j = 0U; j < n; j++) if ((unsigned char)ids[i][j] < 32U || ids[i][j] == 127) rc = COG_ERROR_PROTOCOL;
        if (n + 1U > app->opts.max_context_bytes - bytes) rc = COG_ERROR_LIMIT;
        else bytes += n + 1U;
    }
    if (rc == COG_OK) for (size_t i = 0U; i < count; i++)
        if (fprintf(app->out, "%s\n", ids[i]) < 0) { rc = COG_ERROR_IO; break; }
    if (rc == COG_OK && fflush(app->out) != 0) rc = COG_ERROR_IO;
    for (size_t i = 0U; i < count; i++) free(ids[i]);
    free(ids);
    return rc;
}
static cog_result_t history(cog_cli_app_t *app)
{
    cog_cli_display_snapshot_t snapshot;
    cog_result_t rc = cog_cli_app_display_snapshot(app, &snapshot);
    if (rc != COG_OK) return rc;
    for (size_t i = 0U; i < snapshot.item_count; i++) {
        const cog_cli_display_item_t *item = &snapshot.items[i];
        static const char *const roles[] = {"system", "user", "assistant", "tool"};
        static const char *const states[] = {"complete", "incomplete", "cancelled", "failed"};
        if (fprintf(app->out, "[node=%" PRIu64 " %s %s] ", item->node_id,
            roles[item->role], states[item->status]) < 0 ||
            (item->text_len != 0U && fwrite(item->text, 1U, item->text_len, app->out) != item->text_len) ||
            fputc('\n', app->out) == EOF) return COG_ERROR_IO;
    }
    return fflush(app->out) == 0 ? COG_OK : COG_ERROR_IO;
}
cog_result_t cog_cli_usage(cog_cli_app_t *app, const cog_session_node_t *node,
    FILE *stream, bool explicit_request)
{
    if (node == NULL) return COG_ERROR_NOT_FOUND;
    const cog_node_data_t *data = cog_node_data(node);
    const cog_usage_t *usage = &data->message->usage;
    (void)app;
    if (!usage->tokens_known && !usage->reasoning_tokens_known && !usage->cost_known) {
        if (explicit_request && (fprintf(stream, "node=%" PRIu64 " usage unavailable\n", data->node_id) < 0 ||
            fflush(stream) != 0)) return COG_ERROR_IO;
        return COG_OK;
    }
    if (fprintf(stream, "node=%" PRIu64 " reported usage:", data->node_id) < 0) return COG_ERROR_IO;
    if (usage->tokens_known && fprintf(stream, " input=%" PRIu64 " output=%" PRIu64,
        usage->input_tokens, usage->output_tokens) < 0) return COG_ERROR_IO;
    if (usage->reasoning_tokens_known && fprintf(stream, " reasoning=%" PRIu64, usage->reasoning_tokens) < 0)
        return COG_ERROR_IO;
    if (usage->cost_known && fprintf(stream, " cost=%.17g %s", usage->cost, usage->currency) < 0)
        return COG_ERROR_IO;
    return fputc('\n', stream) != EOF && fflush(stream) == 0 ? COG_OK : COG_ERROR_IO;
}
static cog_result_t escaped_id(FILE *stream, const char *text)
{
    if (fputc('"', stream) == EOF) return COG_ERROR_IO;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', stream) == EOF || fputc(*p, stream) == EOF) return COG_ERROR_IO;
        } else if (*p == '\n') {
            if (fputs("\\n", stream) < 0) return COG_ERROR_IO;
        } else if (*p == '\r') {
            if (fputs("\\r", stream) < 0) return COG_ERROR_IO;
        } else if (*p == '\t') {
            if (fputs("\\t", stream) < 0) return COG_ERROR_IO;
        } else if (*p < 32U || *p >= 127U) {
            if (fprintf(stream, "\\x%02x", (unsigned int)*p) < 0) return COG_ERROR_IO;
        } else if (fputc(*p, stream) == EOF) return COG_ERROR_IO;
    }
    return fputc('"', stream) == EOF ? COG_ERROR_IO : COG_OK;
}
static cog_result_t tools(cog_cli_app_t *app)
{
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(app->session, &snapshot);
    if (rc != COG_OK) return rc;
    static const char *const states[] = {"proposed", "started", "succeeded", "failed", "denied", "cancelled", "indeterminate", "acknowledged"};
    for (size_t i = 0U; i < snapshot.ledger_count; i++) {
        const cog_tool_ledger_record_t *entry = &snapshot.ledger[i];
        cog_session_node_t *node = cog_cli_session_node(app, entry->assistant_node_id);
        if (node == NULL) return COG_ERROR_PROTOCOL;
        const cog_message_t *message = cog_node_data(node)->message;
        size_t ordinal = 0U;
        for (size_t j = 0U; j < message->tool_call_count; j++)
            if (strcmp(message->tool_calls[j].id, entry->call_id) == 0) { ordinal = j + 1U; break; }
        if (ordinal == 0U) return COG_ERROR_PROTOCOL;
        if (fprintf(app->out, "node=%" PRIu64 " ordinal=%zu call=",
            entry->assistant_node_id, ordinal) < 0) return COG_ERROR_IO;
        rc = escaped_id(app->out, entry->call_id);
        if (rc != COG_OK) return rc;
        if (fprintf(app->out, " state=%s\n", states[entry->state]) < 0) return COG_ERROR_IO;
    }
    return fflush(app->out) == 0 ? COG_OK : COG_ERROR_IO;
}
static bool decimal(const char *text, uint64_t *value)
{
    uint64_t result = 0U;
    if (*text == '\0') return false;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || result > (UINT64_MAX - (uint64_t)(*p - '0')) / 10U) return false;
        result = result * 10U + (uint64_t)(*p - '0');
    }
    *value = result; return true;
}
cog_result_t cog_cli_session_command(cog_cli_app_t *app, const char *text, bool *handled)
{
    *handled = true;
    bool recovery = app->opts.session_action == COG_CLI_SESSION_RECOVER;
    if (strcmp(text, "/history") == 0) return history(app);
    if (strcmp(text, "/tools") == 0) return tools(app);
    if (strcmp(text, "/help") == 0) {
        const char *help = recovery ? "/tools /history /reconcile NODE CALL OUTCOME EVIDENCE /reconcile-index NODE ORDINAL OUTCOME EVIDENCE /quit\n" :
            "/sessions /history /tools /fork NODE /usage /cancel /quit\n";
        return fputs(help, app->out) >= 0 && fflush(app->out) == 0 ? COG_OK : COG_ERROR_IO;
    }
    bool indexed = strncmp(text, "/reconcile-index ", sizeof("/reconcile-index ") - 1U) == 0;
    if (indexed || strncmp(text, "/reconcile ", sizeof("/reconcile ") - 1U) == 0) {
        if (!recovery) return COG_ERROR_INVALID_STATE;
        const char *arguments = text + (indexed ? sizeof("/reconcile-index ") - 1U : sizeof("/reconcile ") - 1U);
        char copy[4096]; size_t n = strlen(arguments);
        if (n >= sizeof(copy)) return COG_ERROR_LIMIT;
        memcpy(copy, arguments, n + 1U);
        char *node = copy, *call = strchr(node, ' ');
        if (call == NULL) return COG_ERROR_INVALID_ARG;
        *call++ = '\0'; char *outcome = strchr(call, ' ');
        if (outcome == NULL) return COG_ERROR_INVALID_ARG;
        *outcome++ = '\0'; char *evidence = strchr(outcome, ' ');
        if (evidence == NULL) return COG_ERROR_INVALID_ARG;
        *evidence++ = '\0'; uint64_t id;
        if (!decimal(node, &id)) return COG_ERROR_INVALID_ARG;
        cog_tool_execution_state_t state;
        if (strcmp(outcome, "succeeded") == 0) state = COG_TOOL_EXECUTION_SUCCEEDED;
        else if (strcmp(outcome, "failed") == 0) state = COG_TOOL_EXECUTION_FAILED;
        else if (strcmp(outcome, "cancelled") == 0) state = COG_TOOL_EXECUTION_CANCELLED;
        else if (strcmp(outcome, "acknowledged") == 0) state = COG_TOOL_EXECUTION_ACKNOWLEDGED;
        else return COG_ERROR_INVALID_ARG;
        const char *call_id = call;
        if (indexed) {
            uint64_t ordinal;
            if (!decimal(call, &ordinal) || ordinal == 0U) return COG_ERROR_INVALID_ARG;
            cog_session_node_t *assistant = cog_cli_session_node(app, id);
            if (assistant == NULL) return COG_ERROR_NOT_FOUND;
            const cog_message_t *message = cog_node_data(assistant)->message;
            if (message->role != COG_MESSAGE_ROLE_ASSISTANT || ordinal > message->tool_call_count)
                return COG_ERROR_NOT_FOUND;
            call_id = message->tool_calls[(size_t)ordinal - 1U].id;
        }
        return cog_cli_app_reconcile(app, id, call_id, state, evidence, strlen(evidence));
    }
    if (recovery) return COG_ERROR_INVALID_STATE;
    if (strcmp(text, "/sessions") == 0) return list_sessions(app);
    if (strncmp(text, "/fork ", 6U) == 0) {
        uint64_t id;
        if (!decimal(text + 6, &id)) return COG_ERROR_INVALID_ARG;
        return cog_cli_app_fork(app, id);
    }
    if (strcmp(text, "/usage") == 0) {
        bool found = false;
        for (cog_session_node_t *node = cog_session_active(app->session); node != NULL; node = cog_node_parent(node))
            if (cog_node_data(node)->message->role == COG_MESSAGE_ROLE_ASSISTANT) {
                found = true; cog_result_t rc = cog_cli_usage(app, node, app->out, true);
                if (rc != COG_OK) return rc;
            }
        return found ? COG_OK : COG_ERROR_NOT_FOUND;
    }
    *handled = text[0] == '/' && strcmp(text, "/quit") != 0 && strcmp(text, "/exit") != 0 && strcmp(text, "/cancel") != 0;
    return *handled ? COG_ERROR_INVALID_ARG : COG_OK;
}
static cog_result_t unresolved(cog_cli_app_t *app)
{
    cog_session_snapshot_t snapshot;
    cog_result_t rc = cog_session_snapshot_view(app->session, &snapshot);
    if (rc != COG_OK) return rc;
    for (size_t i = 0U; i < snapshot.ledger_count; i++)
        if (snapshot.ledger[i].state == COG_TOOL_EXECUTION_INDETERMINATE ||
            snapshot.ledger[i].state == COG_TOOL_EXECUTION_STARTED) return COG_ERROR_INDETERMINATE;
    return COG_OK;
}
cog_result_t cog_cli_metadata_run(cog_cli_app_t *app)
{
    if (app->opts.session_action == COG_CLI_SESSION_LIST) {
        if (atomic_load_explicit(&app->stop_requested, memory_order_acquire)) return COG_OK;
        return list_sessions(app);
    }
    if (!app->opts.interactive) return COG_ERROR_INDETERMINATE;
    if (app->recovery_write_failed) return COG_ERROR_INVALID_STATE;
    if (fputs("Recovery is offline. Your --fenced assertion does not stop old processes.\n"
        "Use /tools, /history, /reconcile NODE CALL OUTCOME EVIDENCE, or /quit.\n"
        "For escaped IDs use /reconcile-index NODE ORDINAL OUTCOME EVIDENCE (1-based original call order).\n", stderr) < 0 ||
        fflush(stderr) != 0) return COG_ERROR_IO;
    cog_result_t rc = COG_OK;
    while (!atomic_load_explicit(&app->stop_requested, memory_order_acquire)) {
        struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {app->signal_fd, POLLIN, 0}};
        int ready = poll(fds, app->signal_fd < 0 ? 1U : 2U, 10);
        if (ready < 0) { if (errno == EINTR) continue; rc = COG_ERROR_IO; break; }
        if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0 ||
            (app->signal_fd >= 0 && (fds[1].revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)) {
            rc = COG_ERROR_IO; break;
        }
        if (app->signal_fd >= 0 && (fds[1].revents & POLLIN) != 0) {
            struct signalfd_siginfo info;
            ssize_t got = read(app->signal_fd, &info, sizeof(info));
            if (got == (ssize_t)sizeof(info)) break;
            if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            rc = COG_ERROR_IO; break;
        }
        if ((fds[0].revents & (POLLIN | POLLHUP)) == 0) continue;
        char byte; ssize_t n = read(STDIN_FILENO, &byte, 1U);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; rc = COG_ERROR_IO; break; }
        if (byte != '\n') {
            if (app->line_length + 1U >= sizeof(app->line)) { rc = COG_ERROR_LIMIT; break; }
            app->line[app->line_length++] = byte; continue;
        }
        while (app->line_length != 0U && app->line[app->line_length - 1U] == '\r') app->line_length--;
        app->line[app->line_length] = '\0';
        bool valid = cog_cli_text_valid(app->line, app->line_length);
        app->line_length = 0U;
        if (!valid) { rc = COG_ERROR_INVALID_ARG; break; }
        if (strcmp(app->line, "/quit") == 0 || strcmp(app->line, "/exit") == 0) break;
        bool handled;
        rc = cog_cli_session_command(app, app->line, &handled);
        if (rc == COG_ERROR_IO || rc == COG_ERROR_INDETERMINATE || app->recovery_write_failed) break;
        if (rc != COG_OK && fprintf(stderr, "cog-cli: recovery command refused: %s\n", cog_cli_result_name(rc)) < 0) { rc = COG_ERROR_IO; break; }
        rc = COG_OK;
    }
    atomic_store_explicit(&app->stop_requested, false, memory_order_release);
    return rc == COG_OK ? unresolved(app) : rc;
}
