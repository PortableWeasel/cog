/** @file app_reload.c @brief Bounded copied generations and idle publication. */
#include "app_internal.h"
#include <cog/config.h>
#include <inttypes.h>
#include <string.h>

static const char *const control_keys[] = {"models.default",
    "providers.openrouter.base-url", "providers.openrouter.credential-ref"};

static cog_result_t control_validate(const cog_config_txn_t *txn, const char *prefix)
{
    static const struct { const char *path; const char *first; const char *second; } shape[] = {
        {"", "models", "providers"}, {"models", "default", NULL},
        {"providers", "openrouter", NULL},
        {"providers.openrouter", "base-url", "credential-ref"},
        {"models.default", NULL, NULL}, {"providers.openrouter.base-url", NULL, NULL},
        {"providers.openrouter.credential-ref", NULL, NULL}};
    (void)prefix;
    for (size_t i = 0U; i < sizeof(shape) / sizeof(shape[0]); i++) {
        if (i > 0U && i < 4U) {
            const char *value = NULL;
            if (cog_config_get(txn, shape[i].path, &value) == COG_OK)
                return COG_ERROR_INVALID_ARG; /* Branches carry no values. */
        }
        const char *children[2]; size_t count = 0U;
        cog_result_t rc = cog_config_list(txn, shape[i].path, children, 2U, &count);
        if (rc == COG_ERROR_NOT_FOUND) continue;
        if (rc != COG_OK) return COG_ERROR_INVALID_ARG;
        for (size_t j = 0U; j < count; j++)
            if ((shape[i].first == NULL || strcmp(children[j], shape[i].first) != 0) &&
                (shape[i].second == NULL || strcmp(children[j], shape[i].second) != 0))
                return COG_ERROR_INVALID_ARG;
    }
    for (size_t i = 0U; i < 3U; i++) {
        const char *value = NULL;
        cog_result_t rc = cog_config_get(txn, control_keys[i], &value);
        if (rc == COG_ERROR_NOT_FOUND && i != 0U) continue;
        if (rc != COG_OK || value == NULL || *value == '\0') return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}
static cog_result_t stage(cog_config_txn_t *txn, const cog_cli_options_t *current,
    const cog_cli_reload_options_t *replacement)
{
    const char *values[] = {replacement->model_id != NULL ? replacement->model_id : current->model_id,
        replacement->base_url != NULL ? replacement->base_url : current->base_url,
        replacement->credential_ref != NULL ? replacement->credential_ref : current->credential_ref};
    for (size_t i = 0U; i < 3U; i++) if (values[i] != NULL) {
        cog_result_t rc = cog_config_set(txn, control_keys[i], values[i]);
        if (rc != COG_OK) return rc;
    }
    return COG_OK;
}
cog_result_t cog_cli_reload_init(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (cog_cli_metadata(app)) return COG_OK;
    const cog_config_limits_t limits = {64U, 65536U, CLI_RELOAD_ATTEMPTS + 1U, 1U};
    cog_result_t rc = cog_config_tree_new_ex(&app->reload_tree, &limits);
    if (rc != COG_OK) return rc;
    rc = cog_config_register_validator(app->reload_tree, "", control_validate);
    cog_config_txn_t *txn = NULL;
    if (rc == COG_OK) rc = cog_config_begin(app->reload_tree, &txn);
    if (rc == COG_OK) rc = stage(txn, &app->opts, &(cog_cli_reload_options_t){0});
    if (rc == COG_OK) rc = cog_config_commit(txn);
    cog_config_rollback(txn); /* Finalized transactions explicitly permit this. */
    return rc;
}
static void cleanup_error(cog_cli_app_t *app, cog_result_t rc)
{
    if (rc != COG_OK) {
        if (app->reload_cleanup_error == COG_OK) app->reload_cleanup_error = rc;
        app->shutdown = true;
    }
}
static void dispose(cog_cli_app_t *app, cog_provider_t **provider,
    cog_cli_configuration_t **configuration)
{
    cog_provider_t *owned_provider = *provider;
    cog_cli_configuration_t *owned_configuration = *configuration;
    *provider = NULL; *configuration = NULL;
    cog_result_t rc = cog_provider_disconnect(owned_provider);
    /* Disconnect consumes even on cleanup error and releases all config borrows. */
    cog_cli_configuration_free(owned_configuration);
    cleanup_error(app, rc);
}
cog_result_t cog_cli_reload_free(cog_cli_app_t *app)
{
    if (app == NULL) return COG_OK;
    dispose(app, &app->future_provider, &app->future_configuration);
    dispose(app, &app->retiring_provider, &app->retiring_configuration);
    dispose(app, &app->provider, &app->configuration);
    cog_config_tree_free(app->reload_tree); app->reload_tree = NULL;
    return app->reload_cleanup_error;
}
static cog_result_t candidate_provider(cog_cli_app_t *app, cog_cli_configuration_t *configuration,
    cog_provider_t **provider)
{
    const cog_provider_limits_t limits = {
        .request_bytes = app->opts.max_context_bytes * 4U,
        .parser_bytes = app->opts.max_context_bytes * 16U,
        .writer_bytes = app->opts.max_context_bytes * 8U, .max_json_depth = 64U,
        .max_frame_bytes = app->opts.max_context_bytes, .retained_delta_bytes = app->delta_capacity,
        .retained_delta_count = 16U, .max_text_bytes = app->opts.max_context_bytes,
        .max_tool_calls = CLI_CALLS, .max_tool_id_bytes = CLI_ID_BYTES,
        .max_tool_name_bytes = CLI_ID_BYTES, .max_tool_arguments_bytes = CLI_ARGUMENT_BYTES,
        .max_opaque_bytes = app->opts.max_context_bytes};
    cog_result_t rc = cog_cli_configuration_bind(configuration, &cog_openrouter_provider_vtable,
        &cog_mock_provider_vtable, &app->script, &limits);
    if (rc != COG_OK) return rc;
    if (app->opts.mock_deltas != NULL)
        return cog_mock_provider_new_deltas(app->opts.mock_deltas, app->opts.mock_delta_count, provider);
    return cog_provider_connect(cog_cli_configuration_endpoint(configuration), provider);
}
static cog_result_t preflight(cog_cli_app_t *app, cog_cli_configuration_t *configuration,
    cog_provider_t *provider, uint64_t generation)
{
    cog_session_node_t *selected = app->fork_parent != NULL ? app->fork_parent : cog_session_active(app->session);
    size_t count = 0U;
    for (cog_session_node_t *node = selected; node != NULL; node = cog_node_parent(node))
        if (++count > CLI_NODES) return COG_ERROR_LIMIT;
    size_t at = count;
    for (cog_session_node_t *node = selected; node != NULL; node = cog_node_parent(node))
        app->history[--at] = *cog_node_data(node)->message;
    cog_result_t rc = cog_cli_context_check(app, count);
    if (rc != COG_OK) return rc;
    size_t missing = 0U;
    for (size_t i = 0U; i < count; i++) {
        const cog_message_t *message = &app->history[i];
        if (message->role == COG_MESSAGE_ROLE_ASSISTANT) missing = message->tool_call_count;
        else if (message->role == COG_MESSAGE_ROLE_TOOL && missing != 0U) missing--;
    }
    const cog_cli_options_t *options = cog_cli_configuration_options(configuration);
    cog_provider_request_t request = {.model_id = options->model_id, .messages = app->history,
        .message_count = count, .stream = true, .max_tokens = options->max_output_tokens,
        .configuration_generation = generation};
    if (app->workspace != NULL) { request.tools_json = app->tools_json; request.tools_json_len = app->tools_json_len; }
    return cog_provider_preflight(provider, &request, missing != 0U ?
        COG_PROVIDER_PREFLIGHT_PENDING_TOOLS : COG_PROVIDER_PREFLIGHT_COMPLETE);
}
cog_result_t cog_cli_app_reload(cog_cli_app_t *app,
    const cog_cli_reload_options_t *replacement, uint64_t *out_generation)
{
    if (app == NULL || replacement == NULL || out_generation == NULL) return COG_ERROR_NULL_PTR;
    if ((replacement->model_id == NULL && replacement->base_url == NULL && replacement->credential_ref == NULL) ||
        (replacement->credential_ref == NULL && (replacement->credential_read != NULL || replacement->credential_arg != NULL)))
        return COG_ERROR_INVALID_ARG;
    if (app->opts.backend == COG_CLI_BACKEND_MOCK &&
        (replacement->base_url != NULL || replacement->credential_ref != NULL)) return COG_ERROR_UNSUPPORTED;
    if (cog_cli_metadata(app) || app->shutdown || app->cancelling || app->reload_in_progress ||
        atomic_load_explicit(&app->stop_requested, memory_order_acquire)) return COG_ERROR_INVALID_STATE;
    if (app->recovery || app->assistant_pending || app->terminal_result_pending || app->reload_cleanup_error != COG_OK)
        return COG_ERROR_INDETERMINATE;
    if (app->future_configuration != NULL || app->retiring_configuration != NULL ||
        app->reload_notice_pending || app->reload_notice_queued) return COG_ERROR_AGAIN;
    if (app->reload_attempts == CLI_RELOAD_ATTEMPTS) return COG_ERROR_LIMIT;
    app->reload_in_progress = true;
    uint64_t generation = (uint64_t)++app->reload_attempts + 1U;
    cog_config_txn_t *txn = NULL;
    cog_cli_configuration_t *configuration = NULL; cog_provider_t *provider = NULL;
    cog_result_t rc = cog_config_begin(app->reload_tree, &txn);
    if (rc == COG_OK) rc = stage(txn, &app->opts, replacement);
    if (rc == COG_OK) rc = cog_cli_configuration_reload_new(&configuration, app->configuration, replacement, generation);
    if (rc == COG_OK) rc = candidate_provider(app, configuration, &provider);
    if (rc == COG_OK) rc = preflight(app, configuration, provider, generation);
    if (rc == COG_OK) rc = cog_config_commit(txn);
    cog_config_rollback(txn);
    if (rc == COG_OK) {
        app->future_configuration = configuration; app->future_provider = provider;
        app->future_configuration_generation = generation;
        app->reload_notice_generation = generation;
        app->reload_notice_pending = true; app->reload_notice_queued = false;
        *out_generation = generation;
    } else dispose(app, &provider, &configuration);
    app->reload_in_progress = false;
    return rc;
}
cog_result_t cog_cli_reload_activate(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->future_configuration == NULL) return COG_OK;
    if (app->stdin_eof) {
        bool admitted = false;
        for (size_t i = 0U; i < CLI_INPUTS; i++)
            if (app->inputs[i].used && (!app->continuation_required || app->inputs[i].continuation_authorized))
                admitted = true;
        if (!admitted) return COG_ERROR_AGAIN;
    }
    if (app->core == NULL || app->active || app->provider_active || app->delta_ready || app->event_queued ||
        app->execution != NULL || app->terminal_result_pending || app->assistant_pending || app->resuming_policy ||
        app->cancelling || app->shutdown || app->recovery || app->reload_cleanup_error != COG_OK ||
        atomic_load_explicit(&app->stop_requested, memory_order_acquire)) return COG_ERROR_AGAIN;
    app->retiring_configuration = app->configuration; app->retiring_provider = app->provider;
    app->configuration = app->future_configuration; app->provider = app->future_provider;
    app->configuration_generation = app->future_configuration_generation;
    app->future_configuration = NULL; app->future_provider = NULL; app->future_configuration_generation = 0U;
    app->opts = *cog_cli_configuration_options(app->configuration);
    (void)cog_core_set_provider(app->core, app->provider); /* Valid core: pointer-only, infallible. */
    dispose(app, &app->retiring_provider, &app->retiring_configuration);
    return app->reload_cleanup_error;
}
cog_result_t cog_cli_reload_service(cog_cli_app_t *app)
{
    if (app == NULL) return COG_ERROR_NULL_PTR;
    if (app->reload_cleanup_error != COG_OK) return app->reload_cleanup_error;
    if (!app->reload_notice_pending || app->reload_notice_queued || app->shutdown) return COG_OK;
    const cog_event_t event = {.id = COG_EVENT_CONFIG_CHANGED, .kind = COG_EVENT_CONFIG_CHANGED,
        .source_id = app->reload_notice_generation};
    cog_result_t rc = cog_core_enqueue(app->core, &event);
    if (rc == COG_OK) app->reload_notice_queued = true;
    return rc == COG_ERROR_OVERFLOW ? COG_OK : rc;
}
cog_result_t cog_cli_reload_command(cog_cli_app_t *app, const char *text, bool *handled)
{
    if (app == NULL || text == NULL || handled == NULL) return COG_ERROR_NULL_PTR;
    *handled = strncmp(text, "/reload", 7U) == 0;
    if (!*handled) return COG_OK;
    cog_cli_reload_options_t options = {0};
    static const struct { const char *name; size_t length; unsigned field; } commands[] = {
        {"/reload model ", 14U, 0U}, {"/reload base-url ", 17U, 1U}, {"/reload credential-ref ", 23U, 2U}};
    for (size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strncmp(text, commands[i].name, commands[i].length) == 0) {
            const char *value = text + commands[i].length;
            if (commands[i].field == 0U) options.model_id = value;
            else if (commands[i].field == 1U) options.base_url = value;
            else options.credential_ref = value;
            break;
        }
    uint64_t generation = 0U;
    cog_result_t rc = cog_cli_app_reload(app, &options, &generation);
    if (app->reload_cleanup_error != COG_OK) return app->reload_cleanup_error;
    int written = rc == COG_OK ? fprintf(stderr, "cog-cli: configuration published generation=%" PRIu64 "\n", generation) :
        fprintf(stderr, "cog-cli: reload refused: %s\n", cog_cli_result_name(rc));
    return written < 0 || fflush(stderr) != 0 ? COG_ERROR_IO : COG_OK;
}
